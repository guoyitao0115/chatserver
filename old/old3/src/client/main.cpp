
#include "json.hpp"
#include <iostream>
#include <thread>
#include <string>
#include <vector>
#include <chrono>
#include <ctime>
#include <unordered_map>
#include <functional>
#include <atomic>
#include <mutex>
#include <map>
using namespace std;
using json = nlohmann::json;

#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <semaphore.h>

#include "group.hpp"
#include "user.hpp"
#include "public.hpp"

// --- 帧协议辅助 ---
static int sendFrame(int fd, const string &payload)
{
    uint32_t netLen = htonl((uint32_t)payload.size());
    string frame;
    frame.reserve(4 + payload.size());
    frame.append((const char *)&netLen, 4);
    frame.append(payload);
    size_t total = frame.size(), sent = 0;
    while (sent < total) {
        ssize_t n = ::send(fd, frame.data()+sent, total-sent, 0);
        if (n <= 0) return -1;
        sent += n;
    }
    return (int)sent;
}
static bool recvAll(int fd, char *buf, size_t n)
{
    size_t got=0;
    while(got<n){
        ssize_t r=::recv(fd,buf+got,n-got,0);
        if(r<=0) return false;
        got+=r;
    }
    return true;
}
static string recvFrame(int fd)
{
    char hdr[4]={0};
    if(!recvAll(fd,hdr,4)) return "";
    uint32_t nl=0; memcpy(&nl,hdr,4);
    uint32_t len=ntohl(nl);
    if(len==0||len>4*1024*1024) return "";
    string p(len,'\0');
    if(!recvAll(fd,&p[0],len)) return "";
    return p;
}

// --- message_id 生成 ---
static atomic<int> g_msgSeq{0};
static int g_myUserId=0;
static string generateMsgId()
{
    auto ms=chrono::duration_cast<chrono::milliseconds>(
        chrono::system_clock::now().time_since_epoch()).count();
    return to_string(g_myUserId)+"_"+to_string(ms)+"_"+to_string(++g_msgSeq);
}

// --- pending map (一级ACK) ---
struct PendingEntry {
    string payload;
    chrono::steady_clock::time_point lastSent;
    int retryCount;
};
static mutex g_pendingMutex;
static map<string,PendingEntry> g_pendingMap;
static int g_clientFd=-1;

static void addPending(const string &id, const string &payload)
{
    lock_guard<mutex> lk(g_pendingMutex);
    g_pendingMap[id]={payload,chrono::steady_clock::now(),0};
}
static void removePending(const string &id)
{
    lock_guard<mutex> lk(g_pendingMutex);
    g_pendingMap.erase(id);
}

// --- 重试线程: 指数退避，最多3次 ---
static const int MAX_RETRY=3;
static const int RETRY_BASE_MS=2000;
static void retryThreadHandler()
{
    while(true){
        this_thread::sleep_for(chrono::milliseconds(500));
        if(g_clientFd==-1) continue;
        // 先在锁内收集需重发的条目，不做IO
        vector<pair<string,string>> toSend; // {msgId, payload}
        vector<string> toErase;
        {
            lock_guard<mutex> lk(g_pendingMutex);
            auto now=chrono::steady_clock::now();
            for(auto &kv:g_pendingMap){
                PendingEntry &e=kv.second;
                int waitMs=RETRY_BASE_MS*(1<<e.retryCount);
                auto elapsed=chrono::duration_cast<chrono::milliseconds>(now-e.lastSent).count();
                if(elapsed<waitMs) continue;
                if(e.retryCount>=MAX_RETRY){
                    cerr<<"[retry] "<<kv.first<<" FAILED after "<<MAX_RETRY<<" retries, may be lost!"<<endl;
                    toErase.push_back(kv.first);
                    continue;
                }
                e.retryCount++; e.lastSent=now;
                toSend.push_back({kv.first, e.payload});
            }
            for(auto &id:toErase) g_pendingMap.erase(id);
        } // 释放锁
        // 锁外执行IO发送，避免持锁期间阻塞
        for(auto &p:toSend){
            if(sendFrame(g_clientFd,p.second)==-1){
                cerr<<"[retry] send failed, msgId="<<p.first<<endl;
                removePending(p.first);
            } else {
                cerr<<"[retry] retrying "<<p.first<<endl;
            }
        }
    }
}

// --- 全局状态 ---
User g_currentUser;
vector<User> g_currentUserFriendList;
vector<Group> g_currentUserGroupList;
bool isMainMenuRunning=false;
sem_t rwsem;
atomic_bool g_isLoginSuccess{false};

void readTaskHandler(int);
string getCurrentTime();
void mainMenu(int);
void showCurrentUserData();

int main(int argc, char **argv)
{
    if(argc<3){cerr<<"usage: ./ChatClient ip port"<<endl;exit(-1);}
    char *ip=argv[1]; uint16_t port=atoi(argv[2]);
    int clientfd=socket(AF_INET,SOCK_STREAM,0);
    if(-1==clientfd){cerr<<"socket error"<<endl;exit(-1);}
    sockaddr_in server; memset(&server,0,sizeof(server));
    server.sin_family=AF_INET; server.sin_port=htons(port);
    server.sin_addr.s_addr=inet_addr(ip);
    if(-1==connect(clientfd,(sockaddr*)&server,sizeof(server)))
    {cerr<<"connect error"<<endl;close(clientfd);exit(-1);}
    g_clientFd=clientfd;
    sem_init(&rwsem,0,0);
    thread readTask(readTaskHandler,clientfd); readTask.detach();
    thread retryTask(retryThreadHandler);      retryTask.detach();
    for(;;){
        cout<<"========================"<<endl;
        cout<<"1. login"<<endl;
        cout<<"2. register"<<endl;
        cout<<"3. quit"<<endl;
        cout<<"========================"<<endl;
        cout<<"choice:"; int choice=0; cin>>choice; cin.get();
        switch(choice){
        case 1:{
            int id=0; char pwd[50]={0};
            cout<<"userid:"; cin>>id; cin.get();
            cout<<"userpassword:"; cin.getline(pwd,50);
            json js; js["msgid"]=LOGIN_MSG; js["id"]=id; js["password"]=pwd;
            g_isLoginSuccess=false;
            if(sendFrame(clientfd,js.dump())==-1) cerr<<"send login error"<<endl;
            sem_wait(&rwsem);
            if(g_isLoginSuccess){isMainMenuRunning=true;mainMenu(clientfd);}
        } break;
        case 2:{
            char name[50]={0},pwd[50]={0};
            cout<<"username:"; cin.getline(name,50);
            cout<<"userpassword:"; cin.getline(pwd,50);
            json js; js["msgid"]=REG_MSG; js["name"]=name; js["password"]=pwd;
            if(sendFrame(clientfd,js.dump())==-1) cerr<<"send reg error"<<endl;
            sem_wait(&rwsem);
        } break;
        case 3: close(clientfd);sem_destroy(&rwsem);exit(0);
        default: cerr<<"invalid input"<<endl; break;
        }
    }
    return 0;
}

void doRegResponse(json &js){
    if(0!=js["errno"].get<int>()) cerr<<"register error: name already exist"<<endl;
    else cout<<"register success, userid="<<js["id"]<<", do not forget it!"<<endl;
}

void doLoginResponse(json &rjs){
    if(0!=rjs["errno"].get<int>()){
        cerr<<rjs["errmsg"]<<endl; g_isLoginSuccess=false; return;
    }
    g_currentUser.setId(rjs["id"].get<int>());
    g_currentUser.setName(rjs["name"]);
    g_myUserId=g_currentUser.getId();
    if(rjs.contains("friends")){
        g_currentUserFriendList.clear();
        for(auto &s:rjs["friends"].get<vector<string>>()){
            json fj=json::parse(s); User u;
            u.setId(fj["id"].get<int>()); u.setName(fj["name"]); u.setState(fj["state"]);
            g_currentUserFriendList.push_back(u);
        }
    }
    if(rjs.contains("groups")){
        g_currentUserGroupList.clear();
        for(auto &gs:rjs["groups"].get<vector<string>>()){
            json gj=json::parse(gs); Group g;
            g.setId(gj["id"].get<int>()); g.setName(gj["groupname"]); g.setDesc(gj["groupdesc"]);
            for(auto &us:gj["users"].get<vector<string>>()){
                json uj=json::parse(us); GroupUser u;
                u.setId(uj["id"].get<int>()); u.setName(uj["name"]);
                u.setState(uj["state"]); u.setRole(uj["role"]);
                g.getUsers().push_back(u);
            }
            g_currentUserGroupList.push_back(g);
        }
    }
    showCurrentUserData();
    if(rjs.contains("offlinemsg")){
        for(auto &s:rjs["offlinemsg"].get<vector<string>>()){
            json oj=json::parse(s);
            if(ONE_CHAT_MSG==oj["msgid"].get<int>())
                cout<<oj["time"].get<string>()<<" ["<<oj["id"]<<"]"<<oj["name"].get<string>()
                    <<" said: "<<oj["msg"].get<string>()<<endl;
            else
                cout<<"群消息["<<oj["groupid"]<<"]:"<<oj["time"].get<string>()
                    <<" ["<<oj["id"]<<"]"<<oj["name"].get<string>()
                    <<" said: "<<oj["msg"].get<string>()<<endl;
        }
    }
    g_isLoginSuccess=true;
}

void readTaskHandler(int clientfd)
{
    for(;;){
        string payload=recvFrame(clientfd);
        if(payload.empty()){close(clientfd);exit(-1);}
        json js;
        try{js=json::parse(payload);}
        catch(...){cerr<<"[recv] JSON parse error"<<endl;continue;}
        int msgtype=js["msgid"].get<int>();

        // 一级ACK：服务端对发送端的回执
        if(MSG_ACK==msgtype){
            string mid=js.contains("message_id")?js["message_id"].get<string>():"";
            int st=js.contains("ack_state")?js["ack_state"].get<int>():-1;
            if(st==ACK_OK){
                removePending(mid);
            } else if(st==ACK_DEDUP){
                removePending(mid);
                cerr<<"[ack] message "<<mid<<" deduped by server"<<endl;
            } else {
                cerr<<"[ack] message "<<mid<<" failed, ack_state="<<st<<endl;
            }
            continue;
        }

        // 二级ACK：本机作为接收端，收到服务端转来的原发送端送达确认
        if(MSG_DELIVER_ACK==msgtype){
            string mid=js.contains("message_id")?js["message_id"].get<string>():"";
            cout<<"[delivered] message_id="<<mid<<" confirmed delivered to receiver"<<endl;
            continue;
        }

        if(ONE_CHAT_MSG==msgtype){
            string mid=js.contains("message_id")?js["message_id"].get<string>():"";
            cout<<js["time"].get<string>()<<" ["<<js["id"]<<"]"<<js["name"].get<string>()
                <<" said: "<<js["msg"].get<string>()<<endl;
            // 发送二级ACK：通知服务端"我已展示该消息"
            if(!mid.empty()){
                json ack;
                ack["msgid"]=MSG_DELIVER_ACK;
                ack["message_id"]=mid;
                ack["fromid"]=js["id"].get<int>();   // 原发送人
                ack["toid"]=g_currentUser.getId();    // 我（接收人）
                sendFrame(clientfd,ack.dump());
            }
            continue;
        }

        if(GROUP_CHAT_MSG==msgtype){
            string mid=js.contains("message_id")?js["message_id"].get<string>():"";
            cout<<"群消息["<<js["groupid"]<<"]:"<<js["time"].get<string>()
                <<" ["<<js["id"]<<"]"<<js["name"].get<string>()
                <<" said: "<<js["msg"].get<string>()<<endl;
            // 群聊也发二级ACK
            if(!mid.empty()){
                json ack;
                ack["msgid"]=MSG_DELIVER_ACK;
                ack["message_id"]=mid;
                ack["fromid"]=js["id"].get<int>();
                ack["toid"]=g_currentUser.getId();
                sendFrame(clientfd,ack.dump());
            }
            continue;
        }

        if(LOGIN_MSG_ACK==msgtype){doLoginResponse(js);sem_post(&rwsem);continue;}
        if(REG_MSG_ACK==msgtype){doRegResponse(js);sem_post(&rwsem);continue;}
    }
}

void showCurrentUserData()
{
    cout<<"======================login user======================"<<endl;
    cout<<"current login user => id:"<<g_currentUser.getId()<<" name:"<<g_currentUser.getName()<<endl;
    cout<<"----------------------friend list---------------------"<<endl;
    for(auto &u:g_currentUserFriendList)
        cout<<u.getId()<<" "<<u.getName()<<" "<<u.getState()<<endl;
    cout<<"----------------------group list----------------------"<<endl;
    for(auto &g:g_currentUserGroupList){
        cout<<g.getId()<<" "<<g.getName()<<" "<<g.getDesc()<<endl;
        for(auto &u:g.getUsers())
            cout<<u.getId()<<" "<<u.getName()<<" "<<u.getState()<<" "<<u.getRole()<<endl;
    }
    cout<<"======================================================"<<endl;
}

string getCurrentTime()
{
    auto tt=chrono::system_clock::to_time_t(chrono::system_clock::now());
    struct tm *p=localtime(&tt); char d[60]={0};
    sprintf(d,"%d-%02d-%02d %02d:%02d:%02d",(int)p->tm_year+1900,(int)p->tm_mon+1,(int)p->tm_mday,(int)p->tm_hour,(int)p->tm_min,(int)p->tm_sec);
    return string(d);
}

void help(int fd=0, string str="");
void chat(int,string);
void addfriend(int,string);
void creategroup(int,string);
void addgroup(int,string);
void groupchat(int,string);
void loginout(int,string);

unordered_map<string,string> commandMap={
    {"help",   "显示所有支持的命令，格式help"},
    {"chat",   "一对一聊天，格式chat:friendid:message"},
    {"addfriend",  "添加好友，格式addfriend:friendid"},
    {"creategroup","创建群组，格式creategroup:groupname:groupdesc"},
    {"addgroup",   "加入群组，格式addgroup:groupid"},
    {"groupchat",  "群聊，格式groupchat:groupid:message"},
    {"loginout",   "注销，格式loginout"}};

unordered_map<string,function<void(int,string)>> commandHandlerMap={
    {"help",help},{"chat",chat},{"addfriend",addfriend},
    {"creategroup",creategroup},{"addgroup",addgroup},
    {"groupchat",groupchat},{"loginout",loginout}};

void mainMenu(int clientfd)
{
    help();
    char buf[1024]={0};
    while(isMainMenuRunning){
        cin.getline(buf,1024);
        string cb(buf);
        string cmd; int idx=cb.find(":");
        cmd=(idx==-1)?cb:cb.substr(0,idx);
        auto it=commandHandlerMap.find(cmd);
        if(it==commandHandlerMap.end()){cerr<<"invalid command!"<<endl;continue;}
        it->second(clientfd,cb.substr(idx+1,cb.size()-idx));
    }
}

void help(int,string){
    cout<<"show command list >>>"<<endl;
    for(auto &p:commandMap) cout<<p.first<<" : "<<p.second<<endl;
    cout<<endl;
}

void addfriend(int clientfd,string str){
    json js; js["msgid"]=ADD_FRIEND_MSG;
    js["id"]=g_currentUser.getId(); js["friendid"]=atoi(str.c_str());
    if(sendFrame(clientfd,js.dump())==-1) cerr<<"send addfriend error"<<endl;
}

void chat(int clientfd,string str){
    int idx=str.find(":");
    if(idx==-1){cerr<<"chat command invalid!"<<endl;return;}
    int friendid=atoi(str.substr(0,idx).c_str());
    string msg=str.substr(idx+1,str.size()-idx);
    json js;
    js["msgid"]=ONE_CHAT_MSG;
    js["id"]=g_currentUser.getId();
    js["name"]=g_currentUser.getName();
    js["toid"]=friendid;
    js["msg"]=msg;
    js["time"]=getCurrentTime();
    string mid=generateMsgId();
    js["message_id"]=mid;
    string payload=js.dump();
    addPending(mid,payload);
    if(sendFrame(clientfd,payload)==-1){
        cerr<<"send chat error"<<endl;
        removePending(mid);
    }
}

void creategroup(int clientfd,string str){
    int idx=str.find(":");
    if(idx==-1){cerr<<"creategroup command invalid!"<<endl;return;}
    json js;
    js["msgid"]=CREATE_GROUP_MSG;
    js["id"]=g_currentUser.getId();
    js["groupname"]=str.substr(0,idx);
    js["groupdesc"]=str.substr(idx+1,str.size()-idx);
    if(sendFrame(clientfd,js.dump())==-1) cerr<<"send creategroup error"<<endl;
}

void addgroup(int clientfd,string str){
    json js; js["msgid"]=ADD_GROUP_MSG;
    js["id"]=g_currentUser.getId(); js["groupid"]=atoi(str.c_str());
    if(sendFrame(clientfd,js.dump())==-1) cerr<<"send addgroup error"<<endl;
}

void groupchat(int clientfd,string str){
    int idx=str.find(":");
    if(idx==-1){cerr<<"groupchat command invalid!"<<endl;return;}
    int groupid=atoi(str.substr(0,idx).c_str());
    string msg=str.substr(idx+1,str.size()-idx);
    json js;
    js["msgid"]=GROUP_CHAT_MSG;
    js["id"]=g_currentUser.getId();
    js["name"]=g_currentUser.getName();
    js["groupid"]=groupid;
    js["msg"]=msg;
    js["time"]=getCurrentTime();
    string mid=generateMsgId();
    js["message_id"]=mid;
    string payload=js.dump();
    addPending(mid,payload);
    if(sendFrame(clientfd,payload)==-1){
        cerr<<"send groupchat error"<<endl;
        removePending(mid);
    }
}

void loginout(int clientfd,string){
    json js; js["msgid"]=LOGINOUT_MSG; js["id"]=g_currentUser.getId();
    if(sendFrame(clientfd,js.dump())==-1) cerr<<"send loginout error"<<endl;
    else isMainMenuRunning=false;
}

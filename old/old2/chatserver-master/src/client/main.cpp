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
#include <algorithm>
#include <cstdint>
#include <tuple>
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

// ============================================================
// 帧协议辅助函数（客户端版）
// 帧结构：[4字节 payload_len（网络字节序大端）][payload(JSON字符串)]
// ============================================================

// 将 JSON payload 打包成帧发送，返回发送字节数，-1表示失败
static int sendFrame(int fd, const string &payload)
{
    uint32_t netLen = htonl(static_cast<uint32_t>(payload.size()));
    string frame;
    frame.reserve(4 + payload.size());
    frame.append(reinterpret_cast<const char *>(&netLen), 4);
    frame.append(payload);
    size_t total = frame.size(), sent = 0;
    while (sent < total)
    {
        ssize_t n = ::send(fd, frame.data() + sent, total - sent, 0);
        if (n <= 0) return -1;
        sent += n;
    }
    return (int)sent;
}

// 精确读取 n 字节
static bool recvAll(int fd, char *buf, size_t n)
{
    size_t got = 0;
    while (got < n)
    {
        ssize_t r = ::recv(fd, buf + got, n - got, 0);
        if (r <= 0) return false;
        got += r;
    }
    return true;
}

// 接收一个完整帧，返回 payload 字符串，失败返回空串
static string recvFrame(int fd)
{
    char header[4] = {0};
    if (!recvAll(fd, header, 4)) return "";
    uint32_t netLen = 0;
    memcpy(&netLen, header, 4);
    uint32_t payloadLen = ntohl(netLen);
    if (payloadLen == 0 || payloadLen > 4 * 1024 * 1024) return "";
    string payload(payloadLen, '\0');
    if (!recvAll(fd, &payload[0], payloadLen)) return "";
    return payload;
}

// ============================================================
// 消息ID生成与 pending 管理
// 每条业务消息携带唯一 message_id，收到服务端 MSG_ACK 后移出 pending
// ============================================================
static int g_myUserId = 0;

// Snowflake 64位ID生成器：
// 1位符号位(固定0) + 41位时间戳差值 + 10位workerId + 12位毫秒内序列
class SnowflakeIdGenerator
{
public:
    explicit SnowflakeIdGenerator(uint16_t workerId = 1)
        : _workerId(workerId & WORKER_ID_MASK) {}

    void setWorkerId(uint16_t workerId)
    {
        lock_guard<mutex> lock(_mtx);
        _workerId = workerId & WORKER_ID_MASK;
    }

    uint64_t nextId()
    {
        lock_guard<mutex> lock(_mtx);

        uint64_t ts = nowMs();
        if (ts < _lastTs)
        {
            // 时钟回拨保护：回拨窗口内强制使用 lastTs，保证单调不倒退
            ts = _lastTs;
        }

        if (ts == _lastTs)
        {
            _seq = (_seq + 1) & SEQ_MASK;
            if (_seq == 0)
            {
                ts = waitNextMs(_lastTs);
            }
        }
        else
        {
            _seq = 0;
        }

        _lastTs = ts;

        uint64_t id = ((ts - EPOCH_MS) << TIMESTAMP_SHIFT)
                    | (static_cast<uint64_t>(_workerId) << WORKER_ID_SHIFT)
                    | _seq;
        return id;
    }

private:
    static uint64_t nowMs()
    {
        return chrono::duration_cast<chrono::milliseconds>(
            chrono::system_clock::now().time_since_epoch()).count();
    }

    static uint64_t waitNextMs(uint64_t lastTs)
    {
        uint64_t ts = nowMs();
        while (ts <= lastTs)
        {
            ts = nowMs();
        }
        return ts;
    }

private:
    static constexpr uint64_t EPOCH_MS = 1704067200000ULL; // 2024-01-01 00:00:00 UTC
    static constexpr uint64_t WORKER_ID_BITS = 10;
    static constexpr uint64_t SEQ_BITS = 12;
    static constexpr uint64_t WORKER_ID_MASK = (1ULL << WORKER_ID_BITS) - 1;
    static constexpr uint64_t SEQ_MASK = (1ULL << SEQ_BITS) - 1;
    static constexpr uint64_t WORKER_ID_SHIFT = SEQ_BITS;
    static constexpr uint64_t TIMESTAMP_SHIFT = WORKER_ID_BITS + SEQ_BITS;

    mutex _mtx;
    uint16_t _workerId = 1;
    uint64_t _lastTs = 0;
    uint64_t _seq = 0;
};

static SnowflakeIdGenerator g_snowflake(1);

// 发送端序列号：
// - 单聊：每个接收者(toid)独立递增
// - 群聊：每个群组(groupid)独立递增
static mutex g_clientSeqMutex;
static unordered_map<int, uint64_t> g_peerClientSeq;
static unordered_map<int, uint64_t> g_groupClientSeq;

static uint64_t nextClientSeqForPeer(int toid)
{
    lock_guard<mutex> lock(g_clientSeqMutex);
    uint64_t &seq = g_peerClientSeq[toid];
    seq += 1;
    return seq;
}

static uint64_t nextClientSeqForGroup(int groupid)
{
    lock_guard<mutex> lock(g_clientSeqMutex);
    uint64_t &seq = g_groupClientSeq[groupid];
    seq += 1;
    return seq;
}

static string generateMsgId()
{
    return to_string(g_snowflake.nextId());
}

static mutex g_pendingMutex;
static map<string, string> g_pendingMap; // msgId -> payload

// 记录每条消息的发送时间戳，用于超时重试判断
static map<string, chrono::steady_clock::time_point> g_pendingTime;
// 超时重试间隔：5秒未收到ACK则重发
static const int RETRY_TIMEOUT_SEC = 5;
// 最大重试次数：超过后放弃并从pending移除
static const int MAX_RETRY_COUNT = 3;

// 应用层心跳间隔（秒）：定时向服务端发送 HEARTBEAT_MSG
static const int HEARTBEAT_INTERVAL_SEC = 10;
static atomic_bool g_heartbeatAcked{true};
static map<string, int> g_pendingRetry; // msgId -> 已重试次数
static int g_retryClientFd = -1; // 重试线程使用的socket fd

static void addPending(const string &msgId, const string &payload)
{
    lock_guard<mutex> lock(g_pendingMutex);
    g_pendingMap[msgId] = payload;
    g_pendingTime[msgId] = chrono::steady_clock::now();
    g_pendingRetry[msgId] = 0;
}

static void removePending(const string &msgId)
{
    lock_guard<mutex> lock(g_pendingMutex);
    g_pendingMap.erase(msgId);
    g_pendingTime.erase(msgId);
    g_pendingRetry.erase(msgId);
}

// 重试线程：每秒扫描一次 pending_map，对超时未收到ACK的消息执行重发
// 超过 MAX_RETRY_COUNT 次后放弃，打印告警
static void retryTaskHandler()
{
    while (true)
    {
        this_thread::sleep_for(chrono::seconds(1));
        if (g_retryClientFd < 0) continue;

        vector<tuple<string,string,int>> toRetry;
        vector<string> toRemove;

        {
            lock_guard<mutex> lock(g_pendingMutex);
            auto now = chrono::steady_clock::now();
            for (auto &kv : g_pendingMap)
            {
                auto elapsed = chrono::duration_cast<chrono::seconds>(
                    now - g_pendingTime[kv.first]).count();
                if (elapsed >= RETRY_TIMEOUT_SEC)
                {
                    int &cnt = g_pendingRetry[kv.first];
                    if (cnt < MAX_RETRY_COUNT)
                    {
                        int nextRetry = ++cnt;
                        toRetry.emplace_back(kv.first, kv.second, nextRetry);
                        // 重置计时，等待下一轮ACK
                        g_pendingTime[kv.first] = now;
                    }
                    else
                    {
                        // 超过最大重试次数，放弃
                        toRemove.push_back(kv.first);
                    }
                }
            }
            for (auto &id : toRemove)
            {
                g_pendingMap.erase(id);
                g_pendingTime.erase(id);
                g_pendingRetry.erase(id);
            }
        }

        for (auto &kv : toRetry)
        {
            const string &msgId = get<0>(kv);
            const string &payload = get<1>(kv);
            int retryCnt = get<2>(kv);
            cerr << "[retry] resending message_id=" << msgId
                 << " (retry " << retryCnt << "/" << MAX_RETRY_COUNT << ")" << endl;
            sendFrame(g_retryClientFd, payload);
        }
        for (auto &id : toRemove)
        {
            cerr << "[retry] give up message_id=" << id
                 << ", max retry reached, message may be lost" << endl;
        }
    }
}

// 心跳线程：周期发送 HEARTBEAT_MSG，若连续未收到ACK可快速识别异常连接
static void heartbeatTaskHandler(int clientfd)
{
    while (true)
    {
        this_thread::sleep_for(chrono::seconds(HEARTBEAT_INTERVAL_SEC));

        // 仅登录后发送心跳，避免登录前无意义探测
        if (g_myUserId <= 0)
        {
            continue;
        }

        json hb;
        hb["msgid"] = HEARTBEAT_MSG;
        hb["id"] = g_myUserId;
        hb["ts"] = chrono::duration_cast<chrono::milliseconds>(
            chrono::system_clock::now().time_since_epoch()).count();

        // 上一轮心跳若还未被ACK，打印告警（不立刻断线，避免误判）
        if (!g_heartbeatAcked.load())
        {
            cerr << "[heartbeat] previous heartbeat not acked yet, connection may be unstable" << endl;
        }

        g_heartbeatAcked = false;
        if (sendFrame(clientfd, hb.dump()) == -1)
        {
            cerr << "[heartbeat] send heartbeat failed" << endl;
        }
    }
}

// ============================================================
// 原有全局状态
// ============================================================
User g_currentUser;
vector<User> g_currentUserFriendList;
vector<Group> g_currentUserGroupList;
bool isMainMenuRunning = false;
sem_t rwsem;
atomic_bool g_isLoginSuccess{false};

// 每个会话（单聊按发送者id，群聊按groupid+发送者id）最后已显示的 client_seq
static unordered_map<string, uint64_t> g_lastShownSeq;
// 乱序缓冲：key=session, value=(seq -> message)
static unordered_map<string, map<uint64_t, string>> g_orderBuffer;

static string buildSessionKey(const json &js)
{
    if (ONE_CHAT_MSG == js["msgid"].get<int>())
    {
        // 单聊：按发送者维度重排
        return string("u:") + to_string(js["id"].get<int>());
    }

    // 群聊：按“群+发送者”维度重排，避免不同发送者序号混用
    return string("g:") + to_string(js["groupid"].get<int>()) +
           ":u:" + to_string(js["id"].get<int>());
}

static string formatIncomingLine(const json &js)
{
    if (ONE_CHAT_MSG == js["msgid"].get<int>())
    {
        return js["time"].get<string>() + " [" + to_string(js["id"].get<int>()) + "]" +
               js["name"].get<string>() + " said: " + js["msg"].get<string>();
    }
    return string("群消息[") + to_string(js["groupid"].get<int>()) + "]:" +
           js["time"].get<string>() + " [" + to_string(js["id"].get<int>()) + "]" +
           js["name"].get<string>() + " said: " + js["msg"].get<string>();
}

static void printOrderedIncoming(const json &js)
{
    if (!js.contains("client_seq"))
    {
        cout << formatIncomingLine(js) << endl;
        return;
    }

    string key = buildSessionKey(js);
    uint64_t seq = js["client_seq"].get<uint64_t>();
    uint64_t &last = g_lastShownSeq[key];

    if (seq <= last)
    {
        return; // 重复/过期消息
    }

    g_orderBuffer[key][seq] = formatIncomingLine(js);

    // 连续可显示的序号依次输出
    auto &buf = g_orderBuffer[key];
    while (!buf.empty())
    {
        auto it = buf.begin();
        if (it->first == last + 1)
        {
            cout << it->second << endl;
            last = it->first;
            buf.erase(it);
        }
        else
        {
            break;
        }
    }
}

void readTaskHandler(int clientfd);
static void heartbeatTaskHandler(int clientfd);
string getCurrentTime();
void mainMenu(int);
void showCurrentUserData();

// ============================================================
// main：发送线程
// ============================================================
int main(int argc, char **argv)
{
    if (argc < 3)
    {
        cerr << "command invalid! example: ./ChatClient 127.0.0.1 6000" << endl;
        exit(-1);
    }
    char *ip = argv[1];
    uint16_t port = atoi(argv[2]);

    int clientfd = socket(AF_INET, SOCK_STREAM, 0);
    if (-1 == clientfd) { cerr << "socket create error" << endl; exit(-1); }

    sockaddr_in server;
    memset(&server, 0, sizeof(sockaddr_in));
    server.sin_family = AF_INET;
    server.sin_port = htons(port);
    server.sin_addr.s_addr = inet_addr(ip);

    if (-1 == connect(clientfd, (sockaddr *)&server, sizeof(sockaddr_in)))
    { cerr << "connect server error" << endl; close(clientfd); exit(-1); }

    sem_init(&rwsem, 0, 0);
    std::thread readTask(readTaskHandler, clientfd);
    readTask.detach();

    // 启动重试线程：后台扫描 pending_map，对超时未收到ACK的消息重发
    g_retryClientFd = clientfd;
    std::thread retryTask(retryTaskHandler);
    retryTask.detach();

    // 启动心跳线程：定时探测连接可用性
    std::thread heartbeatTask(heartbeatTaskHandler, clientfd);
    heartbeatTask.detach();

    for (;;)
    {
        cout << "========================" << endl;
        cout << "1. login" << endl;
        cout << "2. register" << endl;
        cout << "3. quit" << endl;
        cout << "========================" << endl;
        cout << "choice:";
        int choice = 0;
        cin >> choice;
        cin.get();

        switch (choice)
        {
        case 1:
        {
            int id = 0; char pwd[50] = {0};
            cout << "userid:"; cin >> id; cin.get();
            cout << "userpassword:"; cin.getline(pwd, 50);
            json js;
            js["msgid"] = LOGIN_MSG;
            js["id"] = id;
            js["password"] = pwd;
            g_isLoginSuccess = false;
            // 使用帧协议发送
            if (sendFrame(clientfd, js.dump()) == -1)
                cerr << "send login msg error" << endl;
            sem_wait(&rwsem);
            if (g_isLoginSuccess) { isMainMenuRunning = true; mainMenu(clientfd); }
        }
        break;
        case 2:
        {
            char name[50] = {0}, pwd[50] = {0};
            cout << "username:"; cin.getline(name, 50);
            cout << "userpassword:"; cin.getline(pwd, 50);
            json js;
            js["msgid"] = REG_MSG;
            js["name"] = name;
            js["password"] = pwd;
            // 使用帧协议发送
            if (sendFrame(clientfd, js.dump()) == -1)
                cerr << "send reg msg error" << endl;
            sem_wait(&rwsem);
        }
        break;
        case 3:
            close(clientfd); sem_destroy(&rwsem); exit(0);
        default:
            cerr << "invalid input!" << endl; break;
        }
    }
    return 0;
}

// 处理注册的响应逻辑
void doRegResponse(json &responsejs)
{
    if (0 != responsejs["errno"].get<int>())
        cerr << "name is already exist, register error!" << endl;
    else
        cout << "name register success, userid is " << responsejs["id"]
             << ", do not forget it!" << endl;
}

// 处理登录的响应逻辑
void doLoginResponse(json &responsejs)
{
    if (0 != responsejs["errno"].get<int>())
    {
        cerr << responsejs["errmsg"] << endl;
        g_isLoginSuccess = false;
    }
    else
    {
        g_currentUser.setId(responsejs["id"].get<int>());
        g_currentUser.setName(responsejs["name"]);
        // 登录成功后设置全局用户ID，并作为雪花workerId的一部分
        g_myUserId = g_currentUser.getId();
        g_snowflake.setWorkerId(static_cast<uint16_t>(g_myUserId & 0x3FF));

        if (responsejs.contains("friends"))
        {
            g_currentUserFriendList.clear();
            vector<string> vec = responsejs["friends"];
            for (string &str : vec)
            {
                json js = json::parse(str);
                User user;
                user.setId(js["id"].get<int>());
                user.setName(js["name"]);
                user.setState(js["state"]);
                g_currentUserFriendList.push_back(user);
            }
        }

        if (responsejs.contains("groups"))
        {
            g_currentUserGroupList.clear();
            vector<string> vec1 = responsejs["groups"];
            for (string &groupstr : vec1)
            {
                json grpjs = json::parse(groupstr);
                Group group;
                group.setId(grpjs["id"].get<int>());
                group.setName(grpjs["groupname"]);
                group.setDesc(grpjs["groupdesc"]);
                vector<string> vec2 = grpjs["users"];
                for (string &userstr : vec2)
                {
                    GroupUser user;
                    json js = json::parse(userstr);
                    user.setId(js["id"].get<int>());
                    user.setName(js["name"]);
                    user.setState(js["state"]);
                    user.setRole(js["role"]);
                    group.getUsers().push_back(user);
                }
                g_currentUserGroupList.push_back(group);
            }
        }

        showCurrentUserData();

        if (responsejs.contains("offlinemsg"))
        {
            vector<string> vec = responsejs["offlinemsg"];
            for (string &str : vec)
            {
                json js = json::parse(str);
                printOrderedIncoming(js);
            }
        }
        g_isLoginSuccess = true;
    }
}

// ============================================================
// 1/2/3子线程 - 接收线程
// 使用帧协议接收，每次 recvFrame() 得到完整 JSON
// 处理 MSG_ACK：从 pending_map 移除对应消息
// ============================================================
void readTaskHandler(int clientfd)
{
    for (;;)
    {
        // 使用帧协议接收完整消息，无粘包/拆包问题
        string payload = recvFrame(clientfd);
        if (payload.empty())
        {
            close(clientfd);
            exit(-1);
        }

        json js;
        try { js = json::parse(payload); }
        catch (...) { cerr << "[recv] JSON parse error" << endl; continue; }

        int msgtype = js["msgid"].get<int>();

        // 处理服务端ACK：从 pending_map 移除对应消息
        if (MSG_ACK == msgtype)
        {
            string msgId = js.contains("message_id") ? js["message_id"].get<string>() : "";
            int ackState = js.contains("ack_state") ? js["ack_state"].get<int>() : -1;
            if (ackState == ACK_OK)
            {
                removePending(msgId); // 服务端已确认，停止重试
            }
            else if (ackState == ACK_DEDUP)
            {
                // 重复消息被服务端去重，同样移出pending（不需要重试）
                removePending(msgId);
                cerr << "[ack] message " << msgId << " was deduped by server" << endl;
            }
            else
            {
                cerr << "[ack] message " << msgId << " failed on server, ack_state=" << ackState << endl;
            }
            continue;
        }

        if (ONE_CHAT_MSG == msgtype)
        {
            printOrderedIncoming(js);
            continue;
        }

        if (GROUP_CHAT_MSG == msgtype)
        {
            printOrderedIncoming(js);
            continue;
        }

        // 心跳ACK：标记连接可用
        if (HEARTBEAT_MSG_ACK == msgtype)
        {
            g_heartbeatAcked = true;
            continue;
        }

        if (LOGIN_MSG_ACK == msgtype)
        {
            doLoginResponse(js);
            sem_post(&rwsem);
            continue;
        }

        if (REG_MSG_ACK == msgtype)
        {
            doRegResponse(js);
            sem_post(&rwsem);
            continue;
        }
    }
}

// 显示当前登录成功用户的基本信息
void showCurrentUserData()
{
    cout << "======================login user======================" << endl;
    cout << "current login user => id:" << g_currentUser.getId() << " name:" << g_currentUser.getName() << endl;
    cout << "----------------------friend list---------------------" << endl;
    for (User &user : g_currentUserFriendList)
        cout << user.getId() << " " << user.getName() << " " << user.getState() << endl;
    cout << "----------------------group list----------------------" << endl;
    for (Group &group : g_currentUserGroupList)
    {
        cout << group.getId() << " " << group.getName() << " " << group.getDesc() << endl;
        for (GroupUser &user : group.getUsers())
            cout << user.getId() << " " << user.getName() << " " << user.getState() << " " << user.getRole() << endl;
    }
    cout << "======================================================" << endl;
}

// 获取系统时间
string getCurrentTime()
{
    auto tt = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    struct tm *ptm = localtime(&tt);
    char date[60] = {0};
    sprintf(date, "%d-%02d-%02d %02d:%02d:%02d",
            (int)ptm->tm_year + 1900, (int)ptm->tm_mon + 1, (int)ptm->tm_mday,
            (int)ptm->tm_hour, (int)ptm->tm_min, (int)ptm->tm_sec);
    return std::string(date);
}

// 命令处理函数声明
void help(int fd = 0, string str = "");
void chat(int, string);
void addfriend(int, string);
void creategroup(int, string);
void addgroup(int, string);
void groupchat(int, string);
void loginout(int, string);

unordered_map<string, string> commandMap = {
    {"help", "显示所有支持的命令，格式help"},
    {"chat", "一对一聊天，格式chat:friendid:message"},
    {"addfriend", "添加好友，格式addfriend:friendid"},
    {"creategroup", "创建群组，格式creategroup:groupname:groupdesc"},
    {"addgroup", "加入群组，格式addgroup:groupid"},
    {"groupchat", "群聊，格式groupchat:groupid:message"},
    {"loginout", "注销，格式loginout"}};

unordered_map<string, function<void(int, string)>> commandHandlerMap = {
    {"help", help},
    {"chat", chat},
    {"addfriend", addfriend},
    {"creategroup", creategroup},
    {"addgroup", addgroup},
    {"groupchat", groupchat},
    {"loginout", loginout}};

void mainMenu(int clientfd)
{
    help();
    char buffer[1024] = {0};
    while (isMainMenuRunning)
    {
        cin.getline(buffer, 1024);
        string commandbuf(buffer);
        string command;
        int idx = commandbuf.find(":");
        if (-1 == idx)
            command = commandbuf;
        else
            command = commandbuf.substr(0, idx);
        auto it = commandHandlerMap.find(command);
        if (it == commandHandlerMap.end())
        { cerr << "invalid input command!" << endl; continue; }
        it->second(clientfd, commandbuf.substr(idx + 1, commandbuf.size() - idx));
    }
}

void help(int, string)
{
    cout << "show command list >>> " << endl;
    for (auto &p : commandMap)
        cout << p.first << " : " << p.second << endl;
    cout << endl;
}

void addfriend(int clientfd, string str)
{
    int friendid = atoi(str.c_str());
    json js;
    js["msgid"] = ADD_FRIEND_MSG;
    js["id"] = g_currentUser.getId();
    js["friendid"] = friendid;
    // 帧协议发送
    if (sendFrame(clientfd, js.dump()) == -1)
        cerr << "send addfriend msg error" << endl;
}

void chat(int clientfd, string str)
{
    int idx = str.find(":");
    if (-1 == idx) { cerr << "chat command invalid!" << endl; return; }
    int friendid = atoi(str.substr(0, idx).c_str());
    string message = str.substr(idx + 1, str.size() - idx);
    json js;
    js["msgid"] = ONE_CHAT_MSG;
    js["id"] = g_currentUser.getId();
    js["name"] = g_currentUser.getName();
    js["toid"] = friendid;
    js["msg"] = message;
    js["time"] = getCurrentTime();
    // 单聊：按目标接收者维度递增序号（toid 独立）
    js["client_seq"] = nextClientSeqForPeer(friendid);
    // 携带唯一 message_id，加入 pending 等待ACK
    string msgId = generateMsgId();
    js["message_id"] = msgId;
    string payload = js.dump();
    addPending(msgId, payload);
    // 帧协议发送
    if (sendFrame(clientfd, payload) == -1)
    {
        cerr << "send chat msg error" << endl;
        removePending(msgId); // 发送失败立即移除
    }
}

void creategroup(int clientfd, string str)
{
    int idx = str.find(":");
    if (-1 == idx) { cerr << "creategroup command invalid!" << endl; return; }
    string groupname = str.substr(0, idx);
    string groupdesc = str.substr(idx + 1, str.size() - idx);
    json js;
    js["msgid"] = CREATE_GROUP_MSG;
    js["id"] = g_currentUser.getId();
    js["groupname"] = groupname;
    js["groupdesc"] = groupdesc;
    if (sendFrame(clientfd, js.dump()) == -1)
        cerr << "send creategroup msg error" << endl;
}

void addgroup(int clientfd, string str)
{
    int groupid = atoi(str.c_str());
    json js;
    js["msgid"] = ADD_GROUP_MSG;
    js["id"] = g_currentUser.getId();
    js["groupid"] = groupid;
    if (sendFrame(clientfd, js.dump()) == -1)
        cerr << "send addgroup msg error" << endl;
}

void groupchat(int clientfd, string str)
{
    int idx = str.find(":");
    if (-1 == idx) { cerr << "groupchat command invalid!" << endl; return; }
    int groupid = atoi(str.substr(0, idx).c_str());
    string message = str.substr(idx + 1, str.size() - idx);
    json js;
    js["msgid"] = GROUP_CHAT_MSG;
    js["id"] = g_currentUser.getId();
    js["name"] = g_currentUser.getName();
    js["groupid"] = groupid;
    js["msg"] = message;
    js["time"] = getCurrentTime();
    // 群聊：按群组维度递增序号（groupid 独立）
    js["client_seq"] = nextClientSeqForGroup(groupid);
    // 携带唯一 message_id
    string msgId = generateMsgId();
    js["message_id"] = msgId;
    string payload = js.dump();
    addPending(msgId, payload);
    if (sendFrame(clientfd, payload) == -1)
    {
        cerr << "send groupchat msg error" << endl;
        removePending(msgId);
    }
}

void loginout(int clientfd, string)
{
    json js;
    js["msgid"] = LOGINOUT_MSG;
    js["id"] = g_currentUser.getId();
    if (sendFrame(clientfd, js.dump()) == -1)
        cerr << "send loginout msg error" << endl;
    else
    {
        isMainMenuRunning = false;
        g_myUserId = 0; // 注销后停止心跳
    }
}

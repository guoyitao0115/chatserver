#include "chatservice.hpp"
#include "public.hpp"
#include "framecodec.hpp"
#include <muduo/base/Logging.h>
#include <vector>
#include <string>
using namespace std;
using namespace muduo;

// 所有服务端下行消息统一通过此函数发送，自动加4字节帧头
static void sendJsonFrame(const TcpConnectionPtr &conn, const json &js)
{
    if (!conn || !conn->connected()) return;
    conn->send(FrameCodec::encode(js.dump()));
}

ChatService *ChatService::instance()
{
    static ChatService service;
    return &service;
}

ChatService::ChatService()
    : _dedup(10000, 120)
{
    _msgHandlerMap.insert({LOGIN_MSG,        std::bind(&ChatService::login,           this, _1, _2, _3)});
    _msgHandlerMap.insert({LOGINOUT_MSG,     std::bind(&ChatService::loginout,        this, _1, _2, _3)});
    _msgHandlerMap.insert({REG_MSG,          std::bind(&ChatService::reg,             this, _1, _2, _3)});
    _msgHandlerMap.insert({ONE_CHAT_MSG,     std::bind(&ChatService::oneChat,         this, _1, _2, _3)});
    _msgHandlerMap.insert({ADD_FRIEND_MSG,   std::bind(&ChatService::addFriend,       this, _1, _2, _3)});
    _msgHandlerMap.insert({CREATE_GROUP_MSG, std::bind(&ChatService::createGroup,     this, _1, _2, _3)});
    _msgHandlerMap.insert({ADD_GROUP_MSG,    std::bind(&ChatService::addGroup,        this, _1, _2, _3)});
    _msgHandlerMap.insert({GROUP_CHAT_MSG,   std::bind(&ChatService::groupChat,       this, _1, _2, _3)});
    _msgHandlerMap.insert({MSG_DELIVER_ACK,  std::bind(&ChatService::handleDeliverAck,this, _1, _2, _3)});

    if (_redis.connect())
    {
        _redis.init_notify_handler(std::bind(&ChatService::handleRedisSubscribeMessage, this, _1, _2));
    }
}

void ChatService::reset()
{
    _userModel.resetState();
}

MsgHandler ChatService::getHandler(int msgid)
{
    auto it = _msgHandlerMap.find(msgid);
    if (it == _msgHandlerMap.end())
    {
        return [=](const TcpConnectionPtr &, json &, Timestamp)
        { LOG_ERROR << "msgid:" << msgid << " can not find handler!"; };
    }
    return it->second;
}

// 一级ACK：服务端收到发送端消息后回执
void ChatService::sendAck(const TcpConnectionPtr &conn, const string &msgId, int ackState)
{
    if (msgId.empty()) return;
    json ack;
    ack["msgid"]      = MSG_ACK;
    ack["message_id"] = msgId;
    ack["ack_state"]  = ackState;
    sendJsonFrame(conn, ack);  // 加帧头
}

// 统一消息投递：local->redis->offline，返回是否成功
bool ChatService::deliverMsg(int toUserId, const string &msg, string &route)
{
    {
        lock_guard<mutex> lock(_connMutex);
        auto it = _userConnMap.find(toUserId);
        if (it != _userConnMap.end())
        {
            it->second->send(FrameCodec::encode(msg));  // 本地推送加帧头
            route = "local";
            return true;
        }
    }

    User user = _userModel.query(toUserId);
    if (user.getState() == "online")
    {
        if (_redis.publish(toUserId, msg))
        {
            route = "redis";
            return true;
        }
        route = "offline_fallback";
        LOG_WARN << "[deliverMsg] redis publish fail, fallback offline, to=" << toUserId;
    }
    else
    {
        route = "offline";
    }

    bool ok = _offlineMsgModel.insert(toUserId, msg);
    if (!ok)
    {
        LOG_ERROR << "[deliverMsg] offline insert fail, to=" << toUserId;
        return false;
    }
    return true;
}

void ChatService::login(const TcpConnectionPtr &conn, json &js, Timestamp)
{
    int id = js["id"].get<int>();
    string pwd = js["password"];
    LOG_INFO << "[login] userid=" << id;
    User user = _userModel.query(id);
    if (user.getId() == id && user.getPwd() == pwd)
    {
        if (user.getState() == "online")
        {
            json r{{"msgid",LOGIN_MSG_ACK},{"errno",2},{"errmsg","account already in use!"}};
            sendJsonFrame(conn, r);
            return;
        }
        {
            lock_guard<mutex> lock(_connMutex);
            _userConnMap[id] = conn;
        }
        _redis.subscribe(id);
        user.setState("online");
        _userModel.updateState(user);
        json response;
        response["msgid"] = LOGIN_MSG_ACK;
        response["errno"] = 0;
        response["id"]   = user.getId();
        response["name"] = user.getName();
        vector<string> off = _offlineMsgModel.query(id);
        if (!off.empty()) { response["offlinemsg"] = off; _offlineMsgModel.remove(id); }
        vector<User> friends = _friendModel.query(id);
        if (!friends.empty())
        {
            vector<string> fv;
            for (auto &u : friends)
            { json f{{"id",u.getId()},{"name",u.getName()},{"state",u.getState()}}; fv.push_back(f.dump()); }
            response["friends"] = fv;
        }
        vector<Group> groups = _groupModel.queryGroups(id);
        if (!groups.empty())
        {
            vector<string> gv;
            for (auto &g : groups)
            {
                json gj; gj["id"]=g.getId(); gj["groupname"]=g.getName(); gj["groupdesc"]=g.getDesc();
                vector<string> uv;
                for (auto &u : g.getUsers())
                { json uj{{"id",u.getId()},{"name",u.getName()},{"state",u.getState()},{"role",u.getRole()}}; uv.push_back(uj.dump()); }
                gj["users"] = uv; gv.push_back(gj.dump());
            }
            response["groups"] = gv;
        }
        sendJsonFrame(conn, response);
        LOG_INFO << "[login] userid=" << id << " success";
    }
    else
    {
        json r{{"msgid",LOGIN_MSG_ACK},{"errno",1},{"errmsg","id or password is invalid!"}};
        sendJsonFrame(conn, r);
        LOG_WARN << "[login] userid=" << id << " auth failed";
    }
}

void ChatService::reg(const TcpConnectionPtr &conn, json &js, Timestamp)
{
    User user; user.setName(js["name"]); user.setPwd(js["password"]);
    json r; r["msgid"] = REG_MSG_ACK;
    if (_userModel.insert(user)) { r["errno"]=0; r["id"]=user.getId(); }
    else { r["errno"]=1; }
    sendJsonFrame(conn, r);
}

void ChatService::loginout(const TcpConnectionPtr &, json &js, Timestamp)
{
    int userid = js["id"].get<int>();
    { lock_guard<mutex> lock(_connMutex); _userConnMap.erase(userid); }
    _redis.unsubscribe(userid);
    _userModel.updateState(User(userid, "", "", "offline"));
}

void ChatService::clientCloseException(const TcpConnectionPtr &conn)
{
    User user;
    {
        lock_guard<mutex> lock(_connMutex);
        for (auto it = _userConnMap.begin(); it != _userConnMap.end(); ++it)
        {
            if (it->second == conn) { user.setId(it->first); _userConnMap.erase(it); break; }
        }
    }
    _redis.unsubscribe(user.getId());
    if (user.getId() != -1) { user.setState("offline"); _userModel.updateState(user); }
}

void ChatService::oneChat(const TcpConnectionPtr &conn, json &js, Timestamp)
{
    int fromId = js["id"].get<int>();
    int toId   = js["toid"].get<int>();
    string msgId = js.contains("message_id") ? js["message_id"].get<string>() : "";
    LOG_INFO << "[oneChat] message_id=" << msgId << " from=" << fromId << " to=" << toId;
    if (_dedup.isDuplicate(msgId)) { sendAck(conn, msgId, ACK_DEDUP); return; }
    string route;
    bool ok = deliverMsg(toId, js.dump(), route);
    sendAck(conn, msgId, ok ? ACK_OK : ACK_FAIL);
    LOG_INFO << "[oneChat] message_id=" << msgId << " route=" << route << " ok=" << ok;
}

void ChatService::addFriend(const TcpConnectionPtr &, json &js, Timestamp)
{
    _friendModel.insert(js["id"].get<int>(), js["friendid"].get<int>());
}

void ChatService::createGroup(const TcpConnectionPtr &, json &js, Timestamp)
{
    int userid = js["id"].get<int>();
    Group group(-1, js["groupname"], js["groupdesc"]);
    if (_groupModel.createGroup(group)) { _groupModel.addGroup(userid, group.getId(), "creator"); }
}

void ChatService::addGroup(const TcpConnectionPtr &, json &js, Timestamp)
{
    _groupModel.addGroup(js["id"].get<int>(), js["groupid"].get<int>(), "normal");
}

void ChatService::groupChat(const TcpConnectionPtr &conn, json &js, Timestamp)
{
    int userid  = js["id"].get<int>();
    int groupid = js["groupid"].get<int>();
    string msgId = js.contains("message_id") ? js["message_id"].get<string>() : "";
    if (_dedup.isDuplicate(msgId)) { sendAck(conn, msgId, ACK_DEDUP); return; }
    bool allOk = true;
    for (int id : _groupModel.queryGroupUsers(userid, groupid))
    {
        string route;
        if (!deliverMsg(id, js.dump(), route)) allOk = false;
    }
    sendAck(conn, msgId, allOk ? ACK_OK : ACK_FAIL);
}

// 二级ACK：接收端展示后回执，服务端转发给原发送端
void ChatService::handleDeliverAck(const TcpConnectionPtr &, json &js, Timestamp)
{
    int originSender = js["fromid"].get<int>();
    string route;
    bool ok = deliverMsg(originSender, js.dump(), route);
    LOG_INFO << "[deliverAck] message_id=" << js["message_id"].get<string>()
             << " to(sender)=" << originSender << " route=" << route << " ok=" << ok;
}

void ChatService::handleRedisSubscribeMessage(int userid, string msg)
{
    string route;
    deliverMsg(userid, msg, route);
    LOG_INFO << "[redisMsg] userid=" << userid << " route=" << route;
}

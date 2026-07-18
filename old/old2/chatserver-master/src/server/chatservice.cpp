#include "chatservice.hpp"
#include "public.hpp"
#include "framecodec.hpp"
#include "password_hasher.hpp"
#include <muduo/base/Logging.h>
#include <vector>
#include <string>
#include <unistd.h>
using namespace std;
using namespace muduo;

static bool isBcryptHash(const string &hash)
{
    return hash.rfind("$2a$", 0) == 0 || hash.rfind("$2b$", 0) == 0 || hash.rfind("$2y$", 0) == 0;
}

// ChatService 实现：
// 1) 业务处理（登录/注册/聊天/群聊）
// 2) ACK 回执
// 3) 去重（Redis优先，本地兜底）
// 4) 统一投递（本地/Kafka/离线）

// 获取单例对象的接口函数
ChatService *ChatService::instance()
{
    static ChatService service;
    return &service;
}

// 注册消息以及对应的Handler回调操作
ChatService::ChatService()
    : _dedup(10000, 120) // 去重缓存：最多10000条记录，TTL=120秒
{
    // 用户基本业务管理相关事件处理回调注册
    _msgHandlerMap.insert({LOGIN_MSG,        std::bind(&ChatService::login,       this, _1, _2, _3)});
    _msgHandlerMap.insert({LOGINOUT_MSG,     std::bind(&ChatService::loginout,    this, _1, _2, _3)});
    _msgHandlerMap.insert({REG_MSG,          std::bind(&ChatService::reg,         this, _1, _2, _3)});
    _msgHandlerMap.insert({ONE_CHAT_MSG,     std::bind(&ChatService::oneChat,     this, _1, _2, _3)});
    _msgHandlerMap.insert({ADD_FRIEND_MSG,   std::bind(&ChatService::addFriend,   this, _1, _2, _3)});

    // 群组业务管理相关事件处理回调注册
    _msgHandlerMap.insert({CREATE_GROUP_MSG, std::bind(&ChatService::createGroup, this, _1, _2, _3)});
    _msgHandlerMap.insert({ADD_GROUP_MSG,    std::bind(&ChatService::addGroup,    this, _1, _2, _3)});
    _msgHandlerMap.insert({GROUP_CHAT_MSG,   std::bind(&ChatService::groupChat,   this, _1, _2, _3)});
    _msgHandlerMap.insert({HEARTBEAT_MSG,    std::bind(&ChatService::heartbeat,   this, _1, _2, _3)});

    // 连接redis服务器（用于去重键）
    _redis.connect();

    // 连接Kafka总线（用于跨节点转发）
    // 方案A：每个server实例使用独立consumer group，实现“广播消费+本地过滤”
    string instanceId = to_string(getpid());
    string groupId = string("chatserver_group_") + instanceId;
    if (_kafkaBus.connect("127.0.0.1:9092", "chat_cross_server", groupId, instanceId))
    {
        // 设置上报消息的回调
        _kafkaBus.init_notify_handler(
            std::bind(&ChatService::handleKafkaBusMessage, this, _1, _2));
    }
}

// 服务器异常，业务重置方法
void ChatService::reset()
{
    // 把online状态的用户，设置成offline
    _userModel.resetState();
}

// 获取消息对应的处理器
MsgHandler ChatService::getHandler(int msgid)
{
    auto it = _msgHandlerMap.find(msgid);
    if (it == _msgHandlerMap.end())
    {
        // 记录错误日志，msgid没有对应的事件处理回调
        return [=](const TcpConnectionPtr &conn, json &js, Timestamp)
        {
            LOG_ERROR << "msgid:" << msgid << " can not find handler!";
        };
    }
    return _msgHandlerMap[msgid];
}

// ============================================================
// 去重策略：优先 Redis，失败时回退本地LRU
// 返回 true=重复消息 false=新消息
// ============================================================
bool ChatService::isDuplicateWithFallback(const string &msgId)
{
    if (msgId.empty())
    {
        // 兼容旧客户端：无 message_id 不做去重
        return false;
    }

    // 先走 Redis 去重：跨实例共享，且在 Redis 持久化配置下可跨重启窗口生效
    // key 示例：chat:dedup:42_1711382400123_7
    int ret = _redis.markMessageIfFirst(REDIS_DEDUP_KEY_PREFIX + msgId, 120);
    if (ret == 1)
    {
        return false; // 首次出现
    }
    if (ret == 0)
    {
        return true; // 重复
    }

    // Redis 异常/不可用时，回退到本地内存去重，保证功能可用
    return _dedup.isDuplicate(msgId);
}

// ============================================================
// 向客户端发送服务端接收确认（MSG_ACK）
// 说明：只要服务端成功解析并进入处理流程，立即回 ACK 给发送方。
//       发送端收到 ACK 后可将消息从 pending_map 中移除，停止重试计时器。
//       ackState 参见 public.hpp AckState 枚举：
//         ACK_OK(0)=成功  ACK_DEDUP(1)=去重跳过  ACK_FAIL(2)=失败
// ============================================================
void ChatService::sendAck(const TcpConnectionPtr &conn,
                          const string &msgId,
                          int ackState)
{
    // 没有 message_id 的消息不发ACK（兼容旧客户端）
    if (!conn || !conn->connected() || msgId.empty())
        return;

    json ack;
    ack["msgid"]      = MSG_ACK;   // 服务端ACK消息类型
    ack["message_id"] = msgId;     // 对应原消息的唯一ID
    ack["ack_state"]  = ackState;  // ACK状态码
    conn->send(FrameCodec::encode(ack.dump()));
}

// ============================================================
// 统一消息投递函数
// 投递优先级：
//   1. 目标用户在本节点在线 -> 直接推送（route = "local"）
//   2. 目标用户在其他节点在线 -> Kafka 跨节点转发（route = "kafka"）
//      若 publish 失败 -> 降级写入离线库（route = "offline_fallback"）
//   3. 目标用户不在线 -> 写入离线库（route = "offline"）
//      落库失败会记录 ERROR 日志
//
// route 输出参数用于可观测性日志
// ============================================================
void ChatService::deliverMsg(int toUserId, const string &msg, string &route)
{
    // --- 路径1：本节点在线用户，直接推送 ---
    {
        lock_guard<mutex> lock(_connMutex);
        auto it = _userConnMap.find(toUserId);
        if (it != _userConnMap.end())
        {
            it->second->send(FrameCodec::encode(msg));
            route = "local";
            return;
        }
    }

    // --- 路径2：查询数据库判断用户是否在其他节点在线 ---
    User user = _userModel.query(toUserId);
    if (user.getState() == "online")
    {
        // 通过 Kafka 跨节点转发到目标用户所在节点
        bool pubOk = _kafkaBus.publish(toUserId, msg);
        if (pubOk)
        {
            route = "kafka";
            return;
        }
        // publish 失败（Kafka不可用），降级写入离线库兜底
        // 宁可让用户下次登录收到，也不能丢消息
        LOG_WARN << "[deliverMsg] Kafka publish failed for userid=" << toUserId
                 << ", fallback to offline storage";
        route = "offline_fallback";
    }
    else
    {
        route = "offline";
    }

    // --- 路径3 / 兜底：写入离线消息库 ---
    // offlinemessagemodel::insert 内部使用了转义与错误日志
    // 检查离线消息落库是否成功，失败时记录告警
    bool insertOk = _offlineMsgModel.insert(toUserId, msg);
    if (!insertOk)
    {
        LOG_ERROR << "[deliverMsg] offline message insert failed for userid=" << toUserId
                  << ", message may be lost! route=" << route;
    }
}

// ============================================================
// 处理登录业务
// ============================================================
void ChatService::login(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    int id      = js["id"].get<int>();
    string pwd  = js["password"];

    // 登录链路日志
    LOG_INFO << "[login] userid=" << id << " attempt login";

    User user = _userModel.query(id);

    bool authOk = false;
    bool needUpgradeToBcrypt = false;
    if (user.getId() == id)
    {
        const string storedPwd = user.getPwd();
        if (isBcryptHash(storedPwd))
        {
            authOk = PasswordHasher::verifyBcrypt(pwd, storedPwd);
        }
        else
        {
            // 兼容历史明文：先按明文校验，通过后升级为 bcrypt 存储
            authOk = (storedPwd == pwd);
            needUpgradeToBcrypt = authOk;
        }
    }

    if (authOk)
    {
        if (user.getState() == "online")
        {
            // 该用户已经登录，不允许重复登录
            json response;
            response["msgid"] = LOGIN_MSG_ACK;
            response["errno"] = 2;
            response["errmsg"] = "this account is using, input another!";
            conn->send(FrameCodec::encode(response.dump()));
            LOG_WARN << "[login] userid=" << id << " already online, rejected";
        }
        else
        {
            // 登录成功，记录用户连接信息
            {
                lock_guard<mutex> lock(_connMutex);
                _userConnMap.insert({id, conn});
            }

            // 登录成功后，Kafka方案A无需按用户动态订阅（保留兼容调用）
            _kafkaBus.subscribe(id);

            // 登录成功，更新用户状态信息 state offline=>online
            user.setState("online");
            _userModel.updateState(user);

            // 历史明文密码平滑迁移：首次成功登录后升级为 bcrypt 哈希
            if (needUpgradeToBcrypt)
            {
                string newHash = PasswordHasher::hashBcrypt(pwd, 12);
                if (!newHash.empty())
                {
                    _userModel.updatePassword(id, newHash);
                }
            }

            json response;
            response["msgid"] = LOGIN_MSG_ACK;
            response["errno"] = 0;
            response["id"]    = user.getId();
            response["name"]  = user.getName();

            // 查询该用户是否有离线消息
            vector<string> vec = _offlineMsgModel.query(id);
            if (!vec.empty())
            {
                response["offlinemsg"] = vec;
                // 先将离线消息放入响应再删除（先消费再删除）
                // 极端场景：send成功但进程崩溃会导致消息重发，客户端需做去重展示
                _offlineMsgModel.remove(id);
                LOG_INFO << "[login] userid=" << id << " pulled "
                         << vec.size() << " offline messages";
            }

            // 查询该用户的好友信息并返回
            vector<User> userVec = _friendModel.query(id);
            if (!userVec.empty())
            {
                vector<string> vec2;
                for (User &u : userVec)
                {
                    json fjs;
                    fjs["id"]    = u.getId();
                    fjs["name"]  = u.getName();
                    fjs["state"] = u.getState();
                    vec2.push_back(fjs.dump());
                }
                response["friends"] = vec2;
            }

            // 查询用户的群组信息
            vector<Group> groupuserVec = _groupModel.queryGroups(id);
            if (!groupuserVec.empty())
            {
                vector<string> groupV;
                for (Group &group : groupuserVec)
                {
                    json grpjson;
                    grpjson["id"]        = group.getId();
                    grpjson["groupname"] = group.getName();
                    grpjson["groupdesc"] = group.getDesc();
                    vector<string> userV;
                    for (GroupUser &u : group.getUsers())
                    {
                        json guj;
                        guj["id"]    = u.getId();
                        guj["name"]  = u.getName();
                        guj["state"] = u.getState();
                        guj["role"]  = u.getRole();
                        userV.push_back(guj.dump());
                    }
                    grpjson["users"] = userV;
                    groupV.push_back(grpjson.dump());
                }
                response["groups"] = groupV;
            }

            conn->send(FrameCodec::encode(response.dump()));
            LOG_INFO << "[login] userid=" << id << " login success";
        }
    }
    else
    {
        // 该用户不存在或密码错误，登录失败
        json response;
        response["msgid"] = LOGIN_MSG_ACK;
        response["errno"] = 1;
        response["errmsg"] = "id or password is invalid!";
        conn->send(FrameCodec::encode(response.dump()));
        LOG_WARN << "[login] userid=" << id << " auth failed (wrong id or password)";
    }
}

// ============================================================
// 处理注册业务
// ============================================================
void ChatService::reg(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    string name = js["name"];
    string pwd  = js["password"];

    string pwdHash = PasswordHasher::hashBcrypt(pwd, 12);
    if (pwdHash.empty())
    {
        json response;
        response["msgid"] = REG_MSG_ACK;
        response["errno"] = 1;
        conn->send(FrameCodec::encode(response.dump()));
        LOG_ERROR << "[reg] hash password failed, name=" << name;
        return;
    }

    User user;
    user.setName(name);
    user.setPwd(pwdHash);
    bool ok = _userModel.insert(user);
    if (ok)
    {
        json response;
        response["msgid"] = REG_MSG_ACK;
        response["errno"] = 0;
        response["id"]    = user.getId();
        conn->send(FrameCodec::encode(response.dump()));
        LOG_INFO << "[reg] new user registered: name=" << name << " id=" << user.getId();
    }
    else
    {
        json response;
        response["msgid"] = REG_MSG_ACK;
        response["errno"] = 1;
        conn->send(FrameCodec::encode(response.dump()));
        LOG_ERROR << "[reg] insert user failed, name=" << name;
    }
}

// ============================================================
// 处理注销业务
// ============================================================
void ChatService::loginout(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    int userid = js["id"].get<int>();

    {
        lock_guard<mutex> lock(_connMutex);
        auto it = _userConnMap.find(userid);
        if (it != _userConnMap.end())
        {
            _userConnMap.erase(it);
        }
    }

    // 用户注销，Kafka方案A无需按用户动态退订（保留兼容调用）
    _kafkaBus.unsubscribe(userid);

    // 更新用户的状态信息为offline
    User user(userid, "", "", "offline");
    _userModel.updateState(user);

    LOG_INFO << "[loginout] userid=" << userid << " logged out normally";
}

// ============================================================
// 处理客户端异常退出
// ============================================================
void ChatService::clientCloseException(const TcpConnectionPtr &conn)
{
    User user;
    {
        lock_guard<mutex> lock(_connMutex);
        for (auto it = _userConnMap.begin(); it != _userConnMap.end(); ++it)
        {
            if (it->second == conn)
            {
                // 从map表删除用户的链接信息
                user.setId(it->first);
                _userConnMap.erase(it);
                break;
            }
        }
    }

    // 用户下线，Kafka方案A无需按用户动态退订（保留兼容调用）
    _kafkaBus.unsubscribe(user.getId());

    // 更新用户的状态信息
    if (user.getId() != -1)
    {
        user.setState("offline");
        _userModel.updateState(user);
        // 7异常断线打点
        LOG_WARN << "[exception] userid=" << user.getId()
                 << " connection closed abnormally, set offline";
    }
}

// ============================================================
// 一对一聊天业务
// ============================================================
// 收到消息后立即回 MSG_ACK 给发送方
// 先做去重；重复消息返回 ACK_DEDUP 并跳过处理
// 通过 deliverMsg() 统一投递（含 Redis 兜底）
// 打印 message_id/from/to/route 等关键日志
// ============================================================
void ChatService::oneChat(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    int fromId = js["id"].get<int>();
    int toId   = js["toid"].get<int>();

    // 提取 message_id（客户端未携带则为空字符串，兼容旧客户端）
    string msgId = js.contains("message_id") ? js["message_id"].get<string>() : "";

    // 消息接收日志
    LOG_INFO << "[oneChat] recv message_id=" << msgId
             << " from=" << fromId << " to=" << toId;

    // 去重检查：重复消息直接回 ACK_DEDUP，不重复投递
    if (isDuplicateWithFallback(msgId))
    {
        LOG_WARN << "[oneChat] duplicate message_id=" << msgId
                 << " from=" << fromId << ", skip and ack dedup";
        sendAck(conn, msgId, ACK_DEDUP);
        return;
    }

    // 统一投递，route 用于后续日志
    string route;
    deliverMsg(toId, js.dump(), route);

    // 向发送方回 ACK_OK，表示服务端已接收并投递
    sendAck(conn, msgId, ACK_OK);

    // 记录路由日志
    LOG_INFO << "[oneChat] message_id=" << msgId
             << " from=" << fromId << " to=" << toId
             << " route=" << route << " status=ok";
}

// ============================================================
// 添加好友业务 msgid id friendid
// ============================================================
void ChatService::addFriend(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    int userid   = js["id"].get<int>();
    int friendid = js["friendid"].get<int>();

    // 存储好友信息
    _friendModel.insert(userid, friendid);
    LOG_INFO << "[addFriend] userid=" << userid << " add friendid=" << friendid;
}

// ============================================================
// 创建群组业务
// ============================================================
void ChatService::createGroup(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    int userid   = js["id"].get<int>();
    string name  = js["groupname"];
    string desc  = js["groupdesc"];

    // 存储新创建的群组信息
    Group group(-1, name, desc);
    if (_groupModel.createGroup(group))
    {
        // 存储群组创建人信息
        _groupModel.addGroup(userid, group.getId(), "creator");
        LOG_INFO << "[createGroup] userid=" << userid
                 << " created group=" << group.getId() << " name=" << name;
    }
    else
    {
        LOG_ERROR << "[createGroup] failed, userid=" << userid << " groupname=" << name;
    }
}

// ============================================================
// 加入群组业务
// ============================================================
void ChatService::addGroup(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    int userid  = js["id"].get<int>();
    int groupid = js["groupid"].get<int>();
    _groupModel.addGroup(userid, groupid, "normal");
    LOG_INFO << "[addGroup] userid=" << userid << " joined groupid=" << groupid;
}

// ============================================================
// 群组聊天业务
// ============================================================
// 收到消息后立即回 MSG_ACK 给发送方
// 先去重；重复消息返回 ACK_DEDUP 并跳过处理
// 通过 deliverMsg() 统一投递（含 Redis 兜底）
// 记录 message_id/groupid/from/route 等关键日志
// ============================================================
void ChatService::groupChat(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    int userid  = js["id"].get<int>();
    int groupid = js["groupid"].get<int>();

    // 提取 message_id
    string msgId = js.contains("message_id") ? js["message_id"].get<string>() : "";

    // 群消息接收日志
    LOG_INFO << "[groupChat] recv message_id=" << msgId
             << " from=" << userid << " groupid=" << groupid;

    // 去重检查：群消息同样需要去重
    if (isDuplicateWithFallback(msgId))
    {
        LOG_WARN << "[groupChat] duplicate message_id=" << msgId
                 << " from=" << userid << ", skip and ack dedup";
        sendAck(conn, msgId, ACK_DEDUP);
        return;
    }

    // 查询群组内所有成员（排除发送方自己）
    vector<int> useridVec = _groupModel.queryGroupUsers(userid, groupid);

    // 统计各路由的投递数量，用于日志
    int cntLocal = 0, cntKafka = 0, cntOffline = 0;

    // 对每个成员调用统一投递函数
    for (int id : useridVec)
    {
        string route;
        deliverMsg(id, js.dump(), route);

        // 统计路由分布（可观测性）
        if (route == "local")                              cntLocal++;
        else if (route == "kafka")                         cntKafka++;
        else /* offline / offline_fallback */              cntOffline++;
    }

    // 向发送方回 ACK_OK
    sendAck(conn, msgId, ACK_OK);

    // 群消息汇总日志
    LOG_INFO << "[groupChat] message_id=" << msgId
             << " from=" << userid << " groupid=" << groupid
             << " members=" << useridVec.size()
             << " local=" << cntLocal
             << " kafka=" << cntKafka
             << " offline=" << cntOffline;
}

// ============================================================
// 应用层心跳：客户端定时发送 HEARTBEAT_MSG，服务端立即回应 HEARTBEAT_MSG_ACK
// 作用：让客户端快速感知“连接仍可用”，避免长时间静默连接在异常网络下假存活
// ============================================================
void ChatService::heartbeat(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    json ack;
    ack["msgid"] = HEARTBEAT_MSG_ACK;
    if (js.contains("ts"))
    {
        ack["echo_ts"] = js["ts"];
    }
    conn->send(FrameCodec::encode(ack.dump()));
}

// ============================================================
// 从Kafka总线获取跨节点消息
// ============================================================
// handleKafkaBusMessage 同样使用 deliverMsg 兜底逻辑:
//   若用户此刻恰好在本节点在线则直接推送，否则写入离线库
// ============================================================
void ChatService::handleKafkaBusMessage(int userid, string msg)
{
    // 方案A下每个server实例都会消费到消息，这里只做“本地在线投递”。
    // 若目标用户不在本节点连接表中，直接忽略，避免跨节点重复投递或回环发布。
    {
        lock_guard<mutex> lock(_connMutex);
        auto it = _userConnMap.find(userid);
        if (it != _userConnMap.end())
        {
            it->second->send(FrameCodec::encode(msg));
            LOG_INFO << "[kafkaMsg] userid=" << userid << " route=local";
            return;
        }
    }

    LOG_INFO << "[kafkaMsg] userid=" << userid << " route=ignore_nonlocal";
}
 
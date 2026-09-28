# `chatservice.cpp` 讲解

## 作用概览

**业务编排核心。** 把已认证连接与 MySQL、Redis、RabbitMQ 和本地在线连接表连接起来，决定消息走本节点、跨节点还是离线库，并负责 ACK、去重、登录竞争和断线清理。

阅读位置：`src/server/chatservice.cpp`。下文严格按源码顺序展示，每一行只出现一次；解释只针对紧邻的代码片段。

## 代码片段与详细讲解

### 片段 1：第 1-22 行

```cpp
#include "chatservice.hpp"
#include "public.hpp"
#include "framecodec.hpp"
#include "password_hasher.hpp"
#include "config.hpp"
#include <muduo/base/Logging.h>
#include <vector>
#include <string>
#include <cstdlib>
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
// 4) 统一投递（本地/RabbitMQ/离线）
```

这里只依据 bcrypt 摘要固定前缀识别存量密码格式。登录时识别为 bcrypt 就走安全校验；不是该格式则进入旧明文兼容分支，并在校验成功后升级，因而老数据可以平滑迁移。

### 片段 2：第 23-48 行

```cpp

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
    // 允许通过环境变量覆盖实例ID，便于多节点部署
    if (const char *sid = std::getenv("CHAT_SERVER_ID"))
    {
        if (*sid != '\0')
        {
            _serverId = sid;
        }
    }

    _msgHandlerMap.insert({LOGIN_MSG,        std::bind(&ChatService::login,       this, _1, _2, _3)});
    _msgHandlerMap.insert({LOGINOUT_MSG,     std::bind(&ChatService::loginout,    this, _1, _2, _3)});
    _msgHandlerMap.insert({REG_MSG,          std::bind(&ChatService::reg,         this, _1, _2, _3)});
    _msgHandlerMap.insert({ONE_CHAT_MSG,     std::bind(&ChatService::oneChat,     this, _1, _2, _3)});
    _msgHandlerMap.insert({ADD_FRIEND_MSG,   std::bind(&ChatService::addFriend,   this, _1, _2, _3)});
```

函数内静态对象把业务服务限制为进程内单例。所有网络回调共享同一份连接表和基础设施连接，避免不同 ChatServer 回调各自维护互相矛盾的在线状态。

构造阶段把协议号 `LOGIN_MSG`、`LOGINOUT_MSG`、`REG_MSG`、`ONE_CHAT_MSG`、`ADD_FRIEND_MSG` 绑定到成员处理器。网络层以后只需用 JSON 中的 msgid 查表，不需要不断扩展 switch；所有回调都绑定同一个 ChatService 实例，因而共享认证连接表。

本片段读取 `CHAT_SERVER_ID`。未设置时采用紧邻的本机默认值；容器部署则覆盖这些值，因此同一二进制可以作为不同节点运行，无需重新编译。

### 片段 3：第 49-74 行

```cpp

    // 群组业务管理相关事件处理回调注册
    _msgHandlerMap.insert({CREATE_GROUP_MSG, std::bind(&ChatService::createGroup, this, _1, _2, _3)});
    _msgHandlerMap.insert({ADD_GROUP_MSG,    std::bind(&ChatService::addGroup,    this, _1, _2, _3)});
    _msgHandlerMap.insert({GROUP_CHAT_MSG,   std::bind(&ChatService::groupChat,   this, _1, _2, _3)});
    _msgHandlerMap.insert({HEARTBEAT_MSG,    std::bind(&ChatService::heartbeat,   this, _1, _2, _3)});

    // 连接redis服务器（用于去重键）
    _redis.connect();

    // 先设置回调再启动消费线程，避免连接建立瞬间收到消息时回调尚未就绪。
    _rabbitMqBus.init_notify_handler(
        std::bind(&ChatService::handleRabbitMqBusMessage, this, _1, _2));

    const string rabbitHost = chatserver::config::envOr("CHAT_RABBITMQ_HOST", "127.0.0.1");
    const int rabbitPort = chatserver::config::envIntOr("CHAT_RABBITMQ_PORT", 5672);
    const string rabbitExchange = chatserver::config::envOr("CHAT_RABBITMQ_EXCHANGE", "chat_cross_server");
    const string rabbitUser = chatserver::config::envOr("CHAT_RABBITMQ_USER", "guest");
    const string rabbitPassword = chatserver::config::envOr("CHAT_RABBITMQ_PASSWORD", "guest");

    // 方案B：direct 精确路由，按 serverId 投递，避免广播消费开销。
    if (!_rabbitMqBus.connect(rabbitHost, rabbitPort, rabbitExchange, _serverId,
                              rabbitUser, rabbitPassword))
    {
        LOG_WARN << "RabbitMQ initially unavailable; reconnect is running and current "
                    "cross-node delivery will fall back to offline storage";
    }
```

构造阶段把协议号 `CREATE_GROUP_MSG`、`ADD_GROUP_MSG`、`GROUP_CHAT_MSG`、`HEARTBEAT_MSG` 绑定到成员处理器。网络层以后只需用 JSON 中的 msgid 查表，不需要不断扩展 switch；所有回调都绑定同一个 ChatService 实例，因而共享认证连接表。

基础设施初始化的先后顺序有实际意义：Redis 先用于登录路由和去重；RabbitMQ 先安装
返回 bool 的消费回调，再建立队列并启动消费线程，避免连接刚成功就收到消息却没有
处理函数。首次连接失败只记录降级，消费线程仍会重连，当前跨节点消息则落入离线表。

本片段读取 `CHAT_RABBITMQ_HOST`、`CHAT_RABBITMQ_PORT`、`CHAT_RABBITMQ_EXCHANGE`、`CHAT_RABBITMQ_USER`、`CHAT_RABBITMQ_PASSWORD`。未设置时采用紧邻的本机默认值；容器部署则覆盖这些值，因此同一二进制可以作为不同节点运行，无需重新编译。

### 片段 4：第 75-97 行

```cpp
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
        return [=](const TcpConnectionPtr &, json &, Timestamp)
        {
            LOG_ERROR << "msgid:" << msgid << " can not find handler!";
        };
    }
    return _msgHandlerMap[msgid];
}
```

服务启动时把数据库中遗留的 online 状态统一复位。进程异常退出无法逐个执行断线回调，若不清理，下一次投递会把用户误认为在线却找不到有效节点路由。

`msgid` 是协议分发表的键。已注册编号返回对应成员函数；未知编号返回只记录错误的安全处理器，使网络层无需判空，也不会因客户端发出未知协议号而崩溃。

进程启动时数据库可能还残留上次异常退出留下的 online 标记。统一改回 offline 可以避免 `deliverMsg` 误判用户在线后查不到 Redis 路由；真正登录会重新写入数据库状态和带 TTL 的节点路由。

### 片段 5：第 98-123 行

```cpp

int ChatService::authenticatedUserId(const TcpConnectionPtr &conn)
{
    if (!conn)
    {
        return -1;
    }
    lock_guard<mutex> lock(_connMutex);
    auto it = _connUserMap.find(conn->name());
    return it == _connUserMap.end() ? -1 : it->second;
}

void ChatService::sendError(const TcpConnectionPtr &conn,
                            int code,
                            const string &message)
{
    if (!conn || !conn->connected())
    {
        return;
    }
    json response;
    response["msgid"] = ERROR_MSG;
    response["code"] = code;
    response["message"] = message;
    conn->send(FrameCodec::encode(response.dump()));
}
```

服务端以连接名反查登录用户，而不是信任 JSON 里的 `id`。查询在互斥锁内完成，因为断线回调可能同时删除映射；找不到时返回 -1，表示这条连接尚未认证。

错误也使用项目长度帧封装，客户端可与正常响应走同一接收循环。`code` 供程序判断 400/401 等类别，`message` 给人阅读；连接已经关闭时直接返回，避免向失效 socket 写数据。

### 片段 6：第 124-152 行

```cpp

bool ChatService::requireAuthenticatedUser(const TcpConnectionPtr &conn,
                                           int claimedUserId,
                                           const string &action)
{
    const int authenticatedId = authenticatedUserId(conn);
    if (authenticatedId == claimedUserId && authenticatedId > 0)
    {
        return true;
    }

    LOG_WARN << "[auth] rejected action=" << action
             << " claimed_userid=" << claimedUserId
             << " authenticated_userid=" << authenticatedId;
    sendError(conn, 401, "authentication required or user id mismatch");
    return false;
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
```

这里比较“连接实际绑定的用户”和“消息声称的用户”。例如 Alice 的连接伪造 `id=Bob` 时两者不等，服务端返回 401 并停止业务处理，堵住仅修改 JSON 就冒用身份的漏洞。

首次出现的 `message_id` 会以带 TTL 的 Redis key 原子占位；返回 0 表示别的节点已经处理过。Redis 故障才退到进程内 LRU，因此正常双节点部署可跨实例去重，降级时仍能防住同一节点重试。

### 片段 7：第 153-175 行

```cpp

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

void ChatService::forgetMessageMark(const string &msgId)
{
    if (msgId.empty()) return;
    _redis.removeMessageMark(REDIS_DEDUP_KEY_PREFIX + msgId);
    _dedup.forget(msgId);
}
```

只有后续投递完全失败时才撤销先前占用的去重标记。这样客户端重试仍会被当成新机会处理；如果保留标记，首次失败后的重试会收到“已去重”，造成真正的消息丢失。

### 片段 8：第 176-197 行

```cpp

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
```

ACK 绑定原消息的 `message_id`，发送端据此删除 pending 项。`ACK_OK` 表示本地、MQ 或离线落库至少一条路径成功，`ACK_DEDUP` 让重试停止，`ACK_FAIL` 则保留再次发送的可能。

写出数据前补上 4 字节大端长度头，接收方因此能从连续 TCP 字节中判断一条 JSON 到哪里结束。例如两条消息合并到一次读取时，会按各自长度连续拆出，而不会把两个 JSON 拼成坏数据。

### 片段 9：第 198-220 行

```cpp

// ============================================================
// 统一消息投递函数
// 投递优先级：
//   1. 目标用户在本节点在线 -> 直接推送（route = "local"）
//   2. 目标用户在其他节点在线 -> RabbitMQ 精确路由转发（route = "rabbitmq"）
//      若 publish 失败 -> 降级写入离线库（route = "offline_fallback"）
//   3. 目标用户不在线 -> 写入离线库（route = "offline"）
//      落库失败会记录 ERROR 日志
//
// route 输出参数用于可观测性日志
// ============================================================
bool ChatService::deliverMsg(int toUserId, const string &msg, string &route)
{
    // --- 路径1：本节点在线用户，直接推送 ---
    TcpConnectionPtr localConn;
    {
        lock_guard<mutex> lock(_connMutex);
        auto it = _userConnMap.find(toUserId);
        if (it != _userConnMap.end())
        {
            localConn = it->second;
        }
```

这是投递决策的唯一入口：先在加锁区复制本地连接，解锁后发送；本地没有连接才依据用户状态和 Redis 路由尝试 RabbitMQ；路由缺失或发布失败最终写离线表。锁外执行网络和数据库操作，避免慢 I/O 阻塞其他连接表访问。

两张索引解决相反方向的问题：投递时用 userId 找连接，断线时用 connection name 找 userId。更新时必须在同一互斥区保持二者对应；发送前只复制智能指针，离开锁后再进行 socket 写。

### 片段 10：第 221-242 行

```cpp
    }
    if (localConn && localConn->connected())
    {
        localConn->send(FrameCodec::encode(msg));
        route = "local";
        return true;
    }

    // --- 路径2：查询数据库判断用户是否在其他节点在线 ---
    User user = _userModel.query(toUserId);
    if (user.getState() == "online")
    {
        // 从 Redis 在线路由表读取目标用户所在 serverId，按 direct routing 精确投递
        string toServerId = _redis.getUserServer(toUserId);
        if (!toServerId.empty())
        {
            bool pubOk = _rabbitMqBus.publish(toServerId, toUserId, msg);
            if (pubOk)
            {
                route = "rabbitmq";
                return true;
            }
```

本地没有连接后，数据库 online 只是“可能在其他节点”的提示，真正目标节点来自 Redis。查到 serverId 才向 direct exchange 发布；任何路由缺失或发布失败都继续执行下面的离线落库，不能把日志当成成功。

### 片段 11：第 243-268 行

```cpp
            LOG_WARN << "[deliverMsg] RabbitMQ publish failed for userid=" << toUserId
                     << ", toServerId=" << toServerId << ", fallback to offline storage";
        }
        else
        {
            LOG_WARN << "[deliverMsg] online user but missing route, userid=" << toUserId
                     << ", fallback to offline storage";
        }

        // 路由缺失或发布失败，降级写离线库兜底
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
```

离线表写入的是未经改写的完整 JSON，因此接收者恢复后仍保留 message_id、client_seq、发送者和时间。只有 insert 返回成功，统一投递才向发送端承诺 ACK_OK；失败会返回 false 并由上层允许同 id 重试。

### 片段 12：第 269-299 行

```cpp
    return insertOk;
}

// ============================================================
// 处理登录业务
// ============================================================
void ChatService::login(const TcpConnectionPtr &conn, json &js, Timestamp)
{
    int id      = js["id"].get<int>();
    string pwd  = js["password"];

    if (id <= 0 || pwd.empty())
    {
        json response;
        response["msgid"] = LOGIN_MSG_ACK;
        response["errno"] = 1;
        response["errmsg"] = "id or password is invalid";
        conn->send(FrameCodec::encode(response.dump()));
        return;
    }

    const int alreadyAuthenticated = authenticatedUserId(conn);
    if (alreadyAuthenticated > 0 && alreadyAuthenticated != id)
    {
        json response;
        response["msgid"] = LOGIN_MSG_ACK;
        response["errno"] = 3;
        response["errmsg"] = "this connection is already authenticated";
        conn->send(FrameCodec::encode(response.dump()));
        return;
    }
```

登录不是单纯查密码：它先校验凭据，再用 Redis `SET NX` 抢占用户路由，成功后更新数据库状态并建立双向连接索引。任一步失败都回滚已抢到的路由。成功响应还装入离线消息、好友与群组，使客户端一次完成会话初始化。

同一 TCP 连接一旦绑定用户，就不能再拿它登录另一个账号；否则连接反向索引会被覆盖，旧账号断线清理和身份校验都会出错。

### 片段 13：第 300-321 行

```cpp

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
```

把数据库摘要本身交给 `crypt_r`，bcrypt 会从中提取算法、cost 和 salt 重算候选摘要。随后逐字节恒定时间比较，避免遇到首个差异就提前返回而泄漏匹配前缀。

密码分支同时兼容新旧数据：bcrypt 摘要用安全校验，历史明文只在完全相等时放行，并设置迁移标记。迁移发生在登录状态建立后，哈希升级失败不会把一次合法登录误判失败。

### 片段 14：第 322-343 行

```cpp

    if (authOk)
    {
        // Redis SET NX 作为跨节点原子登录闸门，避免两个节点同时“先查到离线、再都登录成功”。
        // Redis 不可用时退化为原数据库状态检查，保证服务可用但日志明确提示降级。
        const int routeClaim = _redis.claimUserServer(id, _serverId, 120);
        if (routeClaim == 0 || (routeClaim < 0 && user.getState() == "online"))
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
            if (routeClaim < 0)
            {
                LOG_WARN << "[login] Redis unavailable, duplicate-login protection degraded userid=" << id;
            }
```

`routeClaim` 的三个值分别表示抢占成功、已有节点占用、Redis 故障。成功后若数据库 online 更新失败，会条件删除刚写的路由完成回滚；故障时才退回数据库状态检查，并明确记录保护能力下降。

### 片段 15：第 344-366 行

```cpp

            user.setState("online");
            if (!_userModel.updateState(user))
            {
                if (routeClaim == 1)
                {
                    _redis.removeUserServerIfMatches(id, _serverId);
                }
                json response;
                response["msgid"] = LOGIN_MSG_ACK;
                response["errno"] = 3;
                response["errmsg"] = "login state update failed";
                conn->send(FrameCodec::encode(response.dump()));
                LOG_ERROR << "[login] state update failed userid=" << id;
                return;
            }

            // 登录成功，记录用户连接信息
            {
                lock_guard<mutex> lock(_connMutex);
                _userConnMap[id] = conn;
                _connUserMap[conn->name()] = id;
            }
```

互斥区保护 `_connMutex`、`_userConnMap`、`_connUserMap` 的一致性。这里需要关注的不只是单个容器不崩溃，还要保证成对索引或链表/哈希表同步更新，其他 I/O 线程不会观察到一半完成的状态。

### 片段 16：第 367-397 行

```cpp

            // direct 路由模式下无需按用户动态订阅（保留兼容调用）
            _rabbitMqBus.subscribe(id);

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
```

登录成功响应在一次往返中装入离线 JSON。模型层返回领域对象，业务层再转换成协议 JSON；客户端据此恢复消息并建立联系人/群列表，无需登录后立即发多次查询。

### 片段 17：第 398-435 行

```cpp

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
```

登录成功响应在一次往返中装入好友 id、名称和状态。模型层返回领域对象，业务层再转换成协议 JSON；客户端据此恢复消息并建立联系人/群列表，无需登录后立即发多次查询。

循环遍历 `userVec`、`groupuserVec`，把每个元素独立转换、投递或校验。结果按遍历顺序追加，某个元素失败时由本片段的状态变量或断言记录，不能用一次总体成功掩盖单项失败。

### 片段 18：第 436-464 行

```cpp
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
void ChatService::reg(const TcpConnectionPtr &conn, json &js, Timestamp)
{
    string name = js["name"];
    string pwd  = js["password"];
```

注册先限制名称和 bcrypt 可处理的密码长度，再生成摘要写库。数据库自增 id 会回填到 `User`，因此成功响应能直接告诉客户端后续登录所需账号；哈希失败或插入失败都不会返回伪成功。

登录成功响应在一次往返中装入群基本信息及成员角色。模型层返回领域对象，业务层再转换成协议 JSON；客户端据此恢复消息并建立联系人/群列表，无需登录后立即发多次查询。

### 片段 19：第 465-499 行

```cpp

    if (name.empty() || name.size() > 50 || pwd.size() < 6 || pwd.size() > 72)
    {
        json response;
        response["msgid"] = REG_MSG_ACK;
        response["errno"] = 1;
        response["errmsg"] = "name or password length is invalid";
        conn->send(FrameCodec::encode(response.dump()));
        return;
    }

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
```

注册边界与存储算法相匹配：用户名限制数据库字段规模，密码至少 6 字符且不超过 bcrypt 实际处理上限 72 字节。通过校验后先生成哈希再构造 User，数据库从未接收到明文密码。

### 片段 20：第 500-528 行

```cpp
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
void ChatService::loginout(const TcpConnectionPtr &conn, json &js, Timestamp)
{
    int userid = js["id"].get<int>();

    if (!requireAuthenticatedUser(conn, userid, "logout"))
    {
        return;
    }

    {
        lock_guard<mutex> lock(_connMutex);
        auto it = _userConnMap.find(userid);
        if (it != _userConnMap.end())
        {
            _userConnMap.erase(it);
        }
```

正常登出先验证连接身份并删除本地两张索引，再用“值仍等于本节点才删除”的 Redis 操作释放路由。条件删除可防止旧连接迟到的登出把已经迁移到另一节点的新会话踢成离线。

### 片段 21：第 529-559 行

```cpp
        _connUserMap.erase(conn->name());
    }

    // 用户注销，清理在线路由并做兼容退订调用
    const int routeRelease = _redis.removeUserServerIfMatches(userid, _serverId);
    _rabbitMqBus.unsubscribe(userid);

    // 返回0表示路由已属于其他实例，旧连接不能把新会话状态覆盖成离线。
    if (routeRelease != 0)
    {
        User user(userid, "", "", "offline");
        _userModel.updateState(user);
    }

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
        auto reverseIt = _connUserMap.find(conn->name());
        if (reverseIt != _connUserMap.end())
        {
            const int userId = reverseIt->second;
            user.setId(userId);
            _connUserMap.erase(reverseIt);
```

异常断线没有可信的请求体，所以通过连接反向索引找到用户。删除映射后同样条件释放 Redis 路由；只有确认路由未被新会话接管时才把数据库状态改为 offline。

### 片段 22：第 560-583 行

```cpp

            auto userIt = _userConnMap.find(userId);
            if (userIt != _userConnMap.end() && userIt->second == conn)
            {
                _userConnMap.erase(userIt);
            }
        }
    }

    // 更新用户的状态信息
    if (user.getId() != -1)
    {
        // 用户异常下线，清理在线路由并做兼容退订调用
        const int routeRelease = _redis.removeUserServerIfMatches(user.getId(), _serverId);
        _rabbitMqBus.unsubscribe(user.getId());
        if (routeRelease != 0)
        {
            user.setState("offline");
            _userModel.updateState(user);
        }
        // 7异常断线打点
        LOG_WARN << "[exception] userid=" << user.getId()
                 << " connection closed abnormally, set offline";
    }
```

这段更新网络生命周期相关索引。用户到连接用于推送，连接到用户用于断线反查；活动时间和半包起点用于不同超时巡检。相应项必须在关闭路径同步删除，避免复用旧连接状态。

### 片段 23：第 584-605 行

```cpp
}

// ============================================================
// 一对一聊天业务
// ============================================================
// 收到消息后立即回 MSG_ACK 给发送方
// 先做去重；重复消息返回 ACK_DEDUP 并跳过处理
// 通过 deliverMsg() 统一投递（含 Redis 兜底）
// 打印 message_id/from/to/route 等关键日志
// ============================================================
void ChatService::oneChat(const TcpConnectionPtr &conn, json &js, Timestamp)
{
    int fromId = js["id"].get<int>();
    int toId   = js["toid"].get<int>();

    if (!requireAuthenticatedUser(conn, fromId, "one_chat") || toId <= 0)
    {
        return;
    }

    // 提取 message_id（客户端未携带则为空字符串，兼容旧客户端）
    string msgId = js.contains("message_id") ? js["message_id"].get<string>() : "";
```

单聊先认证发送者，再以 `message_id` 去重，随后把原 JSON 交给统一投递。成功后 ACK_OK，重复请求 ACK_DEDUP；若所有投递路径失败，撤销去重标记并 ACK_FAIL，让发送端重试而不是静默丢失。

### 片段 24：第 606-633 行

```cpp

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
    const bool delivered = deliverMsg(toId, js.dump(), route);

    if (!delivered)
    {
        // 不能把失败记录成已处理，否则发送端以相同ID重试时会被误判为重复。
        forgetMessageMark(msgId);
        sendAck(conn, msgId, ACK_FAIL);
        LOG_ERROR << "[oneChat] message_id=" << msgId
                  << " from=" << fromId << " to=" << toId
                  << " route=" << route << " status=failed";
        return;
    }
```

`message_id` 贯穿发送、ACK、重试和接收去重：同一业务消息重发时 id 不变，服务端才能识别重复；`ack_state` 则告诉发送者是已接受、已去重还是处理失败，而不是仅凭 TCP 写成功判断业务成功。

### 片段 25：第 634-655 行

```cpp

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
void ChatService::addFriend(const TcpConnectionPtr &conn, json &js, Timestamp)
{
    int userid   = js["id"].get<int>();
    int friendid = js["friendid"].get<int>();

    if (!requireAuthenticatedUser(conn, userid, "add_friend") || friendid <= 0)
    {
        return;
    }
```

这里根据投递结果决定回执：成功才返回 ACK_OK，重复请求返回 ACK_DEDUP，完全失败先撤销去重占位再 ACK_FAIL。撤销动作让客户端以同一 message_id 重试时还有机会重新投递。

### 片段 26：第 656-679 行

```cpp

    // 存储好友信息
    _friendModel.insert(userid, friendid);
    LOG_INFO << "[addFriend] userid=" << userid << " add friendid=" << friendid;
}

// ============================================================
// 创建群组业务
// ============================================================
void ChatService::createGroup(const TcpConnectionPtr &conn, json &js, Timestamp)
{
    int userid   = js["id"].get<int>();
    string name  = js["groupname"];
    string desc  = js["groupdesc"];

    if (!requireAuthenticatedUser(conn, userid, "create_group"))
    {
        return;
    }
    if (name.empty() || name.size() > 50 || desc.size() > 200)
    {
        sendError(conn, 400, "group name or description length is invalid");
        return;
    }
```

这部分完成“业务编排核心”中的边界分支：无效输入或外部操作失败会在写入后续状态前结束，成功路径才把结果交给相邻模块。这样返回值不仅代表函数结束，还决定上层能否发送成功响应或继续投递。

### 片段 27：第 680-706 行

```cpp

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
void ChatService::addGroup(const TcpConnectionPtr &conn, json &js, Timestamp)
{
    int userid  = js["id"].get<int>();
    int groupid = js["groupid"].get<int>();
    if (!requireAuthenticatedUser(conn, userid, "add_group") || groupid <= 0)
    {
        return;
    }
```

这一组先验证稳定在线后的第二次登录被拒绝，再让 Alice 连接在 JSON 中冒充 Carol，必须收到 401。好友和建群操作后主动登出重登，登录响应中仍能查到关系，证明数据确实写入 MySQL 而非只留在进程内存。

### 片段 28：第 707-730 行

```cpp
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
void ChatService::groupChat(const TcpConnectionPtr &conn, json &js, Timestamp)
{
    int userid  = js["id"].get<int>();
    int groupid = js["groupid"].get<int>();

    if (!requireAuthenticatedUser(conn, userid, "group_chat") || groupid <= 0)
    {
        return;
    }

    // 提取 message_id
    string msgId = js.contains("message_id") ? js["message_id"].get<string>() : "";
```

群聊只查询除发送者外的成员 id，并为每个成员独立调用统一投递。计数器区分本地、跨节点和离线路径；只要有成员投递失败，本次 ACK 就是失败且去重标记被撤销，因此重试可能让已成功成员收到重复，客户端仍需按 message_id 去重展示。

### 片段 29：第 731-756 行

```cpp

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
    int cntLocal = 0, cntRabbitMq = 0, cntOffline = 0;
    bool allDelivered = true;

    // 对每个成员调用统一投递函数
    for (int id : useridVec)
    {
        string route;
        allDelivered = deliverMsg(id, js.dump(), route) && allDelivered;
```

群聊扇出集合不含发送者本人。每个成员独立走统一投递，route 计数用于日志观察本地、MQ 和离线比例；`allDelivered` 累积所有结果，不能因为前几个成员成功就提前给整条群消息成功 ACK。

循环每次只消费已经确认完整的字节或已经成功写出的部分。遇到正文尚未到齐便停在当前偏移，下一次收到数据后继续；发送短写则从剩余位置续发，这正是流式 socket 不能假设“一次调用完成一条消息”的原因。

### 片段 30：第 757-782 行

```cpp

        // 统计路由分布（可观测性）
        if (route == "local")                              cntLocal++;
        else if (route == "rabbitmq")                      cntRabbitMq++;
        else /* offline / offline_fallback */              cntOffline++;
    }

    if (!allDelivered)
    {
        forgetMessageMark(msgId);
        sendAck(conn, msgId, ACK_FAIL);
    }
    else
    {
        sendAck(conn, msgId, ACK_OK);
    }

    // 群消息汇总日志
    LOG_INFO << "[groupChat] message_id=" << msgId
             << " from=" << userid << " groupid=" << groupid
             << " members=" << useridVec.size()
             << " local=" << cntLocal
             << " rabbitmq=" << cntRabbitMq
             << " offline=" << cntOffline
             << " status=" << (allDelivered ? "ok" : "partial_failure");
}
```

这里根据投递结果决定回执：成功才返回 ACK_OK，重复请求返回 ACK_DEDUP，完全失败先撤销去重占位再 ACK_FAIL。撤销动作让客户端以同一 message_id 重试时还有机会重新投递。 这里按每个群成员返回的 route 累加本地、MQ 与离线计数，并在全部成员处理结束后统一决定 ACK，日志可用于观察扇出路径分布。

### 片段 31：第 783-806 行

```cpp

// ============================================================
// 应用层心跳：客户端定时发送 HEARTBEAT_MSG，服务端立即回应 HEARTBEAT_MSG_ACK
// 作用：让客户端快速感知“连接仍可用”，避免长时间静默连接在异常网络下假存活
// ============================================================
void ChatService::heartbeat(const TcpConnectionPtr &conn, json &js, Timestamp)
{
    if (!js.contains("id") ||
        !requireAuthenticatedUser(conn, js["id"].get<int>(), "heartbeat"))
    {
        return;
    }
    json ack;
    ack["msgid"] = HEARTBEAT_MSG_ACK;
    if (js.contains("ts"))
    {
        ack["echo_ts"] = js["ts"];
    }
    const int refresh = _redis.refreshUserServerIfMatches(js["id"].get<int>(), _serverId, 120);
    if (refresh == 0)
    {
        LOG_WARN << "[heartbeat] route ownership lost userid=" << js["id"].get<int>()
                 << " server=" << _serverId;
    }
```

心跳既证明 TCP 连接仍活跃，也续期 Redis 中属于本节点的用户路由。条件续期不会覆盖其他节点的新登录；响应回显时间戳，客户端可确认链路往返。

只有已认证连接才能续租自己的在线路由。Redis 条件续期失败可能表示路由已被新会话接管，此时当前连接不应覆盖它；心跳响应回显客户端时间戳，客户端同时获得存活确认和简单 RTT 依据。

### 片段 32：第 807-832 行

```cpp
    conn->send(FrameCodec::encode(ack.dump()));
}

// ============================================================
// 从RabbitMQ总线获取跨节点消息（direct 精确路由）
// ============================================================
// handleRabbitMqBusMessage：direct 路由模式下，该消息就是发给本实例的目标用户
//   直接按 userid 在本节点连接表投递；若用户刚好下线，则降级离线库
// ============================================================
bool ChatService::handleRabbitMqBusMessage(int userid, string msg)
{
    TcpConnectionPtr localConn;
    {
        lock_guard<mutex> lock(_connMutex);
        auto it = _userConnMap.find(userid);
        if (it != _userConnMap.end())
        {
            localConn = it->second;
        }
    }
    if (localConn && localConn->connected())
    {
        localConn->send(FrameCodec::encode(msg));
        LOG_INFO << "[rabbitmqMsg] userid=" << userid << " route=local";
        return true;
    }
```

RabbitMQ 把“目标用户 id + 原消息”送到目标节点。消费回调重新检查本地连接：仍在线
就发帧并返回 true，使消费线程手动 ACK；若路由与连接之间发生断线竞态，则继续尝试
离线落库，而不是提前确认消息。

### 片段 33：第 833-845 行

```cpp

    // 目标用户在消息到达时刚好不在本节点连接表，降级写离线库兜底
    bool insertOk = _offlineMsgModel.insert(userid, msg);
    if (!insertOk)
    {
        LOG_ERROR << "[rabbitmqMsg] userid=" << userid
                  << " route=offline_fallback insert_failed";
        return false;
    }

    LOG_INFO << "[rabbitmqMsg] userid=" << userid << " route=offline_fallback";
    return true;
}

```

这部分处在离线恢复链路：落库成功返回 true，RabbitMQ 删除队列消息；数据库失败返回
false，消费线程 NACK/requeue，避免自动 ACK 后永久丢失。manual ACK 本身丢失仍可能
造成重投，因此客户端还要依靠 `message_id` 去重。

## 面试重点

- 能否沿着一条单聊消息说明本地直发、跨节点路由、离线落库、ACK 与重试之间的成功语义？

- Redis 或 RabbitMQ 故障时系统如何降级，哪些保证仍成立，哪些保证会变弱？

- 为什么“至少一次发送 + message_id 幂等”不等于严格 Exactly Once？

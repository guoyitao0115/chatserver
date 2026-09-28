# `redis.cpp` 讲解

## 作用概览

**Redis 原子状态实现。** 使用 hiredis 串行执行 `SET NX EX` 和 Lua 条件更新/删除，避免“先 GET 再 SET/DEL”在多节点间产生竞态。Redis 连接由互斥锁保护，命令失败以 -1 告知业务层降级。

阅读位置：`src/server/redis/redis.cpp`。下文严格按源码顺序展示，每一行只出现一次；解释只针对紧邻的代码片段。

## 代码片段与详细讲解

### 片段 1：第 1-27 行

```cpp
#include "redis.hpp"
#include "config.hpp"

#include <iostream>

using namespace std;

// 构造时不立即连接 Redis，避免对象创建失败。main() 会在读取配置后显式调用 connect()。
Redis::Redis() : _context(nullptr) {}

Redis::~Redis()
{
    // hiredis 的 redisContext 不是线程安全对象，析构释放也放在同一把锁下，
    // 防止业务线程仍在执行 Redis 命令时发生并发释放。
    lock_guard<mutex> lock(_mutex);
    if (_context != nullptr)
    {
        redisFree(_context);
        _context = nullptr;
    }
}

bool Redis::connect()
{
    // 连接参数来自环境变量，默认值支持本地开发直接启动。
    // 1.5 秒超时可以避免 Redis 故障时服务端启动或请求线程长期卡住。
    lock_guard<mutex> lock(_mutex);
```

互斥区保护 `_mutex` 的一致性。这里需要关注的不只是单个容器不崩溃，还要保证成对索引或链表/哈希表同步更新，其他 I/O 线程不会观察到一半完成的状态。

### 片段 2：第 28-49 行

```cpp

    const string host = chatserver::config::envOr("CHAT_REDIS_HOST", "127.0.0.1");
    const int port = chatserver::config::envIntOr("CHAT_REDIS_PORT", 6379);
    struct timeval timeout {1, 500000};

    _context = redisConnectWithTimeout(host.c_str(), port, timeout);
    if (_context == nullptr || _context->err != 0)
    {
        const string reason = _context != nullptr ? _context->errstr : "allocation failed";
        cerr << "[redis] connect failed: " << reason
             << " host=" << host << ":" << port << endl;
        if (_context != nullptr)
        {
            redisFree(_context);
            _context = nullptr;
        }
        return false;
    }

    cout << "[redis] connected. host=" << host << ":" << port << endl;
    return true;
}
```

本片段读取 `CHAT_REDIS_HOST`、`CHAT_REDIS_PORT`。未设置时采用紧邻的本机默认值；容器部署则覆盖这些值，因此同一二进制可以作为不同节点运行，无需重新编译。

Lua 把读取、比较和更新压进 Redis 的一次原子执行。若拆成多条客户端命令，比较完成到删除/续期之间可能已有新登录改写路由，旧会话就会误删或误续期新状态。

### 片段 3：第 50-75 行

```cpp

int Redis::markMessageIfFirst(const string &key, int ttlSeconds)
{
    // SET ... NX 把“检查是否存在”和“写入标记”合并为一个原子命令。
    // 多个服务节点同时处理同一个 message_id 时，只有一个节点会拿到 OK。
    lock_guard<mutex> lock(_mutex);
    if (_context == nullptr || key.empty() || ttlSeconds <= 0)
    {
        return -1;
    }

    redisReply *reply = static_cast<redisReply *>(redisCommand(
        _context, "SET %b 1 EX %d NX", key.data(), key.size(), ttlSeconds));
    if (reply == nullptr)
    {
        // 返回 -1 表示 Redis 不可判定。上层不能把它当成“重复消息”，否则会误丢消息。
        cerr << "[redis] dedup command failed" << endl;
        return -1;
    }

    int result = -1;
    // OK 表示首次登记；NIL 表示 NX 条件不满足，即该 message_id 已经处理过。
    if (reply->type == REDIS_REPLY_STATUS && reply->str != nullptr && string(reply->str) == "OK")
    {
        result = 1;
    }
```

`SET key 1 NX EX ttl` 把“判断不存在”和“写入”合成一条 Redis 原子命令。reply 为 OK 表示本消息第一次出现，nil 表示已有处理者，连接或协议错误返回 -1 触发本地兜底。

### 片段 4：第 76-99 行

```cpp
    else if (reply->type == REDIS_REPLY_NIL)
    {
        result = 0;
    }
    freeReplyObject(reply);
    return result;
}

bool Redis::removeMessageMark(const string &key)
{
    // 消息落库或投递失败后删除标记，让客户端用同一个 message_id 重试。
    // 如果不删除，失败消息会被后续重试误判为重复，从而造成真实丢消息。
    lock_guard<mutex> lock(_mutex);
    if (_context == nullptr || key.empty())
    {
        return false;
    }

    redisReply *reply = static_cast<redisReply *>(redisCommand(
        _context, "DEL %b", key.data(), key.size()));
    if (reply == nullptr)
    {
        return false;
    }
```

投递失败后删除去重 key，让发送者用相同 message_id 重试。删除本身失败不会伪装成业务成功，日志与后续 ACK_FAIL 仍能暴露这次投递未完成。

`NX` 让“仅当 key 不存在时写入”在 Redis 内原子完成。并发请求不需要先 GET 再 SET，因此不会出现两个节点都观察到空值并同时宣告成功的窗口。

### 片段 5：第 100-122 行

```cpp
    const bool ok = reply->type == REDIS_REPLY_INTEGER;
    freeReplyObject(reply);
    return ok;
}

int Redis::claimUserServer(int userid, const string &serverId, int ttlSeconds)
{
    // 登录时抢占 userId -> serverId 租约。SET NX 保证同一用户同时登录时只有一个连接成功，
    // 解决多节点部署下“一个账号被两个连接同时认为在线”的问题。
    lock_guard<mutex> lock(_mutex);
    if (_context == nullptr || userid <= 0 || serverId.empty() || ttlSeconds <= 0)
    {
        return -1;
    }

    const string key = "chat:user:server:" + to_string(userid);
    redisReply *reply = static_cast<redisReply *>(redisCommand(
        _context, "SET %b %b EX %d NX", key.data(), key.size(),
        serverId.data(), serverId.size(), ttlSeconds));
    if (reply == nullptr)
    {
        return -1;
    }
```

登录用 `SET route serverId NX EX ttl` 竞争唯一在线路由。两个节点同时登录同一账号时只有一个收到 OK，另一个收到 nil 并拒绝登录，消除了先查数据库再更新的竞态窗口。

### 片段 6：第 123-145 行

```cpp
    int result = -1;
    if (reply->type == REDIS_REPLY_STATUS && reply->str != nullptr
        && string(reply->str) == "OK")
    {
        result = 1;
    }
    else if (reply->type == REDIS_REPLY_NIL)
    {
        result = 0;
    }
    freeReplyObject(reply);
    return result;
}

int Redis::refreshUserServerIfMatches(int userid, const string &serverId, int ttlSeconds)
{
    // 心跳续期必须先比较 serverId。否则旧连接延迟到达的心跳可能把新连接的租约继续延长，
    // 造成“用户明明重新登录，却被旧连接覆盖”的状态错乱。
    lock_guard<mutex> lock(_mutex);
    if (_context == nullptr || userid <= 0 || serverId.empty() || ttlSeconds <= 0)
    {
        return -1;
    }
```

Lua 脚本先比较当前值是否仍是本节点，再执行 EXPIRE。典型时序是：旧会话的值为 `server-A`，其路由过期后用户在 `server-B` 重新登录并写入 `server-B`；A 的迟到心跳会比较失败，不能把 B 的 TTL 重置为 120 秒。若应用先单独 `GET` 再单独 `EXPIRE`，A 可能在两条命令之间读到 A、随后 B 接管、最后却把 B 的 key 续期；Lua 在 Redis 内连续执行两步，不会留下这个窗口。注意值中只有 `serverId`，因此它防的是跨实例的旧会话干扰；新旧连接若在同一实例，二者值相同，仍无法区分，需额外存会话唯一 token 才能完全隔离。

只有已认证连接才能续租自己的在线路由。Redis 条件续期失败可能表示路由已被新会话接管，此时当前连接不应覆盖它；心跳响应回显客户端时间戳，客户端同时获得存活确认和简单 RTT 依据。

### 片段 7：第 146-172 行

```cpp

    // Lua 在 Redis 端原子执行 GET + EXPIRE，避免客户端分两条命令时出现竞态窗口。
    static const char *script =
        "if redis.call('GET', KEYS[1]) == ARGV[1] then "
        "return redis.call('EXPIRE', KEYS[1], ARGV[2]) else return 0 end";
    const string key = "chat:user:server:" + to_string(userid);
    redisReply *reply = static_cast<redisReply *>(redisCommand(
        _context, "EVAL %s 1 %b %b %d", script,
        key.data(), key.size(), serverId.data(), serverId.size(), ttlSeconds));
    if (reply == nullptr)
    {
        return -1;
    }
    const int result = reply->type == REDIS_REPLY_INTEGER
                     ? static_cast<int>(reply->integer) : -1;
    freeReplyObject(reply);
    return result;
}

string Redis::getUserServer(int userid)
{
    // 查询用户当前所在服务实例。空串统一表示“无可用路由”，上层会走离线存储或本地判断。
    lock_guard<mutex> lock(_mutex);
    if (_context == nullptr || userid <= 0)
    {
        return "";
    }
```

跨节点投递按 userId 读取当前 serverId。空字符串既表示用户没有路由，也可能表示 Redis 查询失败；ChatService 都不会盲目发布，而是把消息保存到离线表。

### 片段 8：第 173-199 行

```cpp

    const string key = "chat:user:server:" + to_string(userid);
    redisReply *reply = static_cast<redisReply *>(redisCommand(
        _context, "GET %b", key.data(), key.size()));
    if (reply == nullptr)
    {
        return "";
    }

    string serverId;
    if (reply->type == REDIS_REPLY_STRING && reply->str != nullptr)
    {
        serverId.assign(reply->str, reply->len);
    }
    freeReplyObject(reply);
    return serverId;
}

int Redis::removeUserServerIfMatches(int userid, const string &serverId)
{
    // 登出/连接断开时只允许持有租约的服务实例删除路由。
    // 如果不比较 serverId，旧连接断开可能删除新登录连接的路由。
    lock_guard<mutex> lock(_mutex);
    if (_context == nullptr || userid <= 0 || serverId.empty())
    {
        return -1;
    }
```

登出和断线使用 Lua 条件删除：仅当 key 的值仍等于当前 serverId 才 DEL。返回 0 意味着路由已不存在或已被接管，调用者不能据此把数据库状态覆盖为 offline。

### 片段 9：第 200-220 行

```cpp

    // 返回值区分三种情况：1=删除成功，2=原本不存在，0=属于其他实例。
    // 业务层可以据此判断是否需要更新数据库在线状态。
    static const char *script =
        "local current = redis.call('GET', KEYS[1]); "
        "if not current then return 2 end; "
        "if current == ARGV[1] then return redis.call('DEL', KEYS[1]) end; "
        "return 0";
    const string key = "chat:user:server:" + to_string(userid);
    redisReply *reply = static_cast<redisReply *>(redisCommand(
        _context, "EVAL %s 1 %b %b", script,
        key.data(), key.size(), serverId.data(), serverId.size()));
    if (reply == nullptr)
    {
        return -1;
    }
    const int result = reply->type == REDIS_REPLY_INTEGER
                     ? static_cast<int>(reply->integer) : -1;
    freeReplyObject(reply);
    return result;
}
```

这部分完成“Redis 原子状态实现”中的边界分支：无效输入或外部操作失败会在写入后续状态前结束，成功路径才把结果交给相邻模块。这样返回值不仅代表函数结束，还决定上层能否发送成功响应或继续投递。

## 面试重点

- 能否沿着一条单聊消息说明本地直发、跨节点路由、离线落库、ACK 与重试之间的成功语义？

- Redis 或 RabbitMQ 故障时系统如何降级，哪些保证仍成立，哪些保证会变弱？

- 为什么“至少一次发送 + message_id 幂等”不等于严格 Exactly Once？

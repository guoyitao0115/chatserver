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

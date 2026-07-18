#ifndef REDIS_H
#define REDIS_H

#include <hiredis/hiredis.h>
#include <mutex>
#include <string>

// Redis 只承担共享去重键和在线路由表。跨节点消息由 RabbitMQ 负责，
// 因此删除了旧版 Redis Pub/Sub 的第二连接与常驻线程。
class Redis
{
public:
    Redis();
    ~Redis();

    Redis(const Redis &) = delete;
    Redis &operator=(const Redis &) = delete;

    bool connect();

    // 1=首次写入，0=键已存在，-1=Redis 不可用或命令失败。
    int markMessageIfFirst(const std::string &key, int ttlSeconds);
    bool removeMessageMark(const std::string &key);

    // 原子抢占在线路由：1=抢占成功，0=已有会话，-1=Redis异常。
    int claimUserServer(int userid, const std::string &serverId, int ttlSeconds = 120);
    // 仅当路由仍属于当前实例时续期：1=成功，0=不匹配/不存在，-1=异常。
    int refreshUserServerIfMatches(int userid, const std::string &serverId, int ttlSeconds = 120);
    std::string getUserServer(int userid);
    // 仅删除当前实例持有的路由：1=已删除，2=键不存在，0=属于其他实例，-1=异常。
    int removeUserServerIfMatches(int userid, const std::string &serverId);

private:
    redisContext *_context;
    // hiredis 同步连接不支持并发调用；服务端多 IO 线程必须串行访问。
    std::mutex _mutex;
};

#endif

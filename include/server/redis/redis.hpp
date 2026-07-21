#ifndef REDIS_H
#define REDIS_H

#include <hiredis/hiredis.h>
#include <mutex>
#include <string>

/**
 * hiredis 同步客户端封装，只承担两类短生命周期共享状态：
 * 1. message_id 去重标记；2. userId -> serverId 在线路由租约。
 * 跨节点消息正文由 RabbitMQ 传输，不再使用 Redis Pub/Sub。
 *
 * 单个同步 redisContext 不能被多个线程同时读写，因此每个公开命令都由 _mutex
 * 串行化。对象不可复制；析构时释放连接。当前实现连接断开后不会自动重连，调用方
 * 应根据 -1/空值进入本地去重或离线存储等降级路径。
 */
class Redis
{
public:
    // 初始为未连接状态，不进行网络 I/O。
    Redis();
    // 在锁内释放 redisContext；析构前外部应停止所有可能访问本对象的线程。
    ~Redis();

    // 同一同步连接不能安全复制，显式禁止浅拷贝导致重复释放。
    Redis(const Redis &) = delete;
    Redis &operator=(const Redis &) = delete;

    /**
     * 使用 CHAT_REDIS_HOST/PORT 建立带 1.5 秒超时的连接。
     * 成功返回 true；失败释放错误 context 并保持 _context==nullptr，允许上层降级。
     * 应在业务线程启动前调用；当前接口不用于并发重连。
     */
    bool connect();

    /**
     * 用 SET key 1 EX ttl NX 原子登记去重键。
     * 返回 1=首次写入，0=键已存在，-1=参数无效、Redis 不可用或响应异常。
     * “检查+写入”由 Redis 单命令完成，可避免多节点同时判断为新消息。
     */
    int markMessageIfFirst(const std::string &key, int ttlSeconds);

    // 删除去重键，供投递失败后允许同 ID 重试。DEL 的整数响应均视为命令成功，
    // 因而键不存在时仍返回 true；false 表示连接/参数/命令层失败。
    bool removeMessageMark(const std::string &key);

    // SET NX 原子抢占在线路由并设置租约：1=成功，0=已有会话，-1=异常或参数错误。
    int claimUserServer(int userid, const std::string &serverId, int ttlSeconds = 120);

    // Lua 原子比较 serverId 后续期，防止旧连接延长新会话租约：
    // 1=成功，0=不匹配/不存在，-1=异常。
    int refreshUserServerIfMatches(int userid, const std::string &serverId, int ttlSeconds = 120);

    // 查询路由值；用户不存在、参数错误和 Redis 故障都返回空串，调用方应按“无可用路由”降级。
    std::string getUserServer(int userid);

    // Lua 原子比较后删除，避免旧节点注销覆盖新节点登录：
    // 1=已删除，2=键不存在，0=属于其他实例，-1=异常。
    int removeUserServerIfMatches(int userid, const std::string &serverId);

private:
    redisContext *_context; // hiredis 连接句柄，由本对象独占并在析构时 redisFree
    // hiredis 同步连接不支持并发调用；服务端多 IO 线程必须串行访问。
    std::mutex _mutex;
};

#endif

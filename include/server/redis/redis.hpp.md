# `redis.hpp` 讲解

## 作用概览

**Redis 原子状态接口。** 声明跨节点消息去重和 `userId -> serverId` 在线路由操作。返回值区分成功、竞争失败和 Redis 故障，业务层据此选择拒绝、继续或降级。

阅读位置：`include/server/redis/redis.hpp`。下文严格按源码顺序展示，每一行只出现一次；解释只针对紧邻的代码片段。

## 代码片段与详细讲解

### 片段 1：第 1-23 行

```cpp
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
```

这里固定模块需要长期保存的状态。这些成员把跨回调信息留在对象生命周期内；实现文件中的锁和清理逻辑必须围绕它们保持一致。

### 片段 2：第 24-45 行

```cpp

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
```

这一组值把部署差异留在环境层：服务进程和 Compose 使用同名键，测试还可临时调大消息数或超时。示例文件只给安全占位和本机默认，不应保存真实生产密码。

### 片段 3：第 46-67 行

```cpp

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
```

只有已认证连接才能续租自己的在线路由。Redis 条件续期失败可能表示路由已被新会话接管，此时当前连接不应覆盖它；心跳响应回显客户端时间戳，客户端同时获得存活确认和简单 RTT 依据。

互斥区保护 `_mutex` 的一致性。这里需要关注的不只是单个容器不崩溃，还要保证成对索引或链表/哈希表同步更新，其他 I/O 线程不会观察到一半完成的状态。

## 面试重点

- 能否沿着一条单聊消息说明本地直发、跨节点路由、离线落库、ACK 与重试之间的成功语义？

- Redis 或 RabbitMQ 故障时系统如何降级，哪些保证仍成立，哪些保证会变弱？

- 为什么“至少一次发送 + message_id 幂等”不等于严格 Exactly Once？

# `chatservice.hpp` 讲解

## 作用概览

**业务层契约与共享状态。** 集中声明登录、关系、聊天、心跳等处理器，以及本地连接表、数据模型、Redis 路由、RabbitMQ 总线和去重缓存。两个方向的连接索引用于既能按用户投递，也能在断线时反查用户。

阅读位置：`include/server/chatservice.hpp`。下文严格按源码顺序展示，每一行只出现一次；解释只针对紧邻的代码片段。

## 代码片段与详细讲解

### 片段 1：第 1-27 行

```cpp
#ifndef CHATSERVICE_H
#define CHATSERVICE_H

/*
 * ChatService 业务层：
 * - 用户/好友/群组/离线消息管理
 * - RabbitMQ 跨节点转发
 * - 消息 ACK 与去重（去重仍用Redis键）
 */

#include <muduo/net/TcpConnection.h>
#include <unordered_map>
#include <functional>
#include <mutex>
using namespace std;
using namespace muduo;
using namespace muduo::net;

#include "redis.hpp"
#include "rabbitmq_bus.hpp"
#include "groupmodel.hpp"
#include "friendmodel.hpp"
#include "usermodel.hpp"
#include "offlinemessagemodel.hpp"
#include "msgdedup.hpp"   // 消息去重模块
#include "json.hpp"
using json = nlohmann::json;
```

这些头文件把“业务层契约与共享状态”接到项目公共协议、领域对象和所需系统库。依赖方向保持从实现到接口：模型不知道网络连接，帧工具不知道用户业务，当前文件负责在自己的层内组合它们。

### 片段 2：第 28-49 行

```cpp

// Redis 去重键命名空间。它必须在所有服务实例中保持一致，才能实现跨节点去重；
// message_id 由客户端生成并拼接在此前缀后，TTL 由业务方法设置。
static const string REDIS_DEDUP_KEY_PREFIX = "chat:dedup:";

// 消息处理器由网络层同步调用。json 已完成语法和 msgid 基础校验，但具体字段可能仍
// 来自不可信客户端；每个处理器必须验证类型、范围和连接认证绑定。
using MsgHandler = std::function<void(const TcpConnectionPtr &conn, json &js, Timestamp)>;

/**
 * 聊天系统业务编排单例。
 *
 * 它连接网络会话、MySQL 模型、Redis 路由/去重以及 RabbitMQ 跨节点总线，并统一决定
 * 消息走本地连接、跨节点队列还是离线库。实例会被多个 Muduo I/O 线程和 RabbitMQ
 * 消费线程共同访问；连接索引由 _connMutex 保护，各外部客户端的线程安全规则由
 * Redis/RabbitMqBus/MySQL 包装层分别承担。
 */
class ChatService
{
public:
    // 返回进程内唯一实例。C++11 保证函数局部 static 的首次初始化线程安全；不转移所有权。
    static ChatService *instance();
```

这里固定模块需要长期保存的状态。这些成员把跨回调信息留在对象生命周期内；实现文件中的锁和清理逻辑必须围绕它们保持一致。

### 片段 3：第 50-76 行

```cpp

    // 登录：校验密码、原子抢占在线路由、绑定连接，并返回离线消息/好友/群组快照。
    void login(const TcpConnectionPtr &conn, json &js, Timestamp time);
    // 注册：校验名称和 bcrypt 支持的密码长度，哈希后写库并返回新用户 ID。
    void reg(const TcpConnectionPtr &conn, json &js, Timestamp time);
    // 一对一聊天：校验发送者身份和目标，去重后统一投递，并按结果返回 ACK。
    void oneChat(const TcpConnectionPtr &conn, json &js, Timestamp time);
    // 添加好友：仅允许当前已认证用户以自己的 ID 发起关系写入。
    void addFriend(const TcpConnectionPtr &conn, json &js, Timestamp time);
    // 创建群组：落库群信息后将创建者以 creator 角色加入成员表。
    void createGroup(const TcpConnectionPtr &conn, json &js, Timestamp time);
    // 加入群组：以 normal 角色写入群成员关系；输入 ID 仍需认证和范围校验。
    void addGroup(const TcpConnectionPtr &conn, json &js, Timestamp time);
    // 群聊：查询除发送者外的成员并逐个投递；任一投递失败时返回 ACK_FAIL 并撤销去重标记。
    void groupChat(const TcpConnectionPtr &conn, json &js, Timestamp time);
    // 心跳：验证连接身份、刷新当前节点持有的 Redis 路由租约并回送时间戳。
    void heartbeat(const TcpConnectionPtr &conn, json &js, Timestamp time);
    // 正常注销：移除双向连接索引，仅在仍持有路由时把数据库状态更新为 offline。
    void loginout(const TcpConnectionPtr &conn, json &js, Timestamp time);
    // 异常断线清理，按连接反查用户；旧连接不得覆盖用户在其他节点建立的新会话。
    void clientCloseException(const TcpConnectionPtr &conn);
    // 服务启动时将数据库遗留的 online 状态复位；会影响全表，应只在明确的恢复流程调用。
    void reset();
    // 按 msgid 返回处理器副本；未知类型返回记录错误的安全占位处理器，而非空函数。
    MsgHandler getHandler(int msgid);
    // RabbitMQ 消费线程入口：成功发给本地连接或写入离线库后返回 true，消息总线才 ACK。
    bool handleRabbitMqBusMessage(int userid, string msg);
```

这里的布尔返回值是业务层和 RabbitMQ manual ACK 之间的契约：本地投递或离线落库成功
才返回 true，消费线程随后 ACK；落库失败返回 false，消费线程 NACK/requeue。它避免
MQ 在业务真正完成前删除消息。

### 片段 4：第 77-104 行

```cpp

private:
    // 私有构造器完成处理器注册和 Redis/RabbitMQ 初始化，防止产生多个状态中心。
    ChatService();

    /*
     * 向客户端发送服务端处理确认（MSG_ACK）。
     * 参数：
     *   conn      —— 目标连接
     *   msgId     —— 原消息的 message_id
     *   ackState  —— ACK 状态码（参见 public.hpp AckState 枚举）
     * 空连接、已关闭连接或空 message_id 时静默跳过，以兼容旧协议。ACK_OK 表示服务端
     * 已完成本地/队列/离线库中的一种投递，不等价于接收用户已经阅读。
     */
    void sendAck(const TcpConnectionPtr &conn, const string &msgId, int ackState);

    /*
     * 统一消息投递函数
     * 优先本节点直接推送，否则通过RabbitMQ按 serverId 精确路由跨节点转发；
     * 转发失败时同步写入离线库兜底；只有直接发送、队列发布或离线落库成功才返回 true。
     * 参数：
     *   toUserId  —— 目标用户ID
     *   msg       —— 消息JSON字符串
     *   route     —— 输出参数，记录实际路由路径，供日志和测试观察，不参与投递决策
     * 复制本地 TcpConnectionPtr 后立即释放 _connMutex，再执行网络发送，避免慢客户端
     * 把登录/退出等连接表操作阻塞在锁内。
     */
    bool deliverMsg(int toUserId, const string &msg, string &route);
```

这里根据投递结果决定回执：成功才返回 ACK_OK，重复请求返回 ACK_DEDUP，完全失败先撤销去重占位再 ACK_FAIL。撤销动作让客户端以同一 message_id 重试时还有机会重新投递。

### 片段 5：第 105-127 行

```cpp

    /*
     * 去重策略：优先 Redis（跨实例共享），失败时回退本地 LRU 去重。
     * 返回 true=重复消息，false=新消息。
     * false 会同时创建处理标记；若后续投递失败，调用方必须 forgetMessageMark()。
     */
    bool isDuplicateWithFallback(const string &msgId);
    // 同时撤销 Redis 与本地标记；两边删除均设计为幂等，供失败重试路径调用。
    void forgetMessageMark(const string &msgId);

    // 在 _connMutex 下按连接名查询已认证用户；未登录、空连接返回 -1。
    int authenticatedUserId(const TcpConnectionPtr &conn);
    // 验证 JSON 声明的用户 ID 与连接绑定一致；失败时记录 action 并发送 401。
    bool requireAuthenticatedUser(const TcpConnectionPtr &conn,
                                  int claimedUserId,
                                  const string &action);
    // 发送统一 ERROR_MSG；连接无效或已关闭时不进行 I/O。
    void sendError(const TcpConnectionPtr &conn,
                   int code,
                   const string &message);

    // msgid -> 业务处理器。只在构造期写入，此后并发读取，无需额外加锁。
    unordered_map<int, MsgHandler> _msgHandlerMap;
```

这里根据投递结果决定回执：成功才返回 ACK_OK，重复请求返回 ACK_DEDUP，完全失败先撤销去重占位再 ACK_FAIL。撤销动作让客户端以同一 message_id 重试时还有机会重新投递。 这段继续落实“业务层契约与共享状态”的当前分支，并把已确认结果交给紧接着的状态更新；失败路径不会伪装成成功响应。

### 片段 6：第 128-157 行

```cpp

    // userId -> 本节点在线连接；shared_ptr 保证取出后连接对象在发送期间仍存活。
    unordered_map<int, TcpConnectionPtr> _userConnMap;

    // conn->name() -> userId，避免每条消息 O(n) 反查并阻止身份冒用。
    unordered_map<string, int> _connUserMap;

    // 同时保护正向、反向连接索引，保证登录、注销和异常断线更新具备一致视图。
    mutex _connMutex;

    // 数据操作类对象
    UserModel       _userModel;
    OfflineMsgModel _offlineMsgModel;
    FriendModel     _friendModel;
    GroupModel      _groupModel;

    // Redis 同时保存共享去重键和 userId -> serverId 的带 TTL 在线路由。
    Redis _redis;

    // RabbitMQ跨节点消息总线（direct 精确路由）
    RabbitMqBus _rabbitMqBus;

    // 当前实例唯一 ID，用作 RabbitMQ routing_key 和 Redis 路由值；可由环境变量覆盖。
    string _serverId = "server-1";

    // 消息去重器：LRU+TTL，容量10000条，TTL120秒
    MsgDedup _dedup;
};

#endif
```

互斥区保护 `_userConnMap`、`_connUserMap`、`_connMutex` 的一致性。这里需要关注的不只是单个容器不崩溃，还要保证成对索引或链表/哈希表同步更新，其他 I/O 线程不会观察到一半完成的状态。

## 面试重点

- 能否沿着一条单聊消息说明本地直发、跨节点路由、离线落库、ACK 与重试之间的成功语义？

- Redis 或 RabbitMQ 故障时系统如何降级，哪些保证仍成立，哪些保证会变弱？

- 为什么“至少一次发送 + message_id 幂等”不等于严格 Exactly Once？

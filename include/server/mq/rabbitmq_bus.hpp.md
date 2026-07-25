# `rabbitmq_bus.hpp` 讲解

## 作用概览

**跨节点消息总线接口。** 封装 direct exchange、节点专属队列、发布连接和消费线程。消息按目标 `serverId` 路由，只有持有目标用户连接的节点收到正文。

阅读位置：`include/server/mq/rabbitmq_bus.hpp`。下文严格按源码顺序展示，每一行只出现一次；解释只针对紧邻的代码片段。

## 代码片段与详细讲解

### 片段 1：第 1-35 行

```cpp
#ifndef RABBITMQ_BUS_H
#define RABBITMQ_BUS_H

#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>

#include <amqp.h>
#include <amqp_tcp_socket.h>

using std::string;
using std::function;

/*
 * RabbitMqBus：跨节点消息转发总线（RabbitMQ direct 路由）。
 *
 * 架构策略（方案B：精确路由）:
 *   - Exchange 类型：direct
 *   - 每个 server 实例绑定一个固定 routing_key（serverId）
 *   - 发送端先查询目标用户所在 serverId，再按 routing_key 精确投递
 *   - 避免 fanout 广播下“每条消息所有节点都消费”的无效开销
 *
 * 消息格式（body）："<userid>|<json_payload>"
 *   - userid  : 目标用户ID
 *   - payload : 原业务消息JSON字符串
 */
class RabbitMqBus
{
public:
    // 构造时尚未连接；队列名初始化为空，网络资源在 connect() 中创建。
    RabbitMqBus();
    // 先停止并 join 消费线程，再关闭消费/发布通道和连接，最后释放队列名副本。
    ~RabbitMqBus();
```

这里固定模块需要长期保存的状态。这些成员把跨回调信息留在对象生命周期内；实现文件中的锁和清理逻辑必须围绕它们保持一致。

### 片段 2：第 36-59 行

```cpp

    /**
     * 建立彼此独立的发布/消费连接，声明 direct exchange、独占自动删除队列，按
     * serverId 绑定后启动消费线程。
     *
     * host/port/user/password 是连接参数；exchange 必须在各节点一致；serverId 必须
     * 在同时运行的节点间唯一。成功返回 true；任一步失败返回 false，业务层应转入
     * 离线消息兜底。当前对象设计为只连接一次，不提供断线重连或重复 connect 语义。
     * init_notify_handler() 应在本函数之前完成，避免线程启动后收到消息却没有处理器。
     */
    bool connect(const string &host,
                 int           port,
                 const string &exchange,
                 const string &serverId,
                 const string &user     = "guest",
                 const string &password = "guest");

    /**
     * 将 "userid|payload" 发布到 toServerId 对应的 direct routing_key。
     * 返回 true 只表示 rabbitmq-c 已接受本次 basic_publish 调用；当前没有 publisher
     * confirm，不能证明 broker 已持久化或目标队列一定存在。失败时上层写离线库。
     * 发布连接由 _publishMutex 串行保护，可由多个 Muduo I/O 线程调用。
     */
    bool publish(const string &toServerId, int userid, const string &payload);
```

进程按命令行地址创建 TCP socket，并把文本 IP 与端口转换为网络字节序后连接。连接成功后再启动接收、重试、顺序冲刷和心跳线程，避免后台任务在 fd 尚不可用时抢先发送。

### 片段 3：第 60-83 行

```cpp

    // 保留旧业务接口；direct 模式以服务实例为粒度绑定，当前两个函数不操作 broker，恒为 true。
    bool subscribe(int userid);
    bool unsubscribe(int userid);

    // 设置消费线程的业务回调。必须在 connect() 启动线程前调用，且运行期间不要替换，
    // 因为该 std::function 本身没有互斥保护。回调运行在专用消费线程，不能抛出异常。
    void init_notify_handler(function<void(int, string)> fn);

private:
    // 消费线程主循环：以 1 秒超时等待消息，解析信封后同步调用业务回调，直到停止或连接错误。
    void consumeLoop();

    // 解析第一个 '|'：前段必须能转换为用户 ID，后段原样作为 JSON payload 输出。
    // 成功返回 true；格式/数字错误返回 false，调用方会丢弃该无效队列消息。
    static bool parseEnvelope(const string &value, int &userid, string &payload);

    // 统一检查 RabbitMQ RPC 响应并输出连接/通道/库错误上下文；正常响应返回 true。
    static bool checkAmqpReply(amqp_rpc_reply_t reply, const char *context);

private:
    string   _exchange;                           // direct exchange 名，发布和消费连接必须相同
    string   _serverId;                           // 当前实例路由 ID，也是队列 binding key
    function<void(int, string)> _notifyHandler;   // 在消费线程执行的回调；不拥有 ChatService
```

这里固定模块需要长期保存的状态：`_exchange`、`_serverId`、`_notifyHandler`。这些成员把跨回调信息留在对象生命周期内；实现文件中的锁和清理逻辑必须围绕它们保持一致。

### 片段 4：第 84-99 行

```cpp

    // rabbitmq-c 连接由本对象独占；发布使用 channel 1。
    amqp_connection_state_t _pubConn  = nullptr;

    // 消费使用独立连接和 channel 2，避免阻塞 consume 与并发 publish 共用协议状态。
    amqp_connection_state_t _subConn  = nullptr;
    amqp_bytes_t            _queueName;           // 服务端生成队列名的深拷贝，析构时 amqp_bytes_free

    std::thread       _consumeThread;              // connect 成功后启动，析构时 join
    std::atomic_bool  _running{false};             // 消费循环退出信号
    std::atomic_bool  _ready{false};               // 发布可用标志；析构开始即置 false
    // rabbitmq-c connection/channel cannot be published from multiple IO threads concurrently.
    std::mutex        _publishMutex;
};

#endif
```

互斥区保护 `共享状态` 的一致性。这里需要关注的不只是单个容器不崩溃，还要保证成对索引或链表/哈希表同步更新，其他 I/O 线程不会观察到一半完成的状态。

## 面试重点

- 能否沿着一条单聊消息说明本地直发、跨节点路由、离线落库、ACK 与重试之间的成功语义？

- Redis 或 RabbitMQ 故障时系统如何降级，哪些保证仍成立，哪些保证会变弱？

- 为什么“至少一次发送 + message_id 幂等”不等于严格 Exactly Once？

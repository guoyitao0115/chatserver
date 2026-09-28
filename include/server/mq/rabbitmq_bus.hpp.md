# `rabbitmq_bus.hpp` 讲解

## 作用概览

这个头文件定义跨节点 RabbitMQ 适配器。它把 RabbitMQ 的连接、direct 路由、发布确认、
手动消费确认和断线重连封装起来，让 `ChatService` 只面对两个关键结果：

- `publish(...) == true`：broker 已确认消息并且存在可匹配的节点队列；
- 消费回调返回 `true`：消息已写入本地 TCP 发送缓冲或 MySQL 离线表，可以向 broker ACK。

它仍不能证明最终接收客户端已经处理消息，因此客户端还要按 `message_id` 去重。

## 代码片段与讲解

### 1. 依赖和消息模型

```cpp
#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <cstdint>

#include <amqp.h>
#include <amqp_tcp_socket.h>

using std::string;
using std::function;
```

标准库部分提供配置字符串、消费回调、重连线程、原子运行标志和发布锁。rabbitmq-c
头文件提供 AMQP 连接、channel、发布、消费及确认协议结构。发布和消费各用一条独立
连接，避免阻塞式消费读取与 Muduo I/O 线程中的发布操作争抢同一个 AMQP 协议流。

消息体仍使用 `"<userid>|<json_payload>"`：RabbitMQ routing key 先把消息送到目标
ChatServer，正文中的 `userid` 再让目标节点从本地连接表找到最终 TCP 连接。

### 2. 生命周期和首次连接

```cpp
class RabbitMqBus
{
public:
    RabbitMqBus() = default;
    ~RabbitMqBus();

    bool connect(const string &host,
                 int           port,
                 const string &exchange,
                 const string &serverId,
                 const string &user     = "guest",
                 const string &password = "guest");
```

构造函数不执行网络操作，避免 `ChatService` 单例构造到一半时抛出异常。`connect()`
保存连接配置，首次建立 publisher/subscriber，并且无论首次是否全部成功都会启动消费
重连线程。返回 `false` 只描述“首次连接不完整”，不表示对象以后永远不可用。

析构顺序很重要：先停止并等待消费线程，确保它不再访问 `_subConn`，然后才能销毁
两条 rabbitmq-c 连接。

### 3. 可靠发布接口

```cpp
    bool publish(const string &toServerId, int userid, const string &payload);

    bool subscribe(int userid);
    bool unsubscribe(int userid);

    void init_notify_handler(function<bool(int, string)> fn);
```

`publish()` 使用 `toServerId` 作为 direct routing key，把用户 ID 和原 JSON 组合成
RabbitMQ body。函数同步等待 publisher confirm，并把以下情况都视为失败：

- 当前连接无法建立；
- `basic_publish` 返回错误；
- broker 返回 NACK；
- `mandatory` 发布收到 `Basic.Return`，表示没有目标队列；
- 3 秒内没有收到确认。

失败返回给 `ChatService::deliverMsg()`，后者同步写 MySQL 离线表。`subscribe()` 和
`unsubscribe()` 是旧接口兼容层；direct 模式按服务节点绑定，不需要按用户动态订阅。

消费回调改为返回 `bool`，使 MQ 层能够把业务结果转换成 ACK/NACK。若仍使用 `void`
回调，消息总线无法知道离线落库是否失败，只能过早确认消息。

### 4. 发布确认结果和连接辅助函数

```cpp
private:
    enum class PublishConfirmResult
    {
        Confirmed,
        Unroutable,
        Failed
    };

    bool connectPublisher();
    bool connectSubscriber();
    void closePublisher();
    void closeSubscriber();

    PublishConfirmResult waitForPublisherConfirm();
    void consumeLoop();
```

确认结果特意区分三种状态：

- `Confirmed`：收到 `Basic.Ack`；
- `Unroutable`：mandatory 消息没有匹配 binding；
- `Failed`：NACK、超时或连接/channel 错误。

虽然目前后两种都会触发离线兜底，但保留区分便于日志定位，也为以后不同告警策略留出
接口。发布和消费重连逻辑分开，因为两条连接由不同线程访问，故障也可能只影响其中一条。

### 5. 信封解析和 RPC 错误

```cpp
    static bool parseEnvelope(const string &value, int &userid, string &payload);
    static bool checkAmqpReply(amqp_rpc_reply_t reply, const char *context);
```

`parseEnvelope()` 只切分第一个 `|`，因此 JSON 正文中即使包含竖线也不会被破坏。用户
ID 必须能转成正整数且 payload 非空。无法解析的 poison message 会在消费循环中记录
错误并 ACK 丢弃，否则持续 NACK 会形成无限重投。

`checkAmqpReply()` 统一展开 library exception、channel close 和 connection close，
避免连接代码每一步都重复解析 rabbitmq-c 的联合返回结构。

### 6. 保存的配置与连接状态

```cpp
    string   _host;
    int      _port = 5672;
    string   _user;
    string   _password;
    string   _exchange;
    string   _serverId;
    string   _queueName;
    function<bool(int, string)> _notifyHandler;

    amqp_connection_state_t _pubConn = nullptr;
    amqp_connection_state_t _subConn = nullptr;
```

连接参数必须保存在对象中，断线后才能原样重建。`_queueName` 固定为
`chat.server.<serverId>`，而不是旧版的随机独占队列；固定 durable 队列使目标节点
暂时掉线时，persistent 消息仍可在 broker 中等待重连。

`_notifyHandler` 不拥有 `ChatService`，但 `ChatService` 单例生命周期覆盖 MQ 对象和
消费线程，因此绑定成员函数是安全的。

### 7. 线程协作

```cpp
    std::thread       _consumeThread;
    std::atomic_bool  _running{false};
    std::atomic_bool  _publisherReady{false};
    std::atomic_bool  _subscriberReady{false};
    std::mutex        _publishMutex;
};
```

消费线程独占 `_subConn`。多个 Muduo I/O 线程可能同时调用 `publish()`，所以
`_publishMutex` 必须覆盖发布、等待 confirm、关闭和重建 publisher 的完整过程；否则
一个线程可能读走另一个线程的确认帧。

三个原子标志只承担跨线程的生命周期/可用性通知，不保护 rabbitmq-c 连接内部状态。
真正的 publisher 访问仍靠互斥锁，subscriber 则靠线程所有权隔离。

## 面试重点

- publisher confirm 与 mandatory 分别解决什么问题，为什么不能只开一个？
- manual ACK 为什么必须晚于本地投递或离线落库？
- durable queue、persistent message 和断线重连如何配合？
- confirm 超时为什么可能产生重复，客户端 `message_id` 去重为什么仍然必要？
- 当前 ACK 为什么仍不等于最终接收客户端已处理？

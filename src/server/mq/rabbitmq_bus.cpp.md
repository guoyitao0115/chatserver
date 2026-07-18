# `rabbitmq_bus.cpp` 讲解

## 作用概览

该实现直接使用 rabbitmq-c 建立发布与消费通道，实现按服务节点精确路由的跨节点消息桥。

## 按学习顺序讲解

- `RabbitMqBus()`：把队列名设为空字节对象。
- `~RabbitMqBus()`：先清运行标志并等待消费线程，再依次关闭消费/发布 channel、连接和队列名内存。
- `checkAmqpReply(reply,context)`：统一展开库错误、channel close 和 connection close 详情。
- `connect(...)`：创建发布连接/channel 1并声明 direct exchange；再创建消费连接/channel 2、声明独占自动删除队列、以 `serverId` 绑定、启用消费，最后启动线程。
- `publish(toServerId,userid,payload)`：在发布锁内生成 `userid|payload`，调用 `amqp_basic_publish`。锁防止多个 Muduo I/O 线程并发使用同一 rabbitmq-c 连接。
- `subscribe(int)`、`unsubscribe(int)`：direct 节点队列模式下直接返回 true，仅保持旧业务接口兼容。
- `init_notify_handler(fn)`：保存业务回调。
- `consumeLoop()`：1 秒超时轮询消息，使析构可观察停止标志；正常 envelope 解析后回调，超时继续，真实错误退出。
- `parseEnvelope(value,userid,payload)`：以第一处竖线分隔目标 ID 与 JSON，捕获非法整数。

## 函数详细说明

### `RabbitMqBus::RabbitMqBus()`

构造函数把 `_queueName` 初始化为 `amqp_empty_bytes`。rabbitmq-c 的队列名由 AMQP 字节结构表示，先置为空可以让析构函数安全判断是否需要释放。

其他连接对象、运行标志和回调在类成员默认值中维护。构造函数不主动连接 RabbitMQ，连接参数由 `connect(...)` 统一传入。

### `RabbitMqBus::~RabbitMqBus()`

析构函数负责关闭消费线程、消费连接、发布连接以及队列名字节内存。它先把 `_ready` 和 `_running` 置为 false，再等待消费线程退出，避免对象销毁时后台线程还在访问成员变量。

随后依次关闭消费 channel 2、消费连接、发布 channel 1、发布连接。最后如果 `_queueName.bytes` 非空，就调用 `amqp_bytes_free` 释放 RabbitMQ 自动生成的队列名。

这段代码体现了资源释放顺序：先停线程，再关连接，再释放队列名。面试时可以补充，真实生产还需要考虑阻塞中的消费线程如何更快退出以及断线重连。

### `checkAmqpReply(reply, context)`

这是统一的 RabbitMQ RPC 调用结果检查函数。`reply` 是 rabbitmq-c 返回的 `amqp_rpc_reply_t`，`context` 是当前操作名称，用于错误日志定位。

如果 `reply_type` 是 `AMQP_RESPONSE_NORMAL`，函数直接返回 true。否则根据类型打印更具体的错误：没有回复、库异常、channel close、connection close 等。遇到 server exception 时，还会展开 AMQP 返回码和返回文本。

这个函数让 `connect` 里的错误处理更集中，也避免每个 AMQP 调用后重复写一大段 switch。

### `connect(host, port, exchange, serverId, user, password)`

这是 RabbitMQ 模块最核心的初始化函数。它建立两条连接：一条专门发布消息，一条专门消费消息。分开连接可以避免消费阻塞影响发布，也更符合 rabbitmq-c 同步 API 的使用方式。

发布侧流程是：创建连接、创建 TCP socket、打开 socket、登录、打开 channel 1、声明 direct exchange。direct exchange 的 routing key 会使用目标 `serverId`，实现“发给指定服务节点”。

消费侧流程是：创建另一条连接和 channel 2，声明一个独占、自动删除、由 RabbitMQ 自动命名的队列；然后把这个队列按当前 `serverId` 绑定到 exchange；最后启动 basic consume 和后台消费线程。

返回 true 表示发布和消费链路都初始化成功。任一关键步骤失败会返回 false。当前实现没有自动重试和半初始化清理的完整恢复逻辑，因此生产版本应增加重连状态机。

### `publish(toServerId, userid, payload)`

这个函数把消息发布给目标服务节点。`toServerId` 是 RabbitMQ direct exchange 的 routing key，`userid` 是最终目标用户，`payload` 是完整聊天消息 JSON。

实现会在 `_publishMutex` 内组装消息体，格式是 `userid|payload`。加锁是因为 rabbitmq-c 的同一个连接/channel 不能被多个线程并发使用，而业务 handler 可能运行在多个 Muduo I/O 线程中。

随后调用 `amqp_basic_publish`。返回 true 只表示客户端库成功把发布请求写出去，不等于消息已经持久化到 broker，也不等于目标节点已经消费成功。当前项目把它作为“跨节点投递路径已提交”的信号，面试中不能把它说成 exactly-once。

### `subscribe(int)` / `unsubscribe(int)`

这两个函数保留旧接口兼容，当前 direct 节点队列方案下不再按用户动态订阅队列，所以它们直接返回 true。

历史设计可能是“每个用户一个订阅关系”，但当前实现改为“每个服务节点一个队列，消息体里带 userid”。这样路由数量从用户级收敛到服务节点级，复杂度更低。

### `init_notify_handler(fn)`

这个函数保存业务回调。消费线程收到并解析 RabbitMQ 消息后，会调用该回调，把目标用户 ID 和消息 JSON 交给 `ChatService::handleRabbitMqBusMessage`。

调用顺序很重要：业务层应该先设置回调，再调用 `connect` 启动消费线程。否则消费线程可能先收到消息，却发现回调为空。

### `consumeLoop()`

这是后台消费线程的主循环。它会在 `_running` 为 true 时反复调用 RabbitMQ consume API，并设置 1 秒超时。设置超时的意义是让析构函数把 `_running` 改为 false 后，线程最多等待一个周期就能观察到停止信号。

正常收到 envelope 后，函数会调用 `parseEnvelope` 拆出 userid 和 payload，再调用业务回调。处理完之后销毁 envelope，避免内存泄漏。遇到超时则继续循环，遇到真实错误会打印日志并退出或继续按实现处理。

当前使用自动 ACK，这意味着消息一旦被 broker 交给消费者就视为成功。如果目标节点收到后崩溃，消息可能丢失。可靠版本应改成手动 ACK，并在本地发送或离线落库成功后再确认。

### `parseEnvelope(value, userid, payload)`

这个函数解析 RabbitMQ 消息体。发布端格式是 `userid|payload`，所以解析时寻找第一处竖线，左边转为整数 userid，右边作为原始 JSON payload。

如果找不到分隔符、userid 不是合法整数，函数返回 false。这样消费线程不会把损坏消息交给业务层处理。

这个格式简单，但属于自定义协议。后续可以改成 JSON 包裹或 protobuf，字段扩展会更自然，也能避免 payload 本身格式变化带来的歧义。

## 面试重点

重要性高。常见问题：direct exchange、routing key、独占队列分别做什么；为何自动 ACK 可能丢消息；为何未启用 durable/persistent/publisher confirm；消费线程如何停止。当前代码适合面试演示，若强调生产可靠性，应提出手动确认、持久化、确认发布和重连机制。

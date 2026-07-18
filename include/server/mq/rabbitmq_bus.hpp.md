# `rabbitmq_bus.hpp` 讲解

## 作用概览

`RabbitMqBus` 封装跨服务节点消息总线。每个节点用自己的 `serverId` 绑定 direct exchange，发送者只向目标节点路由，避免所有节点广播消费。

## 按学习顺序讲解

- `RabbitMqBus()/~RabbitMqBus()`：初始化队列名；析构时停止消费线程并关闭两个 AMQP 连接。
- `connect(...)`：分别建立发布、消费连接，声明 exchange、独占自动删除队列，绑定 `serverId` 并启动消费线程。
- `publish(toServerId, userid, payload)`：把消息封装为 `userid|json`，按目标 routing key 发布；互斥锁保护非线程安全连接。
- `subscribe/unsubscribe`：为兼容旧接口保留；direct 节点路由模式不需按用户动态订阅。
- `init_notify_handler(fn)`：设置消费后的业务回调，必须在启动消费前完成。
- `consumeLoop()`：以 1 秒超时阻塞消费，解析 envelope 并回调业务层，同时能定期检查停止标志。
- `parseEnvelope(value,userid,payload)`：拆分第一处 `|`，解析目标用户并保留完整 JSON。
- `checkAmqpReply(reply,context)`：统一识别库异常、channel 异常和 connection 异常。

## 函数详细说明

### `RabbitMqBus()` / `~RabbitMqBus()`

构造函数只初始化服务器队列名。析构先把 ready/running 置 false，等待消费线程退出，再关闭消费与发布 channel/connection，最后释放深拷贝队列名。停止顺序避免线程继续访问已经销毁的 AMQP 连接。

### `bool connect(host,port,exchange,serverId,user,password)`

- 建立独立发布连接和 channel 1，登录并声明 direct exchange。
- 建立独立消费连接和 channel 2，再次声明同名 exchange。
- 声明由 broker 命名的独占、自动删除队列，深拷贝返回的队列名。
- 以本实例 `serverId` 为 routing key 绑定队列，启动自动 ACK 消费，并创建消费线程。
- 任一步 RPC 失败立即返回 false；当前没有统一回滚已创建的前半资源，最终由析构清理。
- 成功后 `_ready=true`，发布函数才接受消息。

### `bool publish(toServerId,userid,payload)`

把目标用户与 JSON 组成 `userid|payload` envelope，在 `_publishMutex` 内调用 `amqp_basic_publish`。互斥锁保护同一 rabbitmq-c 发布连接不会被多个 I/O 线程并发使用。返回 true 只说明客户端库接受发布调用；没有 publisher confirm 时不能证明 broker 已持久接收。

### `subscribe(int)` / `unsubscribe(int)`

这两个函数始终返回 true。旧 Redis Pub/Sub 架构按用户动态订阅；当前 direct 架构按服务节点绑定一次队列，不需要用户级操作，保留函数是为了减少业务层改动。

### `init_notify_handler(function<void(int,string)> fn)`

保存消费后的业务回调。应在 `connect` 启动消费线程之前调用，否则消息可能到达时回调仍为空；`ChatService` 构造顺序已经遵守这一约束。

### `consumeLoop()`

消费线程每次以 1 秒超时调用 `amqp_consume_message`。正常消息转为字符串、调用 `parseEnvelope` 并触发业务回调；超时只用于重新检查 running；其他错误记录后退出。自动 ACK 意味着 envelope 交给客户端库后 broker 就认为完成，业务回调崩溃时可能丢失。

### `parseEnvelope(value,userid,payload)`

查找第一处 `|`；左侧用 `stoi` 解析用户 ID，异常返回 false；右侧原样保留，因此 JSON 内后续竖线不会被破坏。当前还可进一步校验 userid > 0 和 payload 非空。

### `checkAmqpReply(reply,context)`

正常 RPC 返回 true；其余根据响应类型打印无回复、库错误、channel close 或 connection close 的具体 code/text，再返回 false。context 让日志指出失败发生在哪个初始化步骤。


## 面试重点

重要性高。可能问题：为何发布和消费用不同连接？阻塞消费不能与多线程发布共用一个非线程安全连接；为什么用 direct 而不是 fanout？减少无效广播；当前 `no_ack=1` 有什么风险？消费者收到后进程崩溃可能丢失，生产可改手动 ACK、持久队列和 publisher confirm。

# `rabbitmq_bus.cpp` 讲解

## 作用概览

这个文件实现 RabbitMQ 跨节点可靠投递。执行链路是：

1. publisher 把 `userid|JSON` 作为 persistent 消息发到 direct exchange；
2. `mandatory` 检查是否存在目标节点队列，publisher confirm 检查 broker 是否确认；
3. 目标节点从 durable 队列取消息，业务成功后 manual ACK，失败则 NACK/requeue；
4. 两条连接分别自动恢复；发布失败由 `ChatService` 写入 MySQL 离线表；
5. 确认超时和消费重投可能产生副本，最终由接收客户端按 `message_id` 去重。

## 按执行顺序讲解

### 1. 超时参数和可中断重连等待

```cpp
namespace
{
constexpr int PUBLISH_CONFIRM_TIMEOUT_SEC = 3;
constexpr int RECONNECT_DELAY_MS = 1000;
constexpr int SOCKET_CONNECT_TIMEOUT_SEC = 2;

void waitForReconnectDelay(const atomic_bool &running)
{
    for (int waited = 0; running && waited < RECONNECT_DELAY_MS; waited += 100)
    {
        this_thread::sleep_for(chrono::milliseconds(100));
    }
}
}
```

连接 RabbitMQ 最多等待 2 秒，单次发布最多等待 3 秒确认，消费断线后每秒重试。
重连等待拆成 100 ms 小段，是为了析构把 `_running` 改成 `false` 后，线程能尽快退出，
而不是固定卡满一秒。

### 2. 析构和连接销毁

```cpp
RabbitMqBus::~RabbitMqBus()
{
    _running = false;
    _publisherReady = false;
    _subscriberReady = false;

    if (_consumeThread.joinable())
    {
        _consumeThread.join();
    }

    closeSubscriber();
    lock_guard<mutex> lock(_publishMutex);
    closePublisher();
}
```

先 join 再关闭 subscriber，保证消费线程不再使用连接。publisher 的销毁也要拿发布锁，
避免其他 Muduo I/O 线程恰好在等待 confirm。当前错误路径直接调用
`amqp_destroy_connection()`，不强求已经损坏的连接完成 AMQP close 握手。

```cpp
void RabbitMqBus::closePublisher()
{
    _publisherReady = false;
    if (_pubConn)
    {
        amqp_destroy_connection(_pubConn);
        _pubConn = nullptr;
    }
}

void RabbitMqBus::closeSubscriber()
{
    _subscriberReady = false;
    if (_subConn)
    {
        amqp_destroy_connection(_subConn);
        _subConn = nullptr;
    }
}
```

先把 ready 标志设为 false，再销毁指针，其他路径不会把“非空旧指针”误当作健康连接。
subscriber 连接关闭时，尚未 ACK 的 delivery 会由 RabbitMQ 重新放回队列。

### 3. 统一解释 RabbitMQ RPC 失败

```cpp
bool RabbitMqBus::checkAmqpReply(amqp_rpc_reply_t reply, const char *context)
{
    if (reply.reply_type == AMQP_RESPONSE_NORMAL)
    {
        return true;
    }

    // 根据 NONE、LIBRARY_EXCEPTION、CHANNEL_CLOSE、CONNECTION_CLOSE 输出原因。
    // 任意非正常响应最终返回 false。
}
```

rabbitmq-c 把本地网络错误和 broker 主动关闭 channel/connection 放在不同联合分支。
此函数把错误上下文与 broker reply code/text 一起记录，连接函数收到 `false` 后立即
清理半连接，不能继续复用已经被 broker 关闭的 channel。

### 4. 建立 publisher 并开启 confirm

```cpp
bool RabbitMqBus::connectPublisher()
{
    closePublisher();
    _pubConn = amqp_new_connection();
    amqp_socket_t *socket = amqp_tcp_socket_new(_pubConn);

    struct timeval connectTimeout;
    connectTimeout.tv_sec = SOCKET_CONNECT_TIMEOUT_SEC;
    connectTimeout.tv_usec = 0;
    if (!socket
        || amqp_socket_open_noblock(
               socket, _host.c_str(), _port, &connectTimeout) != AMQP_STATUS_OK)
    {
        closePublisher();
        return false;
    }

    // login -> channel 1 -> declare direct exchange -> confirm.select
    // 任一步失败都 closePublisher() 并返回 false。
    _publisherReady = true;
    return true;
}
```

每次重连都从干净连接开始。非阻塞 open 带 2 秒上限，避免 RabbitMQ 停止时把 Muduo
业务线程无限卡住。登录、打开 channel、声明 exchange 和 `confirm.select` 必须全部
成功后才能设置 ready；只建立 TCP 并不表示 AMQP publisher 可用。

exchange 暂时保持非 durable 以兼容已有同名 exchange；durable 节点队列和 persistent
消息保证 broker 正常运行时的断线积压。broker 整体重启后，重连会重新声明 exchange
和 binding。

### 5. 建立 durable 节点队列和 manual-ACK consumer

```cpp
bool RabbitMqBus::connectSubscriber()
{
    closeSubscriber();
    // 建立独立 TCP/AMQP 连接并打开 channel 2。

    amqp_queue_declare(_subConn, 2,
                       amqp_cstring_bytes(_queueName.c_str()),
                       0, /*durable*/1, /*exclusive*/0, /*auto_delete*/0,
                       amqp_empty_table);

    amqp_queue_bind(_subConn, 2,
                    amqp_cstring_bytes(_queueName.c_str()),
                    amqp_cstring_bytes(_exchange.c_str()),
                    amqp_cstring_bytes(_serverId.c_str()),
                    amqp_empty_table);

    amqp_basic_qos(_subConn, 2, 0, 64, 0);
    amqp_basic_consume(_subConn, 2,
                       amqp_cstring_bytes(_queueName.c_str()),
                       amqp_empty_bytes,
                       0, /*no_ack*/0, 0, amqp_empty_table);

    _subscriberReady = true;
    return true;
}
```

队列名称固定为 `chat.server.<serverId>`，durable 且不独占、不自动删除。节点临时断线
不会把队列一并删除。binding key 使用 `_serverId`，所以 direct exchange 只把消息送到
目标节点。

`basic_qos` 把未确认消息限制为 64 条，防止业务处理变慢时 broker 一次向进程推入无界
消息。`no_ack=0` 开启 manual ACK，消息是否删除由后面的业务返回值决定。

### 6. 首次连接和后台恢复

```cpp
bool RabbitMqBus::connect(const string &host,
                          int port,
                          const string &exchange,
                          const string &serverId,
                          const string &user,
                          const string &password)
{
    if (_running || host.empty() || port <= 0 || exchange.empty() || serverId.empty())
    {
        return false;
    }

    _host = host;
    _port = port;
    _exchange = exchange;
    _serverId = serverId;
    _user = user;
    _password = password;
    _queueName = "chat.server." + _serverId;

    bool publisherConnected = false;
    {
        lock_guard<mutex> lock(_publishMutex);
        publisherConnected = connectPublisher();
    }
    const bool subscriberConnected = connectSubscriber();

    _running = true;
    _consumeThread = thread(&RabbitMqBus::consumeLoop, this);
    return publisherConnected && subscriberConnected;
}
```

参数先保存，后面的 publisher/consumer 才能使用相同配置重建。即使首次连接失败也启动
消费线程，因此 RabbitMQ 晚于 ChatServer 启动时仍可自行恢复。返回值仅让构造阶段打印
一次降级告警，实际发布还会再次尝试 publisher 连接。

### 7. 等待 publisher confirm 和 mandatory return

```cpp
RabbitMqBus::PublishConfirmResult RabbitMqBus::waitForPublisherConfirm()
{
    const auto deadline = chrono::steady_clock::now()
                        + chrono::seconds(PUBLISH_CONFIRM_TIMEOUT_SEC);

    while (chrono::steady_clock::now() < deadline)
    {
        amqp_frame_t frame;
        const int status =
            amqp_simple_wait_frame_noblock(_pubConn, &frame, &timeout);

        if (status != AMQP_STATUS_OK) return PublishConfirmResult::Failed;
        if (frame.frame_type != AMQP_FRAME_METHOD) continue;
        if (frame.payload.method.id == AMQP_BASIC_ACK_METHOD)
            return PublishConfirmResult::Confirmed;
        if (frame.payload.method.id == AMQP_BASIC_NACK_METHOD)
            return PublishConfirmResult::Failed;
        if (frame.payload.method.id == AMQP_BASIC_RETURN_METHOD)
            return PublishConfirmResult::Unroutable;
        if (frame.payload.method.id == AMQP_CHANNEL_CLOSE_METHOD
            || frame.payload.method.id == AMQP_CONNECTION_CLOSE_METHOD)
            return PublishConfirmResult::Failed;
    }
    return PublishConfirmResult::Failed;
}
```

`Basic.Ack` 表示 broker 接受了本次发布；`Basic.Nack` 表示 broker 明确拒绝。publisher
confirm 本身不知道消息有没有匹配队列，所以还要识别 mandatory 产生的
`Basic.Return`。两者组合后，`publish()` 才能把“broker 接受但无人路由”判为失败。

发布锁保证同一时刻只有一条在途消息，因此收到的 Ack 可以明确归属当前调用，无需再建
delivery-tag 到业务消息的并发表。

### 8. 发布 persistent 消息并把失败交给离线表

```cpp
bool RabbitMqBus::publish(const string &toServerId, int userid, const string &payload)
{
    lock_guard<mutex> lock(_publishMutex);
    if (!_publisherReady && !connectPublisher())
    {
        return false;
    }

    const string body = to_string(userid) + "|" + payload;
    amqp_basic_properties_t properties;
    memset(&properties, 0, sizeof(properties));
    properties._flags = AMQP_BASIC_CONTENT_TYPE_FLAG
                      | AMQP_BASIC_DELIVERY_MODE_FLAG;
    properties.content_type = amqp_cstring_bytes("application/json");
    properties.delivery_mode = 2;

    const int status = amqp_basic_publish(
        _pubConn, 1,
        amqp_cstring_bytes(_exchange.c_str()),
        amqp_cstring_bytes(toServerId.c_str()),
        /*mandatory*/1, 0, &properties, bodyBytes);

    if (status == AMQP_STATUS_OK
        && waitForPublisherConfirm() == PublishConfirmResult::Confirmed)
    {
        return true;
    }

    closePublisher();
    connectPublisher();
    return false;
}
```

`delivery_mode=2` 把消息标为 persistent，`mandatory=1` 要求不可路由时返回。只有发布
调用和 confirm 都成功才返回 true。

失败后关闭连接，是因为 `Basic.Return` 后还可能有内容帧留在协议流中，直接复用会污染
下一次 confirm。函数会尽力为后续消息重建 publisher，但当前消息不在 MQ 层自行重发，
而是返回 false 让 `deliverMsg()` 同步写离线表。

confirm 超时可能只是确认帧丢失，消息本身也许已经入队；此时离线表会形成第二份副本。
这就是接收客户端必须按 `message_id` 去重的原因之一。

### 9. 消费、业务处理和 ACK/NACK

```cpp
void RabbitMqBus::consumeLoop()
{
    while (_running)
    {
        if (!_subscriberReady && !connectSubscriber())
        {
            waitForReconnectDelay(_running);
            continue;
        }

        amqp_envelope_t envelope;
        amqp_maybe_release_buffers(_subConn);

        struct timeval timeout;
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        const amqp_rpc_reply_t reply =
            amqp_consume_message(_subConn, &envelope, &timeout, 0);

        if (reply.reply_type == AMQP_RESPONSE_LIBRARY_EXCEPTION
            && reply.library_error == AMQP_STATUS_TIMEOUT)
        {
            continue;
        }
        if (reply.reply_type != AMQP_RESPONSE_NORMAL)
        {
            if (_running)
            {
                checkAmqpReply(reply, "consume message");
            }
            closeSubscriber();
            waitForReconnectDelay(_running);
            continue;
        }
```

1 秒空闲超时不是故障，它只是让循环定期检查退出标志。
真实连接错误会销毁 subscriber，下一轮重新执行完整声明和绑定。

有效消息调用 `ChatService::handleRabbitMqBusMessage()`：

- 本地连接仍有效并完成 `conn->send()`，返回 true；
- 用户已断开但离线落库成功，返回 true；
- 离线落库也失败，返回 false。

true 才 `basic_ack`；false 使用 `basic_nack(..., requeue=1)`，并等待 200 ms，避免数据库
故障时同一消息形成高速热循环。ACK/NACK 自身发送失败会关闭连接，broker 随后自动把
未确认消息重新入队。

```cpp
        int ackStatus = AMQP_STATUS_OK;
        if (handled)
        {
            ackStatus = amqp_basic_ack(
                _subConn, 2, envelope.delivery_tag, /*multiple*/0);
        }
        else
        {
            ackStatus = amqp_basic_nack(
                _subConn, 2, envelope.delivery_tag,
                /*multiple*/0, /*requeue*/1);
        }
        amqp_destroy_envelope(&envelope);

        if (ackStatus != AMQP_STATUS_OK)
        {
            closeSubscriber();
            waitForReconnectDelay(_running);
            continue;
        }

        if (!handled && !poisonMessage)
        {
            this_thread::sleep_for(chrono::milliseconds(200));
        }
    }
}
```

ACK/NACK 使用当前 envelope 的 `delivery_tag` 且 `multiple=0`，只确认这一条消息。
销毁 envelope 发生在读取完 tag 和 body 之后，避免继续引用 rabbitmq-c 管理的内存。

无法解析的信封属于 poison message，重试不会自动修好，所以记录错误后 ACK 丢弃，
避免永久占满队列；合法信封但业务回调尚未准备好时则 NACK/requeue，不能误删。生产
环境还应把 poison message 送入死信队列。

### 10. 兼容接口、回调和信封解析

```cpp
bool RabbitMqBus::subscribe(int) { return true; }
bool RabbitMqBus::unsubscribe(int) { return true; }

void RabbitMqBus::init_notify_handler(function<bool(int, string)> fn)
{
    _notifyHandler = std::move(fn);
}

bool RabbitMqBus::parseEnvelope(const string &value, int &userid, string &payload)
{
    const size_t pos = value.find('|');
    if (pos == string::npos) return false;
    try { userid = stoi(value.substr(0, pos)); }
    catch (...) { return false; }
    payload = value.substr(pos + 1);
    return userid > 0 && !payload.empty();
}
```

direct 模式的订阅粒度是节点而不是用户，因此旧的按用户订阅接口保留为空操作。回调
必须在 `connect()` 前设置，消费线程才不会收到消息却没有业务接收者。

信封只在第一个 `|` 处分隔，JSON 正文中的字符保持原样。非法用户 ID 或空 payload
返回 false，由消费循环按 poison message 处理。

## 为什么接收客户端还要按 `message_id` 去重

服务端入口的 Redis 去重只能阻止“发送客户端重复提交”再次进入业务处理，无法覆盖消息
进入 RabbitMQ 之后发生的重复：

1. 目标节点已经 `send()`，但 manual ACK 丢失或进程在 ACK 前崩溃，broker 会重投；
2. publisher confirm 超时，发送节点不知道消息是否已入队，于是又写入离线表；
3. 群聊部分成员成功、部分失败后发送方重试，成功成员可能再次收到；
4. 离线消息删除失败或登录响应重放，也可能再次出现同一 JSON。

因此系统采用“至少一次投递 + 各层幂等”：服务端入口去重减少重复业务执行，最终客户端
去重保证同一个 `message_id` 只展示一次。它仍不是严格 Exactly Once，因为去重缓存
有容量和生命周期边界。

## 面试重点

- confirm、mandatory、persistent message、durable queue 各自保证什么？
- manual ACK 丢失为什么会重投，而重投为什么不是 RabbitMQ 的 bug？
- 为什么发布失败写离线表仍可能制造重复？
- 为什么本项目选择同步逐条 confirm，它对吞吐有什么影响？
- 下一步如何用死信队列、接收端 ACK 和 Outbox/Inbox 继续提升？

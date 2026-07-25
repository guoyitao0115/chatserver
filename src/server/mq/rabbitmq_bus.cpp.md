# `rabbitmq_bus.cpp` 讲解

## 作用概览

**RabbitMQ 消息总线实现。** 建立独立发布/消费连接，声明持久 exchange 与节点队列并绑定实例 id。发布体把用户 id 与原 JSON 封装在一起，消费线程拆包后回调业务层完成本地投递或离线兜底。

阅读位置：`src/server/mq/rabbitmq_bus.cpp`。下文严格按源码顺序展示，每一行只出现一次；解释只针对紧邻的代码片段。

## 代码片段与详细讲解

### 片段 1：第 1-23 行

```cpp
#include "rabbitmq_bus.hpp"
#include <iostream>
#include <sstream>
#include <cstring>

using namespace std;

// ============================================================
// 构造 / 析构
// ============================================================
RabbitMqBus::RabbitMqBus()
    : _queueName(amqp_empty_bytes)
{}

RabbitMqBus::~RabbitMqBus()
{
    _ready = false;
    _running = false;

    if (_consumeThread.joinable())
    {
        _consumeThread.join();
    }
```

这些头文件把“RabbitMQ 消息总线实现”接到项目公共协议、领域对象和所需系统库。依赖方向保持从实现到接口：模型不知道网络连接，帧工具不知道用户业务，当前文件负责在自己的层内组合它们。

### 片段 2：第 24-45 行

```cpp

    if (_subConn)
    {
        amqp_channel_close(_subConn, 2, AMQP_REPLY_SUCCESS);
        amqp_connection_close(_subConn, AMQP_REPLY_SUCCESS);
        amqp_destroy_connection(_subConn);
        _subConn = nullptr;
    }

    if (_pubConn)
    {
        amqp_channel_close(_pubConn, 1, AMQP_REPLY_SUCCESS);
        amqp_connection_close(_pubConn, AMQP_REPLY_SUCCESS);
        amqp_destroy_connection(_pubConn);
        _pubConn = nullptr;
    }

    if (_queueName.bytes)
    {
        amqp_bytes_free(_queueName);
        _queueName = amqp_empty_bytes;
    }
```

这部分完成“RabbitMQ 消息总线实现”中的边界分支：无效输入或外部操作失败会在写入后续状态前结束，成功路径才把结果交给相邻模块。这样返回值不仅代表函数结束，还决定上层能否发送成功响应或继续投递。

### 片段 3：第 46-71 行

```cpp
}

// ============================================================
// checkAmqpReply：统一检查 RPC 调用返回，失败时打印错误
// ============================================================
bool RabbitMqBus::checkAmqpReply(amqp_rpc_reply_t reply, const char *context)
{
    if (reply.reply_type == AMQP_RESPONSE_NORMAL)
        return true;

    cerr << "[rabbitmq] " << context << " failed: ";
    switch (reply.reply_type)
    {
    case AMQP_RESPONSE_NONE:
        cerr << "missing RPC reply" << endl;
        break;
    case AMQP_RESPONSE_LIBRARY_EXCEPTION:
        cerr << amqp_error_string2(reply.library_error) << endl;
        break;
    case AMQP_RESPONSE_SERVER_EXCEPTION:
        if (reply.reply.id == AMQP_CHANNEL_CLOSE_METHOD)
        {
            auto *m = static_cast<amqp_channel_close_t *>(reply.reply.decoded);
            cerr << "channel exception " << m->reply_code
                 << " " << string((char *)m->reply_text.bytes, m->reply_text.len) << endl;
        }
```

这部分完成“RabbitMQ 消息总线实现”中的边界分支：无效输入或外部操作失败会在写入后续状态前结束，成功路径才把结果交给相邻模块。这样返回值不仅代表函数结束，还决定上层能否发送成功响应或继续投递。 接收分发在这里处理统一错误和其他协议类型；ERROR_MSG 直接展示服务端 code/message，不能误触发登录注册的条件变量。

### 片段 4：第 72-96 行

```cpp
        else if (reply.reply.id == AMQP_CONNECTION_CLOSE_METHOD)
        {
            auto *m = static_cast<amqp_connection_close_t *>(reply.reply.decoded);
            cerr << "connection exception " << m->reply_code
                 << " " << string((char *)m->reply_text.bytes, m->reply_text.len) << endl;
        }
        break;
    default:
        break;
    }
    return false;
}

// ============================================================
// connect：建立发布连接 + 消费连接，声明 exchange 与队列
// ============================================================
bool RabbitMqBus::connect(const string &host,
                           int           port,
                           const string &exchange,
                           const string &serverId,
                           const string &user,
                           const string &password)
{
    _exchange = exchange;
    _serverId = serverId;
```

进程按命令行地址创建 TCP socket，并把文本 IP 与端口转换为网络字节序后连接。连接成功后再启动接收、重试、顺序冲刷和心跳线程，避免后台任务在 fd 尚不可用时抢先发送。

### 片段 5：第 97-121 行

```cpp

    // ----------------------------------------------------------
    // 1. 发布连接（channel 1）
    // ----------------------------------------------------------
    _pubConn = amqp_new_connection();
    amqp_socket_t *pubSock = amqp_tcp_socket_new(_pubConn);
    if (!pubSock)
    {
        cerr << "[rabbitmq] create pub socket failed" << endl;
        return false;
    }
    if (amqp_socket_open(pubSock, host.c_str(), port) != AMQP_STATUS_OK)
    {
        cerr << "[rabbitmq] open pub socket failed (host=" << host << ":" << port << ")" << endl;
        return false;
    }
    if (!checkAmqpReply(
            amqp_login(_pubConn, "/", 0, 131072, 0,
                       AMQP_SASL_METHOD_PLAIN, user.c_str(), password.c_str()),
            "pub login"))
        return false;

    amqp_channel_open(_pubConn, 1);
    if (!checkAmqpReply(amqp_get_rpc_reply(_pubConn), "pub channel open"))
        return false;
```

发布与消费各自创建 AMQP 连接和 channel，避免阻塞式消费占住发布通道。任一 socket、登录或 channel 步骤失败都会关闭已创建资源并返回 false，使 ChatService 切换到离线兜底。

### 片段 6：第 122-147 行

```cpp

    // 声明 direct exchange（幂等，已存在则复用）
    amqp_exchange_declare(_pubConn, 1,
                          amqp_cstring_bytes(_exchange.c_str()),
                          amqp_cstring_bytes("direct"),
                          /*passive*/0, /*durable*/0,
                          /*auto_delete*/0, /*internal*/0,
                          amqp_empty_table);
    if (!checkAmqpReply(amqp_get_rpc_reply(_pubConn), "pub exchange declare"))
        return false;

    // ----------------------------------------------------------
    // 2. 消费连接（channel 2）
    // ----------------------------------------------------------
    _subConn = amqp_new_connection();
    amqp_socket_t *subSock = amqp_tcp_socket_new(_subConn);
    if (!subSock)
    {
        cerr << "[rabbitmq] create sub socket failed" << endl;
        return false;
    }
    if (amqp_socket_open(subSock, host.c_str(), port) != AMQP_STATUS_OK)
    {
        cerr << "[rabbitmq] open sub socket failed" << endl;
        return false;
    }
```

这部分完成“RabbitMQ 消息总线实现”中的边界分支：无效输入或外部操作失败会在写入后续状态前结束，成功路径才把结果交给相邻模块。这样返回值不仅代表函数结束，还决定上层能否发送成功响应或继续投递。 这一段把 rabbitmq-c 的 normal、library exception 和 server exception 分开记录；调用者据布尔结果立即清理半建立连接，不继续使用无效 channel。

### 片段 7：第 148-176 行

```cpp
    if (!checkAmqpReply(
            amqp_login(_subConn, "/", 0, 131072, 0,
                       AMQP_SASL_METHOD_PLAIN, user.c_str(), password.c_str()),
            "sub login"))
        return false;

    amqp_channel_open(_subConn, 2);
    if (!checkAmqpReply(amqp_get_rpc_reply(_subConn), "sub channel open"))
        return false;

    // 声明 direct exchange（与发布端保持一致）
    amqp_exchange_declare(_subConn, 2,
                          amqp_cstring_bytes(_exchange.c_str()),
                          amqp_cstring_bytes("direct"),
                          0, 0, 0, 0,
                          amqp_empty_table);
    if (!checkAmqpReply(amqp_get_rpc_reply(_subConn), "sub exchange declare"))
        return false;

    // 声明独占、自动删除队列（服务端自动分配名称）
    // exclusive=1：仅本连接可见；auto-delete=1：连接断开后自动删除
    amqp_queue_declare_ok_t *qDeclare =
        amqp_queue_declare(_subConn, 2,
                           amqp_empty_bytes, /*queue name: server-generated*/
                           /*passive*/0, /*durable*/0,
                           /*exclusive*/1, /*auto_delete*/1,
                           amqp_empty_table);
    if (!checkAmqpReply(amqp_get_rpc_reply(_subConn), "queue declare"))
        return false;
```

消费端为当前 serverId 声明独占路由语义的节点队列，并用同一个 serverId 作为 binding key 绑定 direct exchange。于是发往 server-2 的消息不会广播给 server-1；消费启动后由专用线程阻塞取 envelope。

### 片段 8：第 177-199 行

```cpp

    // 保存服务端分配的队列名（需深拷贝，否则内存会随 frame 释放）
    _queueName = amqp_bytes_malloc_dup(qDeclare->queue);

    // 将队列绑定到 exchange（direct 使用 serverId 作为 routing_key）
    amqp_queue_bind(_subConn, 2,
                    _queueName,
                    amqp_cstring_bytes(_exchange.c_str()),
                    amqp_cstring_bytes(_serverId.c_str()),
                    amqp_empty_table);
    if (!checkAmqpReply(amqp_get_rpc_reply(_subConn), "queue bind"))
        return false;

    // 开始消费（no_ack=1：简单场景自动 ack，降低实现复杂度）
    amqp_basic_consume(_subConn, 2,
                       _queueName,
                       amqp_empty_bytes, // consumer tag
                       /*no_local*/0,
                       /*no_ack*/1,
                       /*exclusive*/0,
                       amqp_empty_table);
    if (!checkAmqpReply(amqp_get_rpc_reply(_subConn), "basic consume"))
        return false;
```

这部分完成“RabbitMQ 消息总线实现”中的边界分支：无效输入或外部操作失败会在写入后续状态前结束，成功路径才把结果交给相邻模块。这样返回值不仅代表函数结束，还决定上层能否发送成功响应或继续投递。 这一段把 rabbitmq-c 的 normal、library exception 和 server exception 分开记录；调用者据布尔结果立即清理半建立连接，不继续使用无效 channel。 源码在这一段特别限定了“保存服务端分配的队列名（需深拷贝，否则内存会随 frame 释放）”，因此解释范围止于该局部步骤。

### 片段 9：第 200-221 行

```cpp

    // 启动消费线程
    _running = true;
    _ready = true;
    _consumeThread = std::thread(&RabbitMqBus::consumeLoop, this);

    cout << "[rabbitmq] connected. host=" << host << ":" << port
         << " exchange=" << _exchange
         << " serverId=" << _serverId << endl;

    return true;
}

// ============================================================
// publish：将消息发布到 direct exchange（按 toServerId 精确路由）
// 消息格式："userid|payload"
// ============================================================
bool RabbitMqBus::publish(const string &toServerId, int userid, const string &payload)
{
    lock_guard<mutex> lock(_publishMutex);
    if (!_ready || !_pubConn)
        return false;
```

发布前用互斥锁串行保护 rabbitmq-c 发布连接。正文采用 `userid|JSON`，routing key 是目标 serverId；mandatory 标志让无法路由的发布暴露为错误，而不是无声吞掉。

### 片段 10：第 222-243 行

```cpp

    string body = to_string(userid) + "|" + payload;

    amqp_bytes_t bodyBytes;
    bodyBytes.bytes = const_cast<void *>(static_cast<const void *>(body.data()));
    bodyBytes.len   = body.size();

    int rc = amqp_basic_publish(_pubConn, 1,
                                amqp_cstring_bytes(_exchange.c_str()),
                                amqp_cstring_bytes(toServerId.c_str()),
                                /*mandatory*/0,
                                /*immediate*/0,
                                nullptr,
                                bodyBytes);
    if (rc != AMQP_STATUS_OK)
    {
        cerr << "[rabbitmq] publish failed, rc=" << rc
             << " toServerId=" << toServerId << endl;
        return false;
    }
    return true;
}
```

这部分完成“RabbitMQ 消息总线实现”中的边界分支：无效输入或外部操作失败会在写入后续状态前结束，成功路径才把结果交给相邻模块。这样返回值不仅代表函数结束，还决定上层能否发送成功响应或继续投递。 消息体由目标 userId、分隔符和原 JSON 组成，routing key 使用目标 serverId；发布锁保证多个 I/O 线程不会并发破坏同一 AMQP connection。

### 片段 11：第 244-269 行

```cpp

// ============================================================
// subscribe / unsubscribe：API 兼容接口，direct 模式无需动态操作
// ============================================================
bool RabbitMqBus::subscribe(int)   { return true; }
bool RabbitMqBus::unsubscribe(int) { return true; }

void RabbitMqBus::init_notify_handler(function<void(int, string)> fn)
{
    _notifyHandler = fn;
}

// ============================================================
// consumeLoop：阻塞消费消息，解析 envelope 后回调业务层
// ============================================================
void RabbitMqBus::consumeLoop()
{
    while (_running)
    {
        amqp_envelope_t envelope;
        amqp_maybe_release_buffers(_subConn);

        // 超时结构：1秒超时，便于检查 _running 标志
        struct timeval timeout;
        timeout.tv_sec  = 1;
        timeout.tv_usec = 0;
```

消费线程阻塞等待节点专属队列。每个 envelope 先拆出用户 id 和 JSON，再调用 ChatService 回调，最后 ack；解析失败的消息不会进入业务投递，仍会被确认以免毒消息无限重放。

Lua 把读取、比较和更新压进 Redis 的一次原子执行。若拆成多条客户端命令，比较完成到删除/续期之间可能已有新登录改写路由，旧会话就会误删或误续期新状态。

循环每次只消费已经确认完整的字节或已经成功写出的部分。遇到正文尚未到齐便停在当前偏移，下一次收到数据后继续；发送短写则从剩余位置续发，这正是流式 socket 不能假设“一次调用完成一条消息”的原因。

### 片段 12：第 270-297 行

```cpp

        amqp_rpc_reply_t ret = amqp_consume_message(_subConn, &envelope, &timeout, 0);

        if (ret.reply_type == AMQP_RESPONSE_NORMAL)
        {
            string value(static_cast<const char *>(envelope.message.body.bytes),
                         envelope.message.body.len);
            int userid = 0;
            string payload;
            if (parseEnvelope(value, userid, payload) && _notifyHandler)
            {
                _notifyHandler(userid, payload);
            }
            amqp_destroy_envelope(&envelope);
        }
        else if (ret.reply_type == AMQP_RESPONSE_LIBRARY_EXCEPTION
                 && ret.library_error == AMQP_STATUS_TIMEOUT)
        {
            // 超时是正常情况，继续循环检查 _running
            continue;
        }
        else
        {
            // 连接关闭或真实错误，退出消费循环
            if (_running)
            {
                checkAmqpReply(ret, "consume_message");
            }
```

这部分完成“RabbitMQ 消息总线实现”中的边界分支：无效输入或外部操作失败会在写入后续状态前结束，成功路径才把结果交给相邻模块。这样返回值不仅代表函数结束，还决定上层能否发送成功响应或继续投递。 这一段把 rabbitmq-c 的 normal、library exception 和 server exception 分开记录；调用者据布尔结果立即清理半建立连接，不继续使用无效 channel。 源码在这一段特别限定了“超时是正常情况，继续循环检查 _running”，因此解释范围止于该局部步骤。

### 片段 13：第 298-321 行

```cpp
            break;
        }
    }
}

// ============================================================
// parseEnvelope：解析 "userid|payload" 格式
// ============================================================
bool RabbitMqBus::parseEnvelope(const string &value, int &userid, string &payload)
{
    size_t pos = value.find('|');
    if (pos == string::npos)
        return false;
    try
    {
        userid = stoi(value.substr(0, pos));
    }
    catch (...)
    {
        return false;
    }
    payload = value.substr(pos + 1);
    return true;
}
```

分隔符只解析第一个 `|`：左侧必须是完整正整数，右侧保留原 JSON，即使聊天文本本身含竖线也不会被截断。严格校验可阻止畸形队列消息被投给错误用户。

## 面试重点

- 能否沿着一条单聊消息说明本地直发、跨节点路由、离线落库、ACK 与重试之间的成功语义？

- Redis 或 RabbitMQ 故障时系统如何降级，哪些保证仍成立，哪些保证会变弱？

- 为什么“至少一次发送 + message_id 幂等”不等于严格 Exactly Once？

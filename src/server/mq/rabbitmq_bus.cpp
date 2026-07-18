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

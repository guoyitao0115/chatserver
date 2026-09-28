#include "rabbitmq_bus.hpp"

#include <amqp_framing.h>
#include <chrono>
#include <cstring>
#include <exception>
#include <iostream>
#include <thread>
#include <utility>

using namespace std;

namespace
{
constexpr int PUBLISH_CONFIRM_TIMEOUT_SEC = 3;
constexpr int RECONNECT_DELAY_MS = 1000;
constexpr int SOCKET_CONNECT_TIMEOUT_SEC = 2;

void waitForReconnectDelay(const atomic_bool &running)
{
    // 拆成 100 ms 小段，使析构停止消费线程时不必等待完整退避周期。
    for (int waited = 0; running && waited < RECONNECT_DELAY_MS; waited += 100)
    {
        this_thread::sleep_for(chrono::milliseconds(100));
    }
}
}

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

bool RabbitMqBus::checkAmqpReply(amqp_rpc_reply_t reply, const char *context)
{
    if (reply.reply_type == AMQP_RESPONSE_NORMAL)
    {
        return true;
    }

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
            auto *method = static_cast<amqp_channel_close_t *>(reply.reply.decoded);
            cerr << "channel exception " << method->reply_code << " "
                 << string(static_cast<char *>(method->reply_text.bytes),
                           method->reply_text.len)
                 << endl;
        }
        else if (reply.reply.id == AMQP_CONNECTION_CLOSE_METHOD)
        {
            auto *method = static_cast<amqp_connection_close_t *>(reply.reply.decoded);
            cerr << "connection exception " << method->reply_code << " "
                 << string(static_cast<char *>(method->reply_text.bytes),
                           method->reply_text.len)
                 << endl;
        }
        else
        {
            cerr << "server method=" << reply.reply.id << endl;
        }
        break;
    default:
        cerr << "unknown reply type" << endl;
        break;
    }
    return false;
}

void RabbitMqBus::closePublisher()
{
    _publisherReady = false;
    if (_pubConn)
    {
        // 错误连接可能已无法完成 AMQP close 握手，直接销毁本地状态可避免再次阻塞。
        amqp_destroy_connection(_pubConn);
        _pubConn = nullptr;
    }
}

void RabbitMqBus::closeSubscriber()
{
    _subscriberReady = false;
    if (_subConn)
    {
        // 未 ACK 的消息会在连接关闭后由 broker 自动重新入队。
        amqp_destroy_connection(_subConn);
        _subConn = nullptr;
    }
}

bool RabbitMqBus::connectPublisher()
{
    closePublisher();

    _pubConn = amqp_new_connection();
    if (!_pubConn)
    {
        cerr << "[rabbitmq] create publisher connection failed" << endl;
        return false;
    }

    amqp_socket_t *socket = amqp_tcp_socket_new(_pubConn);
    struct timeval connectTimeout;
    connectTimeout.tv_sec = SOCKET_CONNECT_TIMEOUT_SEC;
    connectTimeout.tv_usec = 0;
    if (!socket
        || amqp_socket_open_noblock(
               socket, _host.c_str(), _port, &connectTimeout) != AMQP_STATUS_OK)
    {
        cerr << "[rabbitmq] open publisher socket failed host="
             << _host << ":" << _port << endl;
        closePublisher();
        return false;
    }

    if (!checkAmqpReply(
            amqp_login(_pubConn, "/", 0, 131072, 0,
                       AMQP_SASL_METHOD_PLAIN, _user.c_str(), _password.c_str()),
            "publisher login"))
    {
        closePublisher();
        return false;
    }

    amqp_channel_open(_pubConn, 1);
    if (!checkAmqpReply(amqp_get_rpc_reply(_pubConn), "publisher channel open"))
    {
        closePublisher();
        return false;
    }

    // 保持 exchange 与旧部署兼容；durable 节点队列和 persistent 消息负责 broker
    // 正常运行期间的积压恢复，断线重连时会幂等地重新声明 exchange 和 binding。
    amqp_exchange_declare(_pubConn, 1,
                          amqp_cstring_bytes(_exchange.c_str()),
                          amqp_cstring_bytes("direct"),
                          /*passive*/0, /*durable*/0,
                          /*auto_delete*/0, /*internal*/0,
                          amqp_empty_table);
    if (!checkAmqpReply(amqp_get_rpc_reply(_pubConn), "publisher exchange declare"))
    {
        closePublisher();
        return false;
    }

    amqp_confirm_select(_pubConn, 1);
    if (!checkAmqpReply(amqp_get_rpc_reply(_pubConn), "publisher confirm select"))
    {
        closePublisher();
        return false;
    }

    _publisherReady = true;
    cout << "[rabbitmq] publisher connected host=" << _host << ":" << _port << endl;
    return true;
}

bool RabbitMqBus::connectSubscriber()
{
    closeSubscriber();

    _subConn = amqp_new_connection();
    if (!_subConn)
    {
        cerr << "[rabbitmq] create subscriber connection failed" << endl;
        return false;
    }

    amqp_socket_t *socket = amqp_tcp_socket_new(_subConn);
    struct timeval connectTimeout;
    connectTimeout.tv_sec = SOCKET_CONNECT_TIMEOUT_SEC;
    connectTimeout.tv_usec = 0;
    if (!socket
        || amqp_socket_open_noblock(
               socket, _host.c_str(), _port, &connectTimeout) != AMQP_STATUS_OK)
    {
        cerr << "[rabbitmq] open subscriber socket failed host="
             << _host << ":" << _port << endl;
        closeSubscriber();
        return false;
    }

    if (!checkAmqpReply(
            amqp_login(_subConn, "/", 0, 131072, 0,
                       AMQP_SASL_METHOD_PLAIN, _user.c_str(), _password.c_str()),
            "subscriber login"))
    {
        closeSubscriber();
        return false;
    }

    amqp_channel_open(_subConn, 2);
    if (!checkAmqpReply(amqp_get_rpc_reply(_subConn), "subscriber channel open"))
    {
        closeSubscriber();
        return false;
    }

    amqp_exchange_declare(_subConn, 2,
                          amqp_cstring_bytes(_exchange.c_str()),
                          amqp_cstring_bytes("direct"),
                          /*passive*/0, /*durable*/0,
                          /*auto_delete*/0, /*internal*/0,
                          amqp_empty_table);
    if (!checkAmqpReply(amqp_get_rpc_reply(_subConn), "subscriber exchange declare"))
    {
        closeSubscriber();
        return false;
    }

    amqp_queue_declare(_subConn, 2,
                       amqp_cstring_bytes(_queueName.c_str()),
                       /*passive*/0, /*durable*/1,
                       /*exclusive*/0, /*auto_delete*/0,
                       amqp_empty_table);
    if (!checkAmqpReply(amqp_get_rpc_reply(_subConn), "durable queue declare"))
    {
        closeSubscriber();
        return false;
    }

    amqp_queue_bind(_subConn, 2,
                    amqp_cstring_bytes(_queueName.c_str()),
                    amqp_cstring_bytes(_exchange.c_str()),
                    amqp_cstring_bytes(_serverId.c_str()),
                    amqp_empty_table);
    if (!checkAmqpReply(amqp_get_rpc_reply(_subConn), "queue bind"))
    {
        closeSubscriber();
        return false;
    }

    amqp_basic_qos(_subConn, 2, 0, 64, 0);
    if (!checkAmqpReply(amqp_get_rpc_reply(_subConn), "consumer qos"))
    {
        closeSubscriber();
        return false;
    }

    amqp_basic_consume(_subConn, 2,
                       amqp_cstring_bytes(_queueName.c_str()),
                       amqp_empty_bytes,
                       /*no_local*/0,
                       /*no_ack*/0,
                       /*exclusive*/0,
                       amqp_empty_table);
    if (!checkAmqpReply(amqp_get_rpc_reply(_subConn), "manual-ack consume"))
    {
        closeSubscriber();
        return false;
    }

    _subscriberReady = true;
    cout << "[rabbitmq] subscriber connected queue=" << _queueName
         << " binding=" << _serverId << endl;
    return true;
}

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

RabbitMqBus::PublishConfirmResult RabbitMqBus::waitForPublisherConfirm()
{
    const auto deadline = chrono::steady_clock::now()
                        + chrono::seconds(PUBLISH_CONFIRM_TIMEOUT_SEC);

    while (chrono::steady_clock::now() < deadline)
    {
        const auto remaining = chrono::duration_cast<chrono::microseconds>(
            deadline - chrono::steady_clock::now());
        struct timeval timeout;
        timeout.tv_sec = static_cast<time_t>(remaining.count() / 1000000);
        timeout.tv_usec = static_cast<suseconds_t>(remaining.count() % 1000000);

        amqp_frame_t frame;
        const int status = amqp_simple_wait_frame_noblock(_pubConn, &frame, &timeout);
        if (status != AMQP_STATUS_OK)
        {
            cerr << "[rabbitmq] publisher confirm wait failed: "
                 << amqp_error_string2(status) << endl;
            return PublishConfirmResult::Failed;
        }
        if (frame.frame_type != AMQP_FRAME_METHOD)
        {
            continue;
        }

        if (frame.payload.method.id == AMQP_BASIC_ACK_METHOD)
        {
            return PublishConfirmResult::Confirmed;
        }
        if (frame.payload.method.id == AMQP_BASIC_NACK_METHOD)
        {
            cerr << "[rabbitmq] broker nacked published message" << endl;
            return PublishConfirmResult::Failed;
        }
        if (frame.payload.method.id == AMQP_BASIC_RETURN_METHOD)
        {
            auto *returned = static_cast<amqp_basic_return_t *>(
                frame.payload.method.decoded);
            cerr << "[rabbitmq] unroutable message code=" << returned->reply_code
                 << " text="
                 << string(static_cast<char *>(returned->reply_text.bytes),
                           returned->reply_text.len)
                 << endl;
            return PublishConfirmResult::Unroutable;
        }
        if (frame.payload.method.id == AMQP_CHANNEL_CLOSE_METHOD
            || frame.payload.method.id == AMQP_CONNECTION_CLOSE_METHOD)
        {
            cerr << "[rabbitmq] connection closed while waiting for confirm" << endl;
            return PublishConfirmResult::Failed;
        }
    }

    cerr << "[rabbitmq] publisher confirm timed out" << endl;
    return PublishConfirmResult::Failed;
}

bool RabbitMqBus::publish(const string &toServerId, int userid, const string &payload)
{
    lock_guard<mutex> lock(_publishMutex);
    if (!_publisherReady && !connectPublisher())
    {
        return false;
    }

    const string body = to_string(userid) + "|" + payload;
    amqp_bytes_t bodyBytes;
    bodyBytes.bytes = const_cast<char *>(body.data());
    bodyBytes.len = body.size();

    amqp_basic_properties_t properties;
    memset(&properties, 0, sizeof(properties));
    properties._flags = AMQP_BASIC_CONTENT_TYPE_FLAG
                      | AMQP_BASIC_DELIVERY_MODE_FLAG;
    properties.content_type = amqp_cstring_bytes("application/json");
    properties.delivery_mode = 2; // persistent

    const int status = amqp_basic_publish(
        _pubConn, 1,
        amqp_cstring_bytes(_exchange.c_str()),
        amqp_cstring_bytes(toServerId.c_str()),
        /*mandatory*/1,
        /*immediate*/0,
        &properties,
        bodyBytes);
    if (status == AMQP_STATUS_OK
        && waitForPublisherConfirm() == PublishConfirmResult::Confirmed)
    {
        return true;
    }

    // 确认超时具有歧义：消息可能已入队但 ACK 丢失。当前消息交给离线表兜底，
    // 接收端按 message_id 去重；关闭连接可丢弃 Basic.Return 的后续内容帧。
    closePublisher();
    // 尽力恢复连接供后续消息使用；当前消息不在此处重发，避免进一步扩大重复窗口。
    connectPublisher();
    return false;
}

bool RabbitMqBus::subscribe(int)
{
    return true;
}

bool RabbitMqBus::unsubscribe(int)
{
    return true;
}

void RabbitMqBus::init_notify_handler(function<bool(int, string)> fn)
{
    _notifyHandler = std::move(fn);
}

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

        const string value(
            static_cast<const char *>(envelope.message.body.bytes),
            envelope.message.body.len);
        int userid = 0;
        string payload;
        bool handled = false;
        bool poisonMessage = false;

        if (!parseEnvelope(value, userid, payload))
        {
            // 无法解析的消息无法通过重试恢复，确认后丢弃，避免 poison message 热循环。
            cerr << "[rabbitmq] invalid envelope, acknowledge and drop" << endl;
            poisonMessage = true;
            handled = true;
        }
        else if (!_notifyHandler)
        {
            // 回调缺失属于当前节点尚未准备好，重新入队而不是丢弃合法消息。
            cerr << "[rabbitmq] consumer callback is not ready, requeue message" << endl;
            handled = false;
        }
        else
        {
            try
            {
                handled = _notifyHandler(userid, payload);
            }
            catch (const exception &error)
            {
                cerr << "[rabbitmq] consumer callback failed: "
                     << error.what() << endl;
            }
            catch (...)
            {
                cerr << "[rabbitmq] consumer callback failed with unknown exception" << endl;
            }
        }

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
            cerr << "[rabbitmq] "
                 << (handled ? "ack" : "nack")
                 << " failed: " << amqp_error_string2(ackStatus) << endl;
            closeSubscriber();
            waitForReconnectDelay(_running);
            continue;
        }

        if (!handled && !poisonMessage)
        {
            // 数据库短暂失败时避免同一消息立刻高速重新投递。
            this_thread::sleep_for(chrono::milliseconds(200));
        }
    }
}

bool RabbitMqBus::parseEnvelope(const string &value, int &userid, string &payload)
{
    const size_t pos = value.find('|');
    if (pos == string::npos)
    {
        return false;
    }
    try
    {
        userid = stoi(value.substr(0, pos));
    }
    catch (...)
    {
        return false;
    }
    payload = value.substr(pos + 1);
    return userid > 0 && !payload.empty();
}

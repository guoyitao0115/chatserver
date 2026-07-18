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
    RabbitMqBus();
    ~RabbitMqBus();

    // 初始化连接、声明 exchange 与队列并启动消费线程
    // host      : RabbitMQ 地址，如 "127.0.0.1"
    // port      : 默认 5672
    // exchange  : direct exchange 名，如 "chat_cross_server"
    // serverId  : 本实例唯一路由ID（如 "server-1"）
    // user/pwd  : RabbitMQ 登录凭据
    bool connect(const string &host,
                 int           port,
                 const string &exchange,
                 const string &serverId,
                 const string &user     = "guest",
                 const string &password = "guest");

    // 发布跨节点消息（userid + payload），按 toServerId 精确路由
    bool publish(const string &toServerId, int userid, const string &payload);

    // API 兼容接口（direct 模式无需按用户动态订阅/退订）
    bool subscribe(int userid);
    bool unsubscribe(int userid);

    // 设置收到消息后的回调
    void init_notify_handler(function<void(int, string)> fn);

private:
    void consumeLoop();
    static bool parseEnvelope(const string &value, int &userid, string &payload);

    // 工具：检查 amqp_rpc_reply_t 并打印错误
    static bool checkAmqpReply(amqp_rpc_reply_t reply, const char *context);

private:
    string   _exchange;                           // direct exchange 名
    string   _serverId;                           // 当前实例路由ID（binding key）
    function<void(int, string)> _notifyHandler;   // 业务回调

    // 发布连接（producer）
    amqp_connection_state_t _pubConn  = nullptr;

    // 消费连接（consumer，独立连接避免与发布互扰）
    amqp_connection_state_t _subConn  = nullptr;
    amqp_bytes_t            _queueName;           // 服务器分配的独占队列名

    std::thread       _consumeThread;
    std::atomic_bool  _running{false};
    std::atomic_bool  _ready{false};
    // rabbitmq-c connection/channel cannot be published from multiple IO threads concurrently.
    std::mutex        _publishMutex;
};

#endif

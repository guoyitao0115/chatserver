#ifndef RABBITMQ_BUS_H
#define RABBITMQ_BUS_H

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
    RabbitMqBus() = default;
    // 先停止并 join 消费线程，再关闭消费/发布连接。
    ~RabbitMqBus();

    /**
     * 保存连接参数并尝试建立彼此独立的发布/消费连接。每个节点声明固定名称的
     * durable 队列并按 serverId 绑定；即使首次连接失败，消费线程也会继续重连。
     *
     * host/port/user/password 是连接参数；exchange 必须在各节点一致；serverId 必须
     * 在同时运行的节点间唯一。首次两条连接均成功返回 true，否则返回 false；后续
     * publish 和消费循环仍会自动重连。init_notify_handler() 应在本函数之前完成。
     */
    bool connect(const string &host,
                 int           port,
                 const string &exchange,
                 const string &serverId,
                 const string &user     = "guest",
                 const string &password = "guest");

    /**
     * 将持久化消息发布到 toServerId 对应的 direct routing_key，并同步等待 publisher
     * confirm。mandatory 路由失败、broker NACK、确认超时或连接错误均返回 false，
     * 上层随后写离线表。返回 true 表示 broker 已确认接收且消息存在目标队列路由，
     * 仍不代表最终客户端已经处理。
     */
    bool publish(const string &toServerId, int userid, const string &payload);

    // 保留旧业务接口；direct 模式以服务实例为粒度绑定，当前两个函数不操作 broker，恒为 true。
    bool subscribe(int userid);
    bool unsubscribe(int userid);

    // 回调返回 true 后才向 RabbitMQ 手动 ACK；返回 false 时 NACK 并 requeue。
    // 回调运行在专用消费线程，必须在 connect() 之前设置。
    void init_notify_handler(function<bool(int, string)> fn);

private:
    enum class PublishConfirmResult
    {
        Confirmed,
        Unroutable,
        Failed
    };

    // 分别建立发布连接与消费连接；失败时会释放本次创建的全部资源。
    bool connectPublisher();
    bool connectSubscriber();
    void closePublisher();
    void closeSubscriber();

    // 等待当前唯一在途发布的 broker confirm，并识别 mandatory Basic.Return。
    PublishConfirmResult waitForPublisherConfirm();

    // 消费线程在连接失败后每 1 秒重试，成功恢复后继续处理 durable 队列中的消息。
    void consumeLoop();

    // 解析第一个 '|'：前段必须能转换为用户 ID，后段原样作为 JSON payload 输出。
    // 成功返回 true；格式/数字错误返回 false，调用方会丢弃该无效队列消息。
    static bool parseEnvelope(const string &value, int &userid, string &payload);

    // 统一检查 RabbitMQ RPC 响应并输出连接/通道/库错误上下文；正常响应返回 true。
    static bool checkAmqpReply(amqp_rpc_reply_t reply, const char *context);

private:
    string   _host;
    int      _port = 5672;
    string   _user;
    string   _password;
    string   _exchange;                           // direct exchange 名，发布和消费连接必须相同
    string   _serverId;                           // 当前实例路由 ID，也是队列 binding key
    string   _queueName;                          // durable 队列名：chat.server.<serverId>
    function<bool(int, string)> _notifyHandler;   // true=业务处理成功，可确认消费

    // rabbitmq-c 连接由本对象独占；发布使用 channel 1。
    amqp_connection_state_t _pubConn  = nullptr;

    // 消费使用独立连接和 channel 2，避免阻塞 consume 与并发 publish 共用协议状态。
    amqp_connection_state_t _subConn  = nullptr;

    std::thread       _consumeThread;              // connect 调用后启动，断线期间继续运行
    std::atomic_bool  _running{false};             // 消费循环退出信号
    std::atomic_bool  _publisherReady{false};
    std::atomic_bool  _subscriberReady{false};
    // rabbitmq-c connection/channel cannot be published from multiple IO threads concurrently.
    std::mutex        _publishMutex;
};

#endif

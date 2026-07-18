#ifndef KAFKA_BUS_H
#define KAFKA_BUS_H

#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <memory>

// librdkafka C++ API
#include <rdkafkacpp.h>

using std::string;
using std::function;

/*
 * KafkaBus（方案A）：
 * - 仅替换“跨节点消息转发”链路（原 Redis Pub/Sub）
 * - Redis 仍保留用于去重键 SET NX EX
 *
 * 消息格式（value）："<userid>|<json_payload>"
 * - userid 作为分发目标用户ID
 * - json_payload 为原业务消息JSON字符串
 */
class KafkaBus
{
public:
    KafkaBus();
    ~KafkaBus();

    // 初始化 producer/consumer
    // brokers 示例："127.0.0.1:9092"
    // topic 示例："chat_cross_server"
    // groupId 示例："chatserver_group"
    // consumerInstanceId：当前chatserver实例ID（用于静态组成员标识，可选）
    bool connect(const string &brokers,
                 const string &topic,
                 const string &groupId,
                 const string &consumerInstanceId);

    // 发布跨节点消息（userid + payload）
    bool publish(int userid, const string &payload);

    // 仅保留API兼容语义：登录时调用，当前Kafka方案里无需动态订阅
    bool subscribe(int userid);

    // 仅保留API兼容语义：注销时调用，当前Kafka方案里无需动态退订
    bool unsubscribe(int userid);

    // 初始化消息上报回调（userid, payload）
    void init_notify_handler(function<void(int, string)> fn);

private:
    void consumeLoop();
    static bool parseEnvelope(const string &value, int &userid, string &payload);

private:
    string _topic;
    function<void(int, string)> _notify_message_handler;

    std::unique_ptr<RdKafka::Producer> _producer;
    std::unique_ptr<RdKafka::KafkaConsumer> _consumer;

    std::thread _consumeThread;
    std::atomic_bool _running{false};
};

#endif
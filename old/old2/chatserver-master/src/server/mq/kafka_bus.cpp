#include "kafka_bus.hpp"
#include <iostream>
#include <sstream>

using namespace std;

KafkaBus::KafkaBus() {}

KafkaBus::~KafkaBus()
{
    _running = false;
    if (_consumer)
    {
        _consumer->close();
    }
    if (_consumeThread.joinable())
    {
        _consumeThread.join();
    }
}

bool KafkaBus::connect(const string &brokers,
                       const string &topic,
                       const string &groupId,
                       const string &consumerInstanceId)
{
    _topic = topic;

    string errstr;

    // producer config
    unique_ptr<RdKafka::Conf> pconf(RdKafka::Conf::create(RdKafka::Conf::CONF_GLOBAL));
    if (!pconf || pconf->set("bootstrap.servers", brokers, errstr) != RdKafka::Conf::CONF_OK)
    {
        cerr << "[kafka] producer config bootstrap.servers failed: " << errstr << endl;
        return false;
    }
    // 可靠性关键：等待所有ISR确认
    pconf->set("acks", "all", errstr);
    // 允许重试（交由broker+client实现）
    pconf->set("retries", "10", errstr);

    _producer.reset(RdKafka::Producer::create(pconf.get(), errstr));
    if (!_producer)
    {
        cerr << "[kafka] create producer failed: " << errstr << endl;
        return false;
    }

    // consumer config
    unique_ptr<RdKafka::Conf> cconf(RdKafka::Conf::create(RdKafka::Conf::CONF_GLOBAL));
    if (!cconf || cconf->set("bootstrap.servers", brokers, errstr) != RdKafka::Conf::CONF_OK)
    {
        cerr << "[kafka] consumer config bootstrap.servers failed: " << errstr << endl;
        return false;
    }
    if (cconf->set("group.id", groupId, errstr) != RdKafka::Conf::CONF_OK)
    {
        cerr << "[kafka] consumer config group.id failed: " << errstr << endl;
        return false;
    }
    if (!consumerInstanceId.empty())
    {
        cconf->set("group.instance.id", consumerInstanceId, errstr);
    }

    // 业务处理后再自动提交位点（默认true），避免“未处理先提交”
    cconf->set("enable.auto.commit", "true", errstr);
    cconf->set("auto.offset.reset", "latest", errstr);

    _consumer.reset(RdKafka::KafkaConsumer::create(cconf.get(), errstr));
    if (!_consumer)
    {
        cerr << "[kafka] create consumer failed: " << errstr << endl;
        return false;
    }

    vector<string> topics{_topic};
    RdKafka::ErrorCode ec = _consumer->subscribe(topics);
    if (ec != RdKafka::ERR_NO_ERROR)
    {
        cerr << "[kafka] subscribe topic failed: " << RdKafka::err2str(ec) << endl;
        return false;
    }

    _running = true;
    _consumeThread = thread(&KafkaBus::consumeLoop, this);

    cout << "[kafka] connected. brokers=" << brokers
         << " topic=" << _topic
         << " group=" << groupId << endl;

    return true;
}

bool KafkaBus::publish(int userid, const string &payload)
{
    if (!_producer)
    {
        return false;
    }

    string value = to_string(userid) + "|" + payload;

    RdKafka::ErrorCode ec = _producer->produce(
        _topic,
        RdKafka::Topic::PARTITION_UA,
        RdKafka::Producer::RK_MSG_COPY,
        const_cast<char *>(value.data()),
        value.size(),
        nullptr,
        nullptr);

    if (ec != RdKafka::ERR_NO_ERROR)
    {
        cerr << "[kafka] produce failed: " << RdKafka::err2str(ec) << endl;
        return false;
    }

    _producer->poll(0);
    return true;
}

bool KafkaBus::subscribe(int)
{
    // Kafka在方案A中采用“固定topic + consumer group”模型，不做按用户动态订阅
    return true;
}

bool KafkaBus::unsubscribe(int)
{
    // 同上，保留接口以减少上层改动
    return true;
}

void KafkaBus::init_notify_handler(function<void(int, string)> fn)
{
    _notify_message_handler = fn;
}

void KafkaBus::consumeLoop()
{
    while (_running)
    {
        unique_ptr<RdKafka::Message> msg(_consumer->consume(1000));
        if (!msg)
        {
            continue;
        }

        if (msg->err() == RdKafka::ERR_NO_ERROR)
        {
            string value(static_cast<const char *>(msg->payload()), msg->len());
            int userid = 0;
            string payload;
            if (parseEnvelope(value, userid, payload) && _notify_message_handler)
            {
                _notify_message_handler(userid, payload);
            }
        }
        else if (msg->err() == RdKafka::ERR__TIMED_OUT)
        {
            continue;
        }
        else
        {
            cerr << "[kafka] consume error: " << msg->errstr() << endl;
        }
    }
}

bool KafkaBus::parseEnvelope(const string &value, int &userid, string &payload)
{
    size_t pos = value.find('|');
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
    return true;
}

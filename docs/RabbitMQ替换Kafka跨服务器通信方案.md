# RabbitMQ 替换 Kafka 跨服务器通信方案（当前生效版本）

## 一、改造目标

将项目中的跨节点消息总线从 Kafka 全量替换为 RabbitMQ，并保持原有业务语义不变：
- 跨节点消息投递仍采用“广播消费 + 本地在线过滤”；
- Redis 去重链路保持不变；
- 离线消息兜底逻辑保持不变；
- 上层 `ChatService` 调用方式基本不变（`publish/subscribe/unsubscribe` 兼容）。

---

## 二、改造后的通信模型

采用 RabbitMQ `fanout exchange`：
1. 服务端发布消息到 exchange `chat_cross_server`；
2. 每个服务实例声明独占队列并绑定到 exchange；
3. 每个实例都能收到消息；
4. 业务层按“目标用户是否在本节点在线”过滤投递。

这与原 Kafka 方案A（广播消费 + 本地过滤）在业务层语义上等价。

---

## 三、修改文件与行号（精确）

### 1）新增 RabbitMQ 总线封装

#### 文件
- `include/server/mq/rabbitmq_bus.hpp`
- `src/server/mq/rabbitmq_bus.cpp`

#### 关键行号
- `include/server/mq/rabbitmq_bus.hpp:9-10`：引入 `amqp.h` / `amqp_tcp_socket.h`
- `include/server/mq/rabbitmq_bus.hpp:32`：`class RabbitMqBus`
- `include/server/mq/rabbitmq_bus.hpp:43`：`connect(...)`
- `include/server/mq/rabbitmq_bus.hpp:50`：`publish(...)`
- `include/server/mq/rabbitmq_bus.hpp:53-54`：`subscribe/unsubscribe(...)`
- `include/server/mq/rabbitmq_bus.hpp:57`：`init_notify_handler(...)`
- `include/server/mq/rabbitmq_bus.hpp:60-64`：`consumeLoop/parseEnvelope/checkAmqpReply`

- `src/server/mq/rabbitmq_bus.cpp:15`：析构，安全释放连接与线程
- `src/server/mq/rabbitmq_bus.cpp:42`：释放队列名内存 `amqp_bytes_free`
- `src/server/mq/rabbitmq_bus.cpp:50`：`checkAmqpReply(...)`
- `src/server/mq/rabbitmq_bus.cpp:87`：`connect(...)`
- `src/server/mq/rabbitmq_bus.cpp:121/156`：声明 `fanout exchange`
- `src/server/mq/rabbitmq_bus.cpp:167`：声明独占队列
- `src/server/mq/rabbitmq_bus.cpp:179`：队列绑定 exchange
- `src/server/mq/rabbitmq_bus.cpp:188`：启动消费 `amqp_basic_consume`
- `src/server/mq/rabbitmq_bus.cpp:212`：`publish(...)`
- `src/server/mq/rabbitmq_bus.cpp:224`：`amqp_basic_publish(...)`
- `src/server/mq/rabbitmq_bus.cpp:253`：消费循环 `consumeLoop()`
- `src/server/mq/rabbitmq_bus.cpp:265`：`amqp_consume_message(...)`
- `src/server/mq/rabbitmq_bus.cpp:300`：`parseEnvelope(...)`

---

### 2）业务层 ChatService 替换接入

#### 文件
- `include/server/chatservice.hpp`
- `src/server/chatservice.cpp`

#### 关键行号
- `include/server/chatservice.hpp:20`：引入 `rabbitmq_bus.hpp`
- `include/server/chatservice.hpp:66`：回调改为 `handleRabbitMqBusMessage(...)`
- `include/server/chatservice.hpp:87`：路由注释改为 `local/rabbitmq/offline`
- `include/server/chatservice.hpp:116`：成员改为 `RabbitMqBus _rabbitMqBus`

- `src/server/chatservice.cpp:50`：注释改为连接 RabbitMQ
- `src/server/chatservice.cpp:52`：`_rabbitMqBus.connect("127.0.0.1", 5672, "chat_cross_server")`
- `src/server/chatservice.cpp:55-56`：注册回调 `handleRabbitMqBusMessage`
- `src/server/chatservice.cpp:162`：发布改为 `_rabbitMqBus.publish(...)`
- `src/server/chatservice.cpp:165`：路由标记改为 `route="rabbitmq"`
- `src/server/chatservice.cpp:170`：发布失败日志改为 RabbitMQ
- `src/server/chatservice.cpp:241`：登录后兼容调用 `_rabbitMqBus.subscribe(...)`
- `src/server/chatservice.cpp:393/423`：注销/异常下线调用 `_rabbitMqBus.unsubscribe(...)`
- `src/server/chatservice.cpp:559/569/581`：群聊路由统计改为 `cntRabbitMq` + `rabbitmq=`
- `src/server/chatservice.cpp:601`：注释改为“从RabbitMQ总线获取跨节点消息”
- `src/server/chatservice.cpp:606`：实现 `handleRabbitMqBusMessage(...)`
- `src/server/chatservice.cpp:616/621`：日志标签改为 `[rabbitmqMsg]`

---

### 3）构建系统链接库替换

#### 文件
- `src/server/CMakeLists.txt`

#### 行号
- `src/server/CMakeLists.txt:12`

#### 修改
- 移除：`rdkafka++ rdkafka`
- 新增：`rabbitmq`

---

### 4）删除 Kafka 旧实现文件

#### 文件
- 删除 `include/server/mq/kafka_bus.hpp`
- 删除 `src/server/mq/kafka_bus.cpp`

#### 说明
由于 `src/server/CMakeLists.txt` 使用 `aux_source_directory(./mq MQ_LIST)` 自动收集所有 `mq` 下源码，如果不删除旧文件，会导致编译阶段仍尝试编译 Kafka 代码并引入旧依赖。

---

## 四、关键代码与详细讲解

### 1）ChatService 初始化 RabbitMQ 总线

```cpp
if (_rabbitMqBus.connect("127.0.0.1", 5672, "chat_cross_server"))
{
    _rabbitMqBus.init_notify_handler(
        std::bind(&ChatService::handleRabbitMqBusMessage, this, _1, _2));
}
```

解释：
- 连接 RabbitMQ；
- 注册“收到跨节点消息时”的业务回调；
- 保持上层业务结构不变，只替换底层总线实现。

### 2）跨节点发布

```cpp
bool pubOk = _rabbitMqBus.publish(toUserId, msg);
if (pubOk)
{
    route = "rabbitmq";
    return;
}
```

解释：
- 目标用户在其他节点在线时，通过 RabbitMQ 发送；
- 成功即返回 `route=rabbitmq`；
- 失败进入离线落库兜底。

### 3）RabbitMQ fanout 架构实现点

```cpp
amqp_exchange_declare(..., amqp_cstring_bytes("fanout"), ...);
amqp_queue_declare(..., amqp_empty_bytes, ..., /*exclusive*/1, /*auto_delete*/1, ...);
amqp_queue_bind(..., _queueName, amqp_cstring_bytes(_exchange.c_str()), amqp_empty_bytes, ...);
```

解释：
- `fanout`：发布到 exchange 的消息广播给所有绑定队列；
- `exclusive + auto_delete`：每个 server 实例私有消费队列，进程结束自动回收；
- 业务层继续做“本地在线过滤”以避免重复投递。

### 4）消息格式兼容

```cpp
string body = to_string(userid) + "|" + payload;
```

```cpp
size_t pos = value.find('|');
userid = stoi(value.substr(0, pos));
payload = value.substr(pos + 1);
```

解释：
- 保持与原 Kafka 版本相同 envelope（`userid|payload`）；
- 减少上层协议变更，降低迁移风险。

### 5）析构安全性修复（线程与资源释放）

```cpp
_running = false;
if (_consumeThread.joinable()) { _consumeThread.join(); }
...
if (_queueName.bytes) { amqp_bytes_free(_queueName); }
```

解释：
- 先停消费线程，再关闭连接，避免潜在竞态；
- 释放 `amqp_bytes_malloc_dup` 分配的队列名内存，防止泄漏。

---

## 五、对现有文档的更新建议与执行结果

本次已实际更新：
1. `CHANGES.md`：追加 RabbitMQ 替换 Kafka 的变更章节；
2. `项目总结整合文档.md`：架构与跨节点章节更新为 RabbitMQ；
3. `readme1.md`：技术栈与关键特性改为 RabbitMQ；
4. `ChatServer学习教程.md`：分布式阶段改为 RabbitMQ；
5. `项目STAR法则简历文档.md`：项目叙事改为 RabbitMQ；
6. `实习面试优化建议.md`：亮点表述由 Kafka 替换为 RabbitMQ；
7. `Kafka替换Redis跨服务器通信方案A.md`：补充“已被 RabbitMQ 版本替代”的说明。

---

## 六、运行依赖说明

请确认系统安装 `rabbitmq-c` 开发库。
- Ubuntu: `sudo apt install librabbitmq-dev`
- CMake 已链接 `rabbitmq` 库（见 `src/server/CMakeLists.txt`）。

同时需要运行 RabbitMQ 服务（默认 `127.0.0.1:5672`，账号 `guest/guest`）。
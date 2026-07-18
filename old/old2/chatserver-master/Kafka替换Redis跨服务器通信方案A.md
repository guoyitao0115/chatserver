# Kafka替换Redis跨服务器通信（方案A）

## 一、改造目标

本次改造采用方案A：
- **只替换跨服务器转发链路**（原 Redis Pub/Sub -> Kafka）
- **保留 Redis 去重键能力**（`SET NX EX`）

即：
- 跨节点消息总线：Kafka
- 消息去重存储：Redis

这样做可以在改动可控的前提下，先拿到 Kafka 的持久化与 ACK 可靠性收益。

---

## 二、使用了 Kafka 的哪些功能

### 1）Topic 持久化日志
- 使用固定 topic：`chat_cross_server`
- 跨节点消息先写入 Kafka，再由各节点 consumer 消费

实现效果：
- 避免 Redis Pub/Sub 在订阅端短暂抖动时的“发出即丢”风险

### 2）Producer ACK 机制
- 设置 `acks=all`
- 设置 `retries=10`

实现效果：
- 提升“写入 broker 成功”的可靠性

### 3）Consumer Group 机制（方案A具体实现）
- 每个 server 进程使用独立 group（`chatserver_group_<pid>`）
- 这样每个实例都能收到 topic 消息（广播消费）
- 到达业务层后按“本地是否在线”过滤投递

实现效果：
- 保留你当前架构“按用户在线连接表路由”的习惯
- 避免一次性引入复杂的“精确节点路由”机制

### 4）持续消费线程
- 独立消费线程 `consumeLoop()`
- 解析消息信封 `<userid>|<json_payload>` 并回调业务层

实现效果：
- 与原 Redis observer 线程模型一致，便于平滑替换

---

## 三、相比原 Redis Pub/Sub 的优点

1. **可靠性更高**：消息先写日志，再消费，抗瞬时抖动能力更强。
2. **可恢复性更强**：具备 offset 与重试能力，不是“在线即收，离线即丢”。
3. **可扩展性更好**：后续可平滑演进到精确路由、重放与更细粒度监控。
4. **改造风险可控**：保留 Redis 去重，不动去重链路，先替换最痛的跨节点转发。

---

## 四、修改了哪些代码（文件与行号）

## 1）新增 Kafka 总线封装

### 文件
- `include/server/mq/kafka_bus.hpp`
- `src/server/mq/kafka_bus.cpp`

### 关键行号
- `include/server/mq/kafka_bus.hpp:25`：`class KafkaBus`
- `include/server/mq/kafka_bus.hpp:36`：`connect(...)`
- `include/server/mq/kafka_bus.hpp:42`：`publish(...)`
- `include/server/mq/kafka_bus.hpp:45/48`：`subscribe/unsubscribe`（兼容接口）
- `include/server/mq/kafka_bus.hpp:54/55`：`consumeLoop/parseEnvelope`

- `src/server/mq/kafka_bus.cpp:22`：`KafkaBus::connect(...)`
- `src/server/mq/kafka_bus.cpp:39`：`acks=all`
- `src/server/mq/kafka_bus.cpp:41`：`retries=10`
- `src/server/mq/kafka_bus.cpp:57`：`group.id`
- `src/server/mq/kafka_bus.cpp:79`：订阅 topic
- `src/server/mq/kafka_bus.cpp:105`：producer `produce(...)`
- `src/server/mq/kafka_bus.cpp:141`：`consumeLoop()`
- `src/server/mq/kafka_bus.cpp:172`：`parseEnvelope(...)`

### 详细讲解
- `connect(...)` 同时创建 producer + consumer。
- producer 配置 `acks=all` 确保 ISR 全确认，`retries=10` 增强发送成功率。
- consumer 订阅固定 topic，并启动独立线程拉取消息。
- value 使用轻量信封格式：`userid|payload`，业务层拿到后即可直接复用原消息处理逻辑。

---

## 2）ChatService 接入 Kafka，总线替换 Redis Pub/Sub

### 文件
- `include/server/chatservice.hpp`
- `src/server/chatservice.cpp`

### 关键行号
- `include/server/chatservice.hpp:66`：`handleKafkaBusMessage(...)`
- `include/server/chatservice.hpp:113`：`Redis _redis`（仅去重）
- `include/server/chatservice.hpp:116`：`KafkaBus _kafkaBus`

- `src/server/chatservice.cpp:47`：构造 groupId（`chatserver_group_<pid>`）
- `src/server/chatservice.cpp:48`：`_kafkaBus.connect(...)`
- `src/server/chatservice.cpp:52`：`_kafkaBus.init_notify_handler(...)`
- `src/server/chatservice.cpp:158`：`_kafkaBus.publish(...)`
- `src/server/chatservice.cpp:161`：`route = "kafka"`
- `src/server/chatservice.cpp:219`：登录时 `_kafkaBus.subscribe(id)`（兼容调用）
- `src/server/chatservice.cpp:350/380`：注销/异常下线 `_kafkaBus.unsubscribe(...)`
- `src/server/chatservice.cpp:516/526/538`：群聊路由统计改为 kafka
- `src/server/chatservice.cpp:563`：`handleKafkaBusMessage(...)`

### 详细讲解
- 构造函数中：
  - 保留 `_redis.connect()`，用于去重键（`markMessageIfFirst`）；
  - 新增 `_kafkaBus.connect(...)`，用于跨节点消息总线；
  - 注册 Kafka 消息回调到 `handleKafkaBusMessage`。

- `deliverMsg(...)` 中：
  - 本节点在线：仍然直推（不变）；
  - 其他节点在线：改为 Kafka 发布；
  - Kafka 发布失败：降级离线库兜底（不变思路）。

- `handleKafkaBusMessage(...)` 中：
  - 方案A采用“每实例都消费 -> 本地在线过滤”的策略；
  - 仅当目标用户在本节点连接表中才投递；
  - 不再把 Kafka 消息再次回灌到总线路径，避免回环发布。

---

## 3）构建系统接入 Kafka 库

### 文件
- `CMakeLists.txt`
- `src/server/CMakeLists.txt`

### 关键行号
- `CMakeLists.txt:16`：新增 `include/server/mq`
- `src/server/CMakeLists.txt:6`：新增 `aux_source_directory(./mq MQ_LIST)`
- `src/server/CMakeLists.txt:9`：将 `MQ_LIST` 加入 `ChatServer` 源文件
- `src/server/CMakeLists.txt:11`：新增链接库 `rdkafka++ rdkafka`

### 详细讲解
- 不改动原有 server 目录结构，新增 `mq` 子目录独立维护 Kafka 封装。
- 通过 CMake 增量接入，避免影响客户端构建。

---

## 五、改造后链路对比

## 改造前（Redis Pub/Sub）
1. A 节点判断目标用户在其他节点在线 -> Redis `PUBLISH`。
2. 对应节点订阅通道收到消息后投递。
3. 瞬时订阅异常存在丢失风险。

## 改造后（Kafka 方案A）
1. A 节点判断目标用户在其他节点在线 -> Kafka `produce`（`acks=all`）。
2. 各节点 consumer 拉取消息。
3. 业务层按本地在线用户过滤，仅本地命中节点完成投递。
4. 发布失败仍回退离线库，保持不丢消息兜底路径。

---

## 六、边界与说明（很重要）

1. 方案A是“可靠性优先 + 最小改造”，不是最终最优路由。
2. 当前使用“每实例独立 group 广播消费 + 本地过滤”，会有一定无效消费开销。
3. 后续可演进到“精确节点路由”（按 serverId 分区或 topic）。
4. Redis 仍保留且必要：用于 `message_id` 去重键，防重复投递。

---

## 七、最终收益

1. 跨节点通信可靠性明显提升（Kafka ACK + 持久化）。
2. 保持现有业务接口基本不变，迁移风险低。
3. 保留离线库兜底与去重机制，整体可靠性闭环更完整。

---

## 八、下一步操作建议（仅建议，暂不执行）

1. **Kafka 参数配置化**
   - 将 `brokers/topic/group` 从代码常量改为配置文件读取；
   - 建议放入独立配置（如 `chatserver.conf`），并支持环境区分（dev/test/prod）。

2. **启动前依赖检查**
   - 启动时校验 Kafka、Redis、MySQL 可达性；
   - 若关键依赖不可用，给出明确错误日志并中止启动，避免“半可用”状态。

3. **联调验证脚本化**
   - 准备最小联调步骤：2个 server + 2个 client；
   - 覆盖场景：跨节点单聊、跨节点群聊、目标节点重启后恢复消费。

4. **消费位点策略优化（可选）**
   - 当前方案A为最小改造，可先稳定运行；
   - 后续可演进为“手动提交位点（处理成功后提交）”，进一步强化消费语义。

5. **路由模型升级（可选）**
   - 当前是“广播消费 + 本地过滤”；
   - 后续可升级为“精确节点路由”（按 `serverId` 分区或 topic），降低无效消费与带宽开销。

6. **可观测性增强**
   - 增加 Kafka 关键指标日志：生产失败率、消费延迟、队列堆积、重试次数；
   - 便于压测与线上定位瓶颈。

7. **故障演练与回滚预案**
   - 设计“Kafka不可用”演练：确认离线库兜底生效；
   - 保留快速回滚开关（临时切回 Redis Pub/Sub）作为应急方案。
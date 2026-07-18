# changes.md

> 说明：本文件为最新合并版变更记录，已整合 `CHANGES.md`、`changes1.md`、`change2.md`，并以当前代码为准。
> 更新时间：2026-03-26

---

## 一、先回答：`msgdedup.cpp` 还有没有用？

当前项目里**没有** `msgdedup.cpp` 这个文件，只有头文件：
- `include/server/msgdedup.hpp`

并且这个头文件仍在使用（作为 Redis 异常时的本地去重兜底），因此**不能删**。

---

## 二、当前可靠性改造总览

1. 帧协议（解决 TCP 粘包/拆包）
2. 客户端->服务端 ACK 与 `message_id`
3. 客户端 pending + 超时重试
4. 离线消息落库安全增强
5. 统一投递路径（local / redis / offline）
6. 去重升级：Redis 优先 + 本地兜底（跨实例共享）

---

## 三、按“问题 -> 文件与行号 -> 解决方式”整理

### 问题1：TCP 粘包/拆包导致 JSON 解析不稳定

**文件与行号**
- `src/client/main.cpp:33` `sendFrame(...)`
- `src/client/main.cpp:64` `recvFrame(...)`
- `include/server/chatserver.hpp:45` `onFrameMessage(...)`
- `include/server/chatserver.hpp:53` `FrameCodec _codec`
- `include/server/chatserver.hpp:15` `#include <muduo/net/TcpConnection.h>`

**如何解决**
- 客户端发送改为 `[4字节长度头 + payload]` 帧格式。
- 客户端接收改为按帧读完整 payload。
- 服务端消息入口切成“两层”：
  - `onMessage` 只喂拆帧
  - `onFrameMessage` 只处理完整业务 JSON
- 补充 `TcpConnection.h` 保证 `TcpConnectionPtr` 类型可见。

---

### 问题2：客户端发送后不知道服务端是否收到

**文件与行号**
- `src/server/chatservice.cpp:105` `sendAck(...)`
- `src/server/chatservice.cpp:406` `oneChat` 去重入口
- `src/server/chatservice.cpp:496` `groupChat` 去重入口
- `src/server/chatservice.cpp:199/278/289/312/320` 响应统一帧编码发送

**如何解决**
- 服务端在处理聊天消息后返回 `MSG_ACK`。
- ACK 含 `message_id` 与 `ack_state`。
- 服务端发送统一走帧编码，避免客户端收包错位。

---

### 问题3：客户端 pending 只有记录，没有重试机制

**文件与行号**
- `src/client/main.cpp:97` `RETRY_TIMEOUT_SEC`
- `src/client/main.cpp:99` `MAX_RETRY_COUNT`
- `src/client/main.cpp:103` `addPending(...)`
- `src/client/main.cpp:111` `removePending(...)`
- `src/client/main.cpp:121` `retryTaskHandler(...)`
- `src/client/main.cpp:222` `g_retryClientFd = clientfd`

**如何解决**
- 每条待确认消息记录发送时间与重试次数。
- 后台线程每秒扫描一次：超时重发，达到次数上限后放弃并告警。

---

### 问题4：离线消息 SQL 拼接存在风险，失败日志不清晰

**文件与行号**
- `src/server/model/offlinemessagemodel.cpp:18` `insert(...)`
- `src/server/model/offlinemessagemodel.cpp:33` `mysql_real_escape_string(...)`
- `src/server/model/offlinemessagemodel.cpp:58` `remove(...)`
- `src/server/model/offlinemessagemodel.cpp:82` `query(...)`
- `src/server/chatservice.cpp:131` `deliverMsg(...)`
- `src/server/chatservice.cpp:170` `insertOk` 结果检查

**如何解决**
- 对消息内容做 `mysql_real_escape_string` 转义，降低拼接 SQL 风险。
- `insert` 返回 `bool`。
- 调用侧检查 `insertOk`，失败打错误日志，避免“静默丢消息”。

---

### 问题5：多实例或服务重启后，内存去重状态不共享

**文件与行号**
- `include/server/redis/redis.hpp:27` `markMessageIfFirst(...)`
- `src/server/redis/redis.cpp:67` `Redis::markMessageIfFirst(...)`
- `src/server/redis/redis.cpp:76` `SET %s 1 EX %d NX`
- `src/server/chatservice.cpp:74` `isDuplicateWithFallback(...)`
- `src/server/chatservice.cpp:406` `oneChat` 调用新去重策略
- `src/server/chatservice.cpp:496` `groupChat` 调用新去重策略

**如何解决**
- 去重改为 Redis 原子键：`SET key 1 EX ttl NX`
  - 首次写入：非重复
  - 已存在：重复
- Redis 不可用时，回退 `msgdedup.hpp` 的本地 LRU+TTL 去重。

---

## 四、注释整理说明（本次已做）

已对核心文件注释做统一整理，去掉了大量历史“改动X”标记，保留现在真正有意义的说明：
- `src/server/chatservice.cpp`
- `include/server/chatservice.hpp`
- `src/server/model/offlinemessagemodel.cpp`
- `src/client/main.cpp`
- `include/server/chatserver.hpp`

目标是：
1. 注释描述当前行为，而不是历史操作过程；
2. 让新读代码的人能快速理解关键链路；
3. 降低“注释很多但不可信”的维护成本。

---

## 五、兼容与边界

1. `msgdedup.hpp` 仍保留且有必要（Redis 失败时兜底）。
2. Redis 去重想跨 Redis 重启也生效，需要部署侧开启 AOF/RDB。
3. 服务端->客户端完整双向 ACK（接收方回执）目前仍是增强项，未在本次合并中实现。

---

## 六、本次合并涉及的历史文档

- 已整合来源：`CHANGES.md`、`changes1.md`、`change2.md`
- 本文件作为新的统一版本：`changes.md`

---

## 七、数据库注入问题检查与修复（追加）

### 问题
模型层存在直接将字符串拼接进 SQL 的写法，若输入包含单引号等特殊字符，存在 SQL 注入或语句破坏风险。

### 所修改的文件与行号
1. `src/server/model/usermodel.cpp`
   - `8`：`bool UserModel::insert(User &user)`
   - `21/22/23`：`mysql_real_escape_string(...)`（name/password/state）
   - `26`：`snprintf(...)` 生成安全 SQL
   - `71`：`bool UserModel::updateState(User user)`
   - `80`：`mysql_real_escape_string(...)`（state）
   - `86`：`snprintf(...)` 生成安全 SQL

2. `src/server/model/groupmodel.cpp`
   - `6`：`bool GroupModel::createGroup(Group &group)`
   - `16/17`：`mysql_real_escape_string(...)`（groupname/groupdesc）
   - `20`：`snprintf(...)` 生成安全 SQL
   - `34`：`void GroupModel::addGroup(int userid, int groupid, string role)`
   - `40`：`mysql_real_escape_string(...)`（role）
   - `46`：`snprintf(...)` 生成安全 SQL

### 解决的方法
- 对所有来自业务输入的字符串字段，先用 `mysql_real_escape_string` 转义，再拼接 SQL。
- 将不安全的 `sprintf` 替换为 `snprintf`，避免缓冲区越界风险。
- 仅保留数字参数（`id/groupid/userid`）的格式化直拼（整数类型无字符串注入面）。

### 补充说明
- `src/server/model/offlinemessagemodel.cpp` 之前已完成同类修复（`insert` 内使用 `mysql_real_escape_string`）。
- `src/server/model/friendmoel.cpp` 当前只处理整数 `userid/friendid`，未发现字符串注入入口。

---

## 八、消息按序显示问题（四次修复：按目标维度维护 client_seq）

### 问题
你指出了更深层的问题：
如果发送端 `client_seq` 采用“发送者全局递增”，那么同一发送者发往不同目标（如 A->B、A->C）会互相占用序号，导致接收方看到非连续序号，重排逻辑可能卡住。

### 所修改的文件与行号
1. `src/client/main.cpp`
   - `88-90`：新增按目标维度的序列表
   - `92`：`nextClientSeqForPeer(int toid)`
   - `100`：`nextClientSeqForGroup(int groupid)`
   - `638`：单聊发送改为 `nextClientSeqForPeer(friendid)`
   - `692`：群聊发送改为 `nextClientSeqForGroup(groupid)`

2. `src/client/main.cpp`（接收重排逻辑保持）
   - `191-193`：`g_lastShownSeq/g_orderBuffer`
   - `216`：`buildSessionKey(...)`
   - `241`：`printOrderedIncoming(...)`
   - `446/500/506`：离线与实时消息统一按序入口

### 解决的方法
- 将发送端序号策略从“发送者全局递增”改为“按目标维度递增”：
  - 单聊：每个 `toid` 一条独立序列；
  - 群聊：每个 `groupid` 一条独立序列。
- 接收端继续按 `client_seq` + 会话桶重排，避免跨会话序号干扰。

### 效果
- A 发给 B 的序号不会被 A 发给 C 的消息打断。
- A 在群 G1 的序号不会被 A 在群 G2 的消息打断。
- 消息按序显示更加稳定，满足接收端连续冲刷条件。

---

## 九、应用层心跳机制问题（新增）

### 问题
当前系统已经有 TCP 长连接和业务 ACK，但仍缺少“连接活性探测”。
在弱网、NAT 映射过期、链路半开（half-open）等场景下，连接可能已经不可用，但应用层短时间内感知不到，表现为：
- 客户端长时间静默，不知道连接是否还活着；
- 下一次真正发业务消息时才发现异常；
- 用户体感为“消息发出去没反应/延迟很久才报错”。

### 是否需要心跳
需要。

应用层心跳的价值不是替代 TCP，而是更快、更可控地发现“应用链路是否仍可用”。

### 如果没有心跳会有什么问题
1. **半开连接难以及时发现**：一端异常退出或中间网络丢失，另一端可能长时间不知情。
2. **用户体验变差**：直到下一条业务消息发送时才触发错误。
3. **状态感知滞后**：客户端无法区分“当前空闲”与“连接已失效”。

### 所修改的文件与行号
1. `include/public.hpp`
   - `42`：`HEARTBEAT_MSG`
   - `43`：`HEARTBEAT_MSG_ACK`

2. `include/server/chatservice.hpp`
   - `56`：新增 `heartbeat(const TcpConnectionPtr&, json&, Timestamp)`

3. `src/server/chatservice.cpp`
   - `38`：注册 `HEARTBEAT_MSG` 处理器
   - `539`：实现 `ChatService::heartbeat(...)`
   - `542`：返回 `HEARTBEAT_MSG_ACK`

4. `src/client/main.cpp`
   - `126`：`HEARTBEAT_INTERVAL_SEC = 10`
   - `127`：`g_heartbeatAcked`
   - `206`：`heartbeatTaskHandler(...)`
   - `356`：启动心跳线程
   - `553`：处理 `HEARTBEAT_MSG_ACK`

### 解决的方法
- 客户端每 `10s` 发送一次 `HEARTBEAT_MSG`（携带 `id` 和 `ts`）。
- 服务端收到后立即返回 `HEARTBEAT_MSG_ACK`（可回显 `echo_ts`）。
- 客户端收到 ACK 后将 `g_heartbeatAcked=true`。
- 若下一轮发送前上一轮仍未 ACK，则打印连接不稳定告警。

### 效果
- 应用层可以持续感知连接活性。
- 异常链路可以更早暴露，而不是等到业务消息发送时才发现。
- 为后续“自动重连/状态灯”能力提供基础。

---

## 十、Kafka替换Redis跨服务器通信（方案A）

### 问题
Redis Pub/Sub 在跨节点通信中是“在线转发”模型，订阅端短暂抖动时存在消息丢失风险。

### 目标
- 用 Kafka 替换 Redis Pub/Sub 作为跨节点消息总线；
- 保留 Redis 去重键能力（`SET NX EX`）；
- 保持原有业务层改动最小。

### 所修改的文件与行号
1. 新增 Kafka 总线封装
   - `include/server/mq/kafka_bus.hpp`：`25/36/42/45/48/54/55`
   - `src/server/mq/kafka_bus.cpp`：`22/39/41/57/79/105/141/172`

2. ChatService 接入 Kafka
   - `include/server/chatservice.hpp`：`66/113/116`
   - `src/server/chatservice.cpp`：`47/48/52/158/161/219/350/380/516/526/538/563`

3. 构建系统
   - `CMakeLists.txt`：`16`
   - `src/server/CMakeLists.txt`：`6/9/11`

### 解决的方法
- 新增 `KafkaBus` 封装 producer/consumer 与消费线程。
- producer 设置 `acks=all`、`retries=10`，增强写入可靠性。
- 方案A采用“每实例独立group广播消费 + 本地在线过滤投递”。
- `deliverMsg` 跨节点路径由 `_redis.publish(...)` 改为 `_kafkaBus.publish(...)`。
- Redis 仅保留用于 `markMessageIfFirst(...)` 去重键。

### 效果
- 跨节点转发可靠性提升（ACK + 持久化）。
- 保留离线库兜底与去重机制，整体可靠性更完整。
- 在最小改造前提下完成从 Redis Pub/Sub 到 Kafka 的迁移。

---

## 十一、雪花算法消息ID问题

### 问题
原 `message_id` 采用“用户ID + 毫秒时间 + 本地序号”字符串拼接方式，可用但标准化与可扩展性一般。

### 目标
- 使用 Snowflake 64 位分布式ID生成 `message_id`；
- 保持 ACK/重试/去重链路不变；
- 以最小改造提升唯一性与可观测性。

### 所修改的文件与行号
1. `src/client/main.cpp`
   - `87`：新增 `SnowflakeIdGenerator` 类
   - `93`：`setWorkerId(...)`
   - `125`：雪花ID位运算拼装
   - `149-155`：`EPOCH_MS/位宽/位移` 常量
   - `163`：全局 `g_snowflake`
   - `188-190`：`generateMsgId()` 改为雪花ID
   - `516`：登录成功后设置 `workerId`
   - `766-769`：单聊发送使用雪花 `message_id`
   - `820-823`：群聊发送使用雪花 `message_id`

2. 新增文档
   - `雪花算法消息ID问题.md`

### 解决的方法
- 在客户端实现线程安全 `SnowflakeIdGenerator`。
- `message_id` 改为 `to_string(g_snowflake.nextId())`。
- 登录成功后以 `userId & 0x3FF` 设置 workerId。
- 保持服务端与协议字段不变，继续复用现有 ACK、重试、去重流程。

### 效果
- `message_id` 全局唯一性与趋势递增能力更强。
- 代码改动小，兼容原有链路。
- 为后续多节点与链路追踪提供更标准ID基础。

---

## 十二、bcrypt密码哈希存储（含随机盐）

### 问题
用户密码原先以明文写入数据库，存在高安全风险。

### 目标
- 注册时改为 bcrypt 哈希入库（含随机盐）；
- 登录时改为 bcrypt 校验；
- 兼容历史明文账号并平滑升级。

### 所修改的文件与行号
1. 新增密码安全模块
   - `include/server/security/password_hasher.hpp`：`8/13/16/19`
   - `src/server/security/password_hasher.cpp`：`10/12/35/42/46/53/62/73`

2. 注册/登录链路改造
   - `src/server/chatservice.cpp`：`4/12/195/210/218/249-254/338/343`

3. 用户模型新增密码更新接口
   - `include/server/model/usermodel.hpp`：`16`
   - `src/server/model/usermodel.cpp`：`72/88`

4. 构建系统
   - `CMakeLists.txt`：`17`
   - `src/server/CMakeLists.txt`：`7/10/12`

### 解决的方法
- 新增 `PasswordHasher` 封装 bcrypt 哈希与校验。
- 注册时先 `hashBcrypt(...)` 再入库。
- 登录时优先走 `verifyBcrypt(...)`。
- 历史明文账号登录成功后，自动升级并回写 bcrypt 哈希。

### 效果
- 新增账号不再明文存储密码。
- 历史明文账号可逐步平滑迁移。
- 密码泄露风险显著下降，符合常见安全实践。

---

## 十三、RabbitMQ 替换 Kafka 跨节点消息总线

### 问题
跨节点总线原先依赖 Kafka 版本实现，当前代码已切换为 RabbitMQ，需要同步完成：
- 业务层引用替换；
- 构建依赖替换；
- 旧 Kafka 文件清理；
- 文档口径统一。

### 所修改的文件与行号
1. ChatService 业务层替换
   - `include/server/chatservice.hpp`：`20/66/87/116`
   - `src/server/chatservice.cpp`：`50/52/55/56/162/165/170/241/393/423/559/569/581/601/606/616/621`

2. RabbitMQ 总线实现（新增）
   - `include/server/mq/rabbitmq_bus.hpp`：`9/10/32/43/50/53/54/57/60/61/64`
   - `src/server/mq/rabbitmq_bus.cpp`：`15/42/50/87/121/156/167/179/188/212/224/253/265/300`

3. 构建系统
   - `src/server/CMakeLists.txt`：`12`

4. 清理旧实现
   - 删除 `include/server/mq/kafka_bus.hpp`
   - 删除 `src/server/mq/kafka_bus.cpp`

### 解决的方法
- 将 `ChatService` 的总线对象统一改为 `RabbitMqBus`；
- 将跨节点路由标识从 `kafka` 改为 `rabbitmq`；
- 使用 RabbitMQ `fanout exchange + 实例独占队列` 保持方案A语义（广播消费 + 本地过滤）；
- CMake 链接库由 `rdkafka++/rdkafka` 切换为 `rabbitmq`；
- 移除旧 Kafka 源码，避免 `aux_source_directory(./mq MQ_LIST)` 自动编译旧文件。

### 效果
- 跨节点总线已切换至 RabbitMQ，业务投递语义保持一致；
- 构建依赖与源码实现一致，不再混用 Kafka 依赖；
- 文档与代码口径统一，便于后续维护与面试说明。



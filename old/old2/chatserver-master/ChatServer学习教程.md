# ChatServer 学习教程（学习顺序版）

> 目标：告诉你“按什么顺序学”，每一步要看什么、练什么、产出什么。

---

## 总体学习路线

建议按 7 个阶段学习：
1. 项目全景与运行链路
2. 通信协议与网络层
3. 业务流程与数据模型
4. 可靠性机制
5. 分布式能力
6. 安全机制
7. 面试表达与复盘

---

## 阶段1：项目全景（先建立地图）

### 要看
- `src/server/main.cpp`
- `src/client/main.cpp`
- `include/server/chatserver.hpp`
- `src/server/chatservice.cpp`

### 要理解
- 服务端启动流程
- 客户端启动与命令交互流程
- 一条消息从发送到接收的完整路径

### 产出
- 画一张“消息生命周期图”（发送->服务端->路由->接收）。

---

## 阶段2：通信协议（高优先级）

### 要看
- 客户端 `sendFrame/recvFrame`
- 服务端 `FrameCodec` 与消息入口

### 要理解
- 为什么必须做长度帧协议
- 如何避免 TCP 粘包拆包

### 练习
- 手动构造错误帧，看系统如何表现
- 观察服务端日志对非法数据的处理

---

## 阶段3：核心业务（登录/注册/单聊/群聊）

### 要看
- `ChatService::login/reg/oneChat/groupChat`
- `UserModel/FriendModel/GroupModel/OfflineMsgModel`

### 要理解
- 登录成功后返回了哪些数据（好友、群、离线消息）
- 单聊与群聊的路由逻辑差异

### 练习
- 增加一个自定义业务命令（如“拉取在线人数”）

---

## 阶段4：可靠性机制（项目亮点）

### 要看
- ACK 消息结构与处理分支
- 客户端 `pending_map` 与 `retryTaskHandler`
- Redis 去重（`SET NX EX`）与本地兜底
- 消息按序显示（客户端重排）

### 要理解
- at-least-once 语义下为什么需要幂等去重
- 为什么“按目标维度”维护序号

### 练习
- 人为模拟 ACK 丢失，观察重试与去重是否生效

---

## 阶段5：分布式能力（Kafka）

### 要看
- `mq/kafka_bus.*`
- `ChatService::deliverMsg`、`handleKafkaBusMessage`
- `Kafka替换Redis跨服务器通信方案A.md`

### 要理解
- 为什么要从 Redis Pub/Sub 升级到 Kafka
- 方案A“广播消费+本地过滤”的权衡

### 练习
- 启动双服务实例，验证跨节点投递链路

---

## 阶段6：安全机制（bcrypt）

### 要看
- `security/password_hasher.*`
- `ChatService::reg/login`
- `UserModel::updatePassword`

### 要理解
- bcrypt 随机盐与 cost 的意义
- 历史明文平滑迁移策略

### 练习
- 在数据库对比新老用户密码字段格式变化

---

## 阶段7：复盘与面试准备

### 必读文档
- `项目全量扫描修复报告.md`
- `项目总结整合文档.md`
- `实习面试优化建议.md`

### 输出物
1. 一页架构图
2. 一页可靠性闭环图
3. 一页 STAR 项目讲稿（可参考 `项目STAR法则简历文档.md`）

---

## 推荐学习节奏（7天）

- Day1：跑通项目 + 总览
- Day2：协议与网络层
- Day3：业务与模型层
- Day4：ACK/重试/去重
- Day5：顺序显示 + 心跳
- Day6：Kafka + bcrypt
- Day7：复盘输出 + 面试演练

---

## 学习验收标准

当你能清晰回答以下问题，说明你学透了：
1. 为什么仅靠 TCP 还不够，必须要应用层 ACK？
2. 为什么要 Redis 去重 + 本地兜底？
3. 为什么序号要按目标维度维护？
4. Kafka 方案A为什么是“阶段性最优”而非“终局最优”？
5. bcrypt 改造如何兼容历史明文用户？
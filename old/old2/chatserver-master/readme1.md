# README1 - ChatServer 项目说明

## 1. 项目简介

`ChatServer` 是一个基于 `Muduo` 的 C++ 聊天系统，支持：
- 用户注册/登录/注销
- 单聊、群聊、好友、群组管理
- 离线消息存储与登录拉取
- 跨节点消息转发（Kafka 方案A）
- 消息去重（Redis 优先 + 本地兜底）
- 客户端 ACK + 超时重试
- 应用层心跳探测
- Snowflake 消息ID
- bcrypt 密码哈希存储（含随机盐）

---

## 2. 核心技术栈

- C++11/14
- Muduo 网络库
- MySQL C API
- Redis（hiredis）
- Kafka（librdkafka / rdkafka++）
- nlohmann/json
- CMake

---

## 3. 目录结构（核心）

- `src/server/`：服务端主逻辑
- `src/client/`：客户端主逻辑
- `src/server/model/`：数据模型层
- `src/server/redis/`：Redis 封装（含去重接口）
- `src/server/mq/`：Kafka 总线封装
- `src/server/security/`：密码哈希模块（bcrypt）
- `include/`：头文件
- `*.md`：问题分析、变更与总结文档

---

## 4. 关键特性说明

### 4.1 帧协议
使用 `[4字节长度头 + payload]` 解决 TCP 粘包拆包。

### 4.2 消息可靠性
客户端发送消息加入 pending；服务端返回 ACK；客户端超时重试。

### 4.3 消息去重
服务端优先使用 Redis `SET NX EX` 做跨实例去重，异常时降级本地去重。

### 4.4 跨节点通信
服务端通过 Kafka topic 进行跨节点消息转发，保留离线消息兜底。

### 4.5 安全
用户密码采用 bcrypt 哈希（随机盐）存储，历史明文支持登录后平滑迁移。

---

## 5. 构建说明（示例）

> 具体依赖安装请按你的系统环境调整。

```bash
mkdir build
cd build
cmake ..
make -j
```

生成目标通常包括：
- `ChatServer`
- `ChatClient`

---

## 6. 运行建议（本地联调）

1. 准备 MySQL、Redis、Kafka；
2. 初始化数据库表结构；
3. 启动一个或多个 `ChatServer` 实例；
4. 启动多个 `ChatClient` 验证单聊/群聊/跨节点场景；
5. 观察日志中的 ACK、重试、去重、心跳、Kafka 路由信息。

---

## 7. 推荐阅读文档

- `项目全量扫描修复报告.md`
- `项目总结整合文档.md`
- `实习面试优化建议.md`
- `消息按序显示问题.md`
- `雪花算法消息ID问题.md`
- `应用层心跳机制问题.md`
- `Kafka替换Redis跨服务器通信方案A.md`
- `bcrypt密码哈希存储问题.md`

---

## 8. 面试可讲的一句话亮点

我把一个基础聊天系统逐步升级为具备“分布式可靠性 + 安全性 + 可维护性”的工程化 IM 项目，能完整讲清问题、权衡与落地效果。
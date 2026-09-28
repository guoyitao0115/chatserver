# ChatServer

基于 C++17、Muduo、MySQL、Redis 与 RabbitMQ 的集群聊天项目，提供命令行客户端和轻量 Web 前端。

当前根目录是主版本，`old/` 只保存历史版本，不参与构建。

## 一键运行

需要 Docker Desktop 和 Node.js 20+：

```bash
docker compose up -d --build --wait
node web/test/full-stack.mjs
```

测试通过后打开：

- 主节点 Web 前端：<http://localhost:8080>
- 第二节点 Web 前端：<http://localhost:8081>

停止环境：

```bash
docker compose down
```

也可以执行 `./scripts/全链路测试.sh` 完成构建、启动和基础测试；执行
`./scripts/完整回归与压力测试.sh` 运行完整功能、可靠性和压力测试。

## 本地开发

只构建不依赖 Muduo/MySQL/Redis/RabbitMQ 的命令行客户端和核心测试：

```bash
cmake -S . -B build -DCHAT_BUILD_SERVER=OFF
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

测试 WebSocket/TCP 适配网关：

```bash
npm test --prefix web
```

## 目录

```text
include/        公共协议、服务端头文件
src/server/     Muduo 服务端、业务、数据访问和中间件适配
src/client/     C++ 命令行客户端
web/            静态 Web 前端与 WebSocket/TCP 网关
docker/         MySQL 初始化脚本
test/           C++ 核心单元测试
docs/           设计、学习和面试文档
old/            历史版本，仅用于对照
```

## 关键说明

- 浏览器不能直接连接原始 TCP 服务，`web/gateway.mjs` 只做 WebSocket 与四字节长度帧的协议转换，不承载聊天业务。
- Redis 负责共享去重键和 `userId -> serverId` 在线路由；RabbitMQ direct exchange
  使用 durable 节点队列、持久消息、publisher confirm、mandatory 路由检查和手动消费
  ACK 完成跨节点定向投递，连接异常时自动重连。
- RabbitMQ 发布或确认失败时同步写 MySQL 离线表；CLI 与 Web 接收端再按
  `message_id` 去重，处理确认丢失、重新入队和在线/离线双路径副本。
- MySQL 保存账户、关系、群组和离线消息。
- Docker Compose 凭据只用于本地演示，真实部署请通过环境变量或密钥管理系统替换。

详细内容见 `docs/1-项目修改说明.md` 至 `docs/6-完整测试与压力测试报告.md`。

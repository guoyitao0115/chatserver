# `compose.yaml` 讲解

## 作用概览

该文件编排完整的双节点聊天环境：MySQL、Redis、RabbitMQ、两个 C++ 服务实例和两个分别连接不同后端的 Web 网关，用于演示本地与跨节点消息链路。

## 按学习顺序讲解

1. `x-chatserver-env`：用 YAML anchor 复用数据库、Redis 和 RabbitMQ 配置，减少两个服务实例间的重复。
2. `mysql`：创建 `chat` 数据库、挂载初始化 SQL，并用健康检查阻止后端过早启动。
3. `redis`：开启 AOF 持久化并通过 `PING` 健康检查。
4. `rabbitmq`：设置专用账户，并等待 broker 可响应。
5. `chatserver-1/2`：使用同一镜像和依赖，但分别设置 `server-1`、`server-2`，使 Redis 路由与 RabbitMQ routing key 能区分节点。
6. `web/web-2`：分别代理两个 C++ 节点，并映射到宿主机 8080、8081，方便跨节点端到端测试。

## 面试重点

重要性高。可能问题：为什么 `depends_on` 配健康条件？容器启动不等于服务可用；消息如何从 8080 用户到 8081 用户？发送节点查 Redis 路由，再通过 RabbitMQ direct exchange 投递到目标节点。


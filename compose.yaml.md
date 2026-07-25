# `compose.yaml` 讲解

## 作用概览

**全链路容器编排。** 定义 MySQL、Redis、RabbitMQ、两个 ChatServer 节点和两个 Web 网关的网络、健康检查、端口与环境变量。双节点拓扑正是跨节点路由、去重和故障测试的运行基础。

阅读位置：`compose.yaml`。下文严格按源码顺序展示，每一行只出现一次；解释只针对紧邻的代码片段。

## 代码片段与详细讲解

### 片段 1：第 1-26 行

```yaml
# 两个 ChatServer 节点共享的连接参数。YAML anchor 只减少重复，不会创建服务。
x-chatserver-env: &chatserver-env
  CHAT_MYSQL_HOST: mysql
  CHAT_MYSQL_USER: root
  CHAT_MYSQL_PASSWORD: chat-root-password
  CHAT_MYSQL_DATABASE: chat
  CHAT_REDIS_HOST: redis
  CHAT_RABBITMQ_HOST: rabbitmq
  CHAT_RABBITMQ_USER: chat
  CHAT_RABBITMQ_PASSWORD: chat-password

services:
  # MySQL 保存用户、好友、群组以及离线消息；初始化脚本仅在新数据目录首次创建时执行。
  mysql:
    image: mysql:8.4
    environment:
      MYSQL_ROOT_PASSWORD: chat-root-password
      MYSQL_DATABASE: chat
    volumes:
      - ./docker/mysql/init.sql:/docker-entrypoint-initdb.d/01-chat.sql:ro
    # ChatServer 等到数据库真正可响应，而不是只等容器进程启动。
    healthcheck:
      test: ["CMD", "mysqladmin", "ping", "-h", "127.0.0.1", "-pchat-root-password"]
      interval: 3s
      timeout: 3s
      retries: 30
```

这里声明基础服务 `mysql`及其健康检查。ChatServer 的 depends_on 等待的是命令实际成功，不只是容器进程存在，因此初始化表、Redis PING 和 RabbitMQ 启动完成后才接收聊天流量。

### 片段 2：第 27-48 行

```yaml

  # Redis 保存带 TTL 的用户节点路由和跨节点消息去重键；AOF 提高重启后的可恢复性。
  redis:
    image: redis:7.4-alpine
    command: ["redis-server", "--appendonly", "yes"]
    healthcheck:
      test: ["CMD", "redis-cli", "ping"]
      interval: 3s
      timeout: 3s
      retries: 20

  # RabbitMQ 使用 direct exchange 在服务节点间转发在线消息。
  rabbitmq:
    image: rabbitmq:4.1-alpine
    environment:
      RABBITMQ_DEFAULT_USER: chat
      RABBITMQ_DEFAULT_PASS: chat-password
    healthcheck:
      test: ["CMD", "rabbitmq-diagnostics", "-q", "ping"]
      interval: 5s
      timeout: 5s
      retries: 30
```

这里声明基础服务 `redis`、`rabbitmq`及其健康检查。ChatServer 的 depends_on 等待的是命令实际成功，不只是容器进程存在，因此初始化表、Redis PING 和 RabbitMQ 启动完成后才接收聊天流量。

### 片段 3：第 49-71 行

```yaml

  # 节点 1 与节点 2 使用同一镜像和依赖，但 CHAT_SERVER_ID 必须唯一。
  chatserver-1:
    build: .
    depends_on:
      mysql: { condition: service_healthy }
      redis: { condition: service_healthy }
      rabbitmq: { condition: service_healthy }
    command: ["/app/build/bin/ChatServer", "0.0.0.0", "6000"]
    environment:
      <<: *chatserver-env
      CHAT_SERVER_ID: server-1

  chatserver-2:
    build: .
    environment:
      <<: *chatserver-env
      CHAT_SERVER_ID: server-2
    depends_on:
      mysql: { condition: service_healthy }
      redis: { condition: service_healthy }
      rabbitmq: { condition: service_healthy }
    command: ["/app/build/bin/ChatServer", "0.0.0.0", "6000"]
```

两个 ChatServer 使用同一镜像和端口，但以不同 CHAT_SERVER_ID 加入共享网络。这个 id 同时是 Redis 路由值和 RabbitMQ binding key；若重复，跨节点消息会无法确定唯一目标队列。

### 片段 4：第 72-92 行

```yaml

  # 每个 Web 网关只桥接一个 C++ 节点：浏览器 WebSocket <-> 后端长度帧 TCP。
  web:
    build: ./web
    environment:
      CHAT_WEB_PORT: 8080
      CHAT_BACKEND_HOST: chatserver-1
      CHAT_BACKEND_PORT: 6000
    depends_on: [chatserver-1]
    # 本机 8080 对应 server-1，供浏览器和同/跨节点测试使用。
    ports: ["8080:8080"]

  web-2:
    build: ./web
    environment:
      CHAT_WEB_PORT: 8080
      CHAT_BACKEND_HOST: chatserver-2
      CHAT_BACKEND_PORT: 6000
    depends_on: [chatserver-2]
    # 第二个入口用于验证 Redis 路由和 RabbitMQ 跨节点投递。
    ports: ["8081:8080"]
```

每个 Web 网关固定桥接一个 C++ 节点：8080 指向 server-1，8081 指向 server-2。浏览器通过两个入口登录不同用户后，消息才会真实经过 Redis 查路由和 RabbitMQ 跨节点转发。

## 面试重点

- 从空环境到服务可访问，构建、配置、健康检查和启动依赖的顺序是什么？

- 哪些值应通过环境变量注入，哪些文件或产物不应进入版本库？

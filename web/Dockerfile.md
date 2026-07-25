# `Dockerfile` 讲解

## 作用概览

**容器镜像构建说明。** 描述镜像中依赖安装、源码编译和启动命令。根目录版本构建 C++ 服务，`web/` 版本只封装 Node 网关；二者通过 Compose 组合。

阅读位置：`web/Dockerfile`。下文严格按源码顺序展示，每一行只出现一次；解释只针对紧邻的代码片段。

## 代码片段与详细讲解

### 片段 1：第 1-20 行

```dockerfile
# Web 网关镜像只承载 Node.js 反向代理和静态前端资源；C++ 聊天服务、
# MySQL、Redis、RabbitMQ 均由 compose.yaml 中的独立容器负责。
FROM node:20-bookworm-slim

# 固定工作目录，后续 COPY 和 CMD 都以 /app 为基准，便于在容器内排查文件。
WORKDIR /app

# 当前 Web 层没有第三方 npm 依赖，复制 package.json 主要用于保留启动脚本、
# 测试命令和项目元信息；gateway.mjs 是 WebSocket/HTTP 网关入口。
COPY package.json gateway.mjs ./

# public 目录包含浏览器直接加载的 HTML/CSS/JS，网关会把它作为静态目录暴露。
COPY public ./public

# 8080 是 Web 网关对外暴露的 HTTP/WebSocket 端口；compose 会把宿主机端口映射进来。
EXPOSE 8080

# 使用 exec 形式启动，保证 Node 进程可以直接收到 SIGTERM，方便 docker compose down
# 或滚动重启时优雅退出。
CMD ["node", "gateway.mjs"]
```

这些镜像层按“运行基础—依赖—源码/产物—启动入口”组织。前面稳定的依赖层可被缓存，后续源码变化只重建较小层；最终命令以前台进程运行，容器才能正确接收停止信号。

## 面试重点

- 从空环境到服务可访问，构建、配置、健康检查和启动依赖的顺序是什么？

- 哪些值应通过环境变量注入，哪些文件或产物不应进入版本库？

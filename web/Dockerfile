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

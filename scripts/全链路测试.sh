#!/usr/bin/env bash
# 快速全链路入口：任何构建、健康检查或业务断言失败都会立即停止。
set -euo pipefail

# 脚本位于 scripts/，向上一级得到项目根目录，保证 Compose 相对挂载路径正确。
project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$project_dir"

# 构建并启动 MySQL、Redis、RabbitMQ、两个 C++ 节点和两个 Web 网关。
# --wait 会等待 Compose healthcheck，降低服务尚未就绪导致的随机失败。
docker compose up -d --build --wait
# 从原生 WebSocket 测试客户端出发，验证注册、登录、跨节点消息、ACK、鉴权和离线恢复。
node web/test/full-stack.mjs

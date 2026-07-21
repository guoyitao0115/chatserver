#!/usr/bin/env bash
# 完整回归入口采用严格模式，不能在某个测试失败后继续输出“全部通过”。
set -euo pipefail

# 无论从哪个目录调用，都切换到 Compose 文件所在的项目根目录。
project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$project_dir"

# 压测规模允许调用者覆盖；默认 100 对连接、每对 50 条消息。
load_pairs="${CHAT_LOAD_PAIRS:-100}"
load_messages="${CHAT_LOAD_MESSAGES:-50}"

# 重建当前源码镜像并等待全部基础依赖健康。
docker compose up -d --build --wait
# 先运行速度快、定位清晰的 Node 网关集成测试。
npm test --prefix web
# C++ 核心测试在已包含相同构建产物的服务容器中执行，避免宿主机缺少原生依赖。
docker compose exec -T chatserver-1 ctest --test-dir /app/build --output-on-failure
# 小规模 smoke test 验证主链路，再运行覆盖去重、顺序和离线恢复的可靠性场景。
node web/test/full-stack.mjs
node web/test/reliability.mjs
# 只为本次命令注入压测参数；5 分钟总超时兼容较慢的本地 Docker 环境。
CHAT_LOAD_PAIRS="$load_pairs" \
CHAT_LOAD_MESSAGES="$load_messages" \
CHAT_LOAD_TIMEOUT_MS=300000 \
node web/test/load.mjs

# 只有上面所有命令都以 0 退出时才会执行到这里。
echo "完整回归与压力测试全部通过"

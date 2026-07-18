#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$project_dir"

load_pairs="${CHAT_LOAD_PAIRS:-100}"
load_messages="${CHAT_LOAD_MESSAGES:-50}"

docker compose up -d --build --wait
npm test --prefix web
docker compose exec -T chatserver-1 ctest --test-dir /app/build --output-on-failure
node web/test/full-stack.mjs
node web/test/reliability.mjs
CHAT_LOAD_PAIRS="$load_pairs" \
CHAT_LOAD_MESSAGES="$load_messages" \
CHAT_LOAD_TIMEOUT_MS=300000 \
node web/test/load.mjs

echo "完整回归与压力测试全部通过"

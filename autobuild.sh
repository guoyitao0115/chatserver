#!/usr/bin/env bash
# -e: 任一步失败即退出；-u: 未定义变量视为错误；pipefail: 管道中任一命令失败即失败。
set -euo pipefail

# 基于脚本自身位置定位项目，避免调用者当前目录影响相对路径。
project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# 使用源码/构建目录分离的 Debug 构建，便于本地断点和错误定位。
cmake -S "$project_dir" -B "$project_dir/build" -DCMAKE_BUILD_TYPE=Debug
# 让 CMake 按可用 CPU 并行调度，不把编译器参数写死在脚本中。
cmake --build "$project_dir/build" --parallel
# 执行已注册的无外部依赖核心测试；失败时展开测试程序输出。
ctest --test-dir "$project_dir/build" --output-on-failure

# `autobuild.sh` 讲解

## 作用概览

**本机构建脚本。** 顺序创建构建目录、运行 CMake 并并行编译，给学习者提供比手写多条命令更稳定的入口。

阅读位置：`autobuild.sh`。下文严格按源码顺序展示，每一行只出现一次；解释只针对紧邻的代码片段。

## 代码片段与详细讲解

### 片段 1：第 1-12 行

```bash
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
```

这段 DDL/DML 把运行前必须存在的数据库结构一次性建立起来。字段类型和索引围绕实际查询设计：用户 id 用于关系连接，离线消息按用户过滤并按自增 id 排序。

## 面试重点

- 从空环境到服务可访问，构建、配置、健康检查和启动依赖的顺序是什么？

- 哪些值应通过环境变量注入，哪些文件或产物不应进入版本库？

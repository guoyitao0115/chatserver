# `src/client/CMakeLists.txt` 讲解

## 作用概览

该文件把命令行客户端 `main.cpp` 构建为 `ChatClient`，配置项目头文件、第三方 JSON 头文件、线程库和警告选项。

## 按学习顺序讲解

1. `add_executable` 定义客户端目标。
2. 普通 include 路径用于公共协议与数据模型；第三方目录标记为 `SYSTEM`，避免外部头文件警告污染项目输出。
3. `Threads::Threads` 支持接收、心跳、重试和顺序刷新线程。
4. GNU/Clang 系编译器开启 `-Wall -Wextra -Wpedantic`。

## 面试重点

重要性较低。可能问：为什么第三方 include 用 `SYSTEM`？编译器降低第三方头警告等级，但项目自身仍严格检查。


# `src/server/CMakeLists.txt` 讲解

## 作用概览

该文件定义 `ChatServer` 的全部源码、外部依赖发现、头文件路径、链接库和编译警告，是服务端构建规则的中心。

## 按学习顺序讲解

1. `CHAT_SERVER_SOURCES`：显式列出网络层、业务层、数据模型、RabbitMQ、Redis 和密码模块，新增实现文件时必须加入此处。
2. `find_path/find_library(... REQUIRED)`：分别定位 Muduo、MySQL、hiredis、rabbitmq-c 和 crypt；缺依赖时配置阶段立即失败。
3. `add_executable`：生成服务端目标。
4. 项目 include 与外部 include 分离，第三方 JSON 标记 `SYSTEM`。
5. `target_link_libraries`：链接网络库、基础库、三种基础设施客户端、密码库和线程库。
6. 编译器警告与客户端保持一致。

## 面试重点

重要性中等。可能问题：为什么显式列源码而不是 glob？新增文件需要手工维护，但构建输入变化更清晰、CMake 不会因文件系统变化而意外漏重新配置；库链接顺序在静态链接中为什么重要？符号解析通常从左到右。


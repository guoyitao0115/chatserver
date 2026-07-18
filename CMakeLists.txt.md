# `CMakeLists.txt` 讲解

## 作用概览

这是项目顶层 CMake 入口，统一声明项目版本、C++ 标准、可选构建目标和测试入口，再把具体目标下放到 `src` 与 `test` 子目录。

## 按学习顺序讲解

1. `cmake_minimum_required` 与 `project`：要求 CMake 3.16，并定义 C++ 项目 `ChatServer 2.0.0`。
2. `CHAT_BUILD_SERVER/CLIENT/TESTS`：让容器、开发机或 CI 可以只构建需要的部分。
3. C++17 配置：强制标准且禁用编译器扩展，提高跨平台一致性。
4. `CMAKE_RUNTIME_OUTPUT_DIRECTORY`：把可执行文件集中放到构建目录的 `bin`。
5. `find_package(Threads REQUIRED)`：获得可移植线程目标 `Threads::Threads`。
6. `add_subdirectory(src)`：进入业务目标；测试开关开启时再调用 `enable_testing()` 并加载 `test`。

本文件没有业务函数，学习时应把它看作“构建依赖图的根节点”。

## 面试重点

重要性中等。可能问题：为什么使用 target 级 include/link 配置而不是全局配置？前者能限制依赖传播；如何只构建服务端？使用 `-DCHAT_BUILD_CLIENT=OFF` 等选项。


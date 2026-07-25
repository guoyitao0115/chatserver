# `CMakeLists.txt` 讲解

## 作用概览

**核心单元测试构建规则。** 生成不依赖 MySQL/RabbitMQ 的核心测试程序并注册到 CTest，用较低成本持续验证帧协议、配置读取和内存去重。

阅读位置：`test/CMakeLists.txt`。下文严格按源码顺序展示，每一行只出现一次；解释只针对紧邻的代码片段。

## 代码片段与详细讲解

### 片段 1：第 1-15 行

```cmake
# 核心测试刻意不链接 MySQL/Redis/RabbitMQ，保证开发机和容器中都能快速运行。
add_executable(chat_core_tests core_tests.cpp)
# 测试直接复用生产协议、配置和本地去重实现。
target_include_directories(chat_core_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/include/server"
)
# 并发去重用例会创建多个 std::thread。
target_link_libraries(chat_core_tests PRIVATE Threads::Threads)
# 测试代码同样接受严格警告，避免测试本身存在未定义行为或可疑写法。
target_compile_options(chat_core_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic>
)
# 注册到 CTest 后，可通过 ctest --test-dir build 统一执行。
add_test(NAME chat_core_tests COMMAND chat_core_tests)
```

这一构建片段把依赖发现、源文件范围和目标属性固定下来。配置阶段缺少依赖会在这里提前失败，而不是等链接或运行时才暴露；目标级 include/link 设置也避免污染无关程序。

## 面试重点

- 测试准备了什么外部状态或模拟组件，实际动作经过哪些模块？

- 每个断言证明的是返回值正确，还是“不丢、不重、不乱序、不可冒用”等系统性质？

- 如何避免测试自身的等待竞态和上轮残留状态造成假失败？

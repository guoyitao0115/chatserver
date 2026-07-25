# `CMakeLists.txt` 讲解

## 作用概览

**命令行客户端构建规则。** 把单文件客户端与线程库、JSON 头文件及公共模型组合成可执行程序，使客户端使用与服务端一致的协议常量和数据对象。

阅读位置：`src/client/CMakeLists.txt`。下文严格按源码顺序展示，每一行只出现一次；解释只针对紧邻的代码片段。

## 代码片段与详细讲解

### 片段 1：第 1-18 行

```cmake
# 客户端当前集中在一个入口文件中，可靠性、排序和命令处理均由该目标编译。
add_executable(ChatClient main.cpp)

# 公共协议在 include/；model 目录中的数据对象用于解析登录响应。
target_include_directories(ChatClient PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/include/server/model"
)
# nlohmann/json 是随仓库提供的第三方头，标记 SYSTEM 可抑制依赖内部警告。
target_include_directories(ChatClient SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/thirdparty")

# std::thread 在部分平台需要显式链接 pthread，由可移植的 Threads target 处理。
target_link_libraries(ChatClient PRIVATE Threads::Threads)

# 只对支持这些参数的编译器开启警告，避免 MSVC 等环境误用 GNU 选项。
target_compile_options(ChatClient PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic>
)
```

这一构建片段把依赖发现、源文件范围和目标属性固定下来。配置阶段缺少依赖会在这里提前失败，而不是等链接或运行时才暴露；目标级 include/link 设置也避免污染无关程序。

## 面试重点

- 从空环境到服务可访问，构建、配置、健康检查和启动依赖的顺序是什么？

- 哪些值应通过环境变量注入，哪些文件或产物不应进入版本库？

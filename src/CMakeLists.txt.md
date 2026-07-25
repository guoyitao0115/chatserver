# `CMakeLists.txt` 讲解

## 作用概览

**源码构建入口。** 把 `server` 与 `client` 两个可执行程序交给各自的 CMake 文件，顶层只负责组织，不在这里重复维护源文件列表。

阅读位置：`src/CMakeLists.txt`。下文严格按源码顺序展示，每一行只出现一次；解释只针对紧邻的代码片段。

## 代码片段与详细讲解

### 片段 1：第 1-9 行

```cmake
# 服务端依赖 Muduo、MySQL、Redis、RabbitMQ 和 crypt；在轻量开发环境可单独关闭。
if(CHAT_BUILD_SERVER)
    add_subdirectory(server)
endif()

# 命令行客户端只依赖线程和公共协议头，可独立用于协议调试。
if(CHAT_BUILD_CLIENT)
    add_subdirectory(client)
endif()
```

这一构建片段把依赖发现、源文件范围和目标属性固定下来。配置阶段缺少依赖会在这里提前失败，而不是等链接或运行时才暴露；目标级 include/link 设置也避免污染无关程序。

## 面试重点

- 从空环境到服务可访问，构建、配置、健康检查和启动依赖的顺序是什么？

- 哪些值应通过环境变量注入，哪些文件或产物不应进入版本库？

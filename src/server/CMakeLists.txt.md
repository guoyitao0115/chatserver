# `CMakeLists.txt` 讲解

## 作用概览

**服务端构建规则。** 收集业务、模型、Redis、RabbitMQ 与密码模块，解析第三方库并链接成 `ChatServer`。它也是判断部署环境是否具备 Muduo、MySQL、hiredis、rabbitmq-c 和 crypt 的第一道检查。

阅读位置：`src/server/CMakeLists.txt`。下文严格按源码顺序展示，每一行只出现一次；解释只针对紧邻的代码片段。

## 代码片段与详细讲解

### 片段 1：第 1-27 行

```cmake
# 显式列出源文件能让新增模块必须经过构建审查，也避免 glob 因缓存导致漏编译。
set(CHAT_SERVER_SOURCES
    main.cpp
    chatserver.cpp
    chatservice.cpp
    db/db.cpp
    model/friendmodel.cpp
    model/groupmodel.cpp
    model/offlinemessagemodel.cpp
    model/usermodel.cpp
    mq/rabbitmq_bus.cpp
    redis/redis.cpp
    security/password_hasher.cpp
)

# 以下依赖属于服务端完整链路：网络、持久化、分布式路由、消息总线和密码哈希。
# 使用 REQUIRED 让缺少开发包时在配置阶段立即给出明确错误，而不是链接时失败。
find_path(MUDUO_INCLUDE_DIR muduo/net/TcpServer.h REQUIRED)
find_library(MUDUO_NET_LIBRARY NAMES muduo_net REQUIRED)
find_library(MUDUO_BASE_LIBRARY NAMES muduo_base REQUIRED)
find_path(MYSQL_INCLUDE_DIR mysql/mysql.h REQUIRED)
find_library(MYSQL_LIBRARY NAMES mysqlclient mariadb REQUIRED)
find_path(HIREDIS_INCLUDE_DIR hiredis/hiredis.h REQUIRED)
find_library(HIREDIS_LIBRARY NAMES hiredis REQUIRED)
find_path(RABBITMQ_INCLUDE_DIR amqp.h REQUIRED)
find_library(RABBITMQ_LIBRARY NAMES rabbitmq REQUIRED)
find_library(CRYPT_LIBRARY NAMES crypt xcrypt REQUIRED)
```

这一构建片段把依赖发现、源文件范围和目标属性固定下来。配置阶段缺少依赖会在这里提前失败，而不是等链接或运行时才暴露；目标级 include/link 设置也避免污染无关程序。

### 片段 2：第 28-58 行

```cmake

# 主服务进程；运行参数由 main.cpp 解析，默认监听 6000。
add_executable(ChatServer ${CHAT_SERVER_SOURCES})

# 项目头和外部库头仅对 ChatServer 可见，不污染其他 target。
target_include_directories(ChatServer PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/include/server"
    "${PROJECT_SOURCE_DIR}/include/server/db"
    "${PROJECT_SOURCE_DIR}/include/server/model"
    "${PROJECT_SOURCE_DIR}/include/server/redis"
    "${PROJECT_SOURCE_DIR}/include/server/mq"
    "${PROJECT_SOURCE_DIR}/include/server/security"
    "${MUDUO_INCLUDE_DIR}"
    "${MYSQL_INCLUDE_DIR}"
    "${HIREDIS_INCLUDE_DIR}"
    "${RABBITMQ_INCLUDE_DIR}"
)
# 仓库内第三方 JSON 头作为 SYSTEM 依赖，避免其警告影响项目自身的告警质量。
target_include_directories(ChatServer SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/thirdparty")

# Muduo net 依赖 base；其余库分别对应数据库、Redis、RabbitMQ、bcrypt/crypt 和线程。
target_link_libraries(ChatServer PRIVATE
    "${MUDUO_NET_LIBRARY}"
    "${MUDUO_BASE_LIBRARY}"
    "${MYSQL_LIBRARY}"
    "${HIREDIS_LIBRARY}"
    "${RABBITMQ_LIBRARY}"
    "${CRYPT_LIBRARY}"
    Threads::Threads
)
```

这部分执行 `add_executable`、`target_include_directories`、`target_link_libraries`，作用目标是 `ChatServer`。源文件只进入对应可执行目标，include、编译选项和链接库也以目标级作用域传播，因此客户端或测试不会无意继承服务端全部依赖。

### 片段 3：第 59-63 行

```cmake

# 保持常见编译器的基础警告开启；暂未使用 -Werror，避免平台差异阻断构建。
target_compile_options(ChatServer PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic>
)
```

这部分执行 `target_compile_options`，作用目标是 `ChatServer`。源文件只进入对应可执行目标，include、编译选项和链接库也以目标级作用域传播，因此客户端或测试不会无意继承服务端全部依赖。

## 面试重点

- 从空环境到服务可访问，构建、配置、健康检查和启动依赖的顺序是什么？

- 哪些值应通过环境变量注入，哪些文件或产物不应进入版本库？

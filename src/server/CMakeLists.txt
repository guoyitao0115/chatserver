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

# 保持常见编译器的基础警告开启；暂未使用 -Werror，避免平台差异阻断构建。
target_compile_options(ChatServer PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic>
)

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

add_executable(ChatServer ${CHAT_SERVER_SOURCES})

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
target_include_directories(ChatServer SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/thirdparty")

target_link_libraries(ChatServer PRIVATE
    "${MUDUO_NET_LIBRARY}"
    "${MUDUO_BASE_LIBRARY}"
    "${MYSQL_LIBRARY}"
    "${HIREDIS_LIBRARY}"
    "${RABBITMQ_LIBRARY}"
    "${CRYPT_LIBRARY}"
    Threads::Threads
)

target_compile_options(ChatServer PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic>
)

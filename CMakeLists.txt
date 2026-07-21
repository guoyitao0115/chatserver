# 当前工程使用较新的 target_* 风格命令；3.16 同时兼容常见的 Ubuntu/Debian 构建环境。
cmake_minimum_required(VERSION 3.16)

# 只启用 C++，避免 CMake 为未使用的 C 编译器执行额外探测。
project(ChatServer VERSION 2.0.0 LANGUAGES CXX)

# 三个开关用于按环境裁剪目标：例如只构建无外部依赖的客户端和核心测试。
option(CHAT_BUILD_SERVER "Build the Muduo chat server" ON)
option(CHAT_BUILD_CLIENT "Build the command-line client" ON)
option(CHAT_BUILD_TESTS "Build unit tests" ON)

# 项目使用结构化绑定、optional 等 C++17 能力，并禁止退回编译器私有方言。
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# 把所有可执行文件放到同一目录，Docker、CTest 和本地脚本可使用稳定路径。
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# 客户端重试/接收线程和服务端依赖都需要系统线程库。
find_package(Threads REQUIRED)

# 子目录内部再根据上面的开关决定是否创建具体 target。
add_subdirectory(src)

if(CHAT_BUILD_TESTS)
    # enable_testing 必须在 add_test 生效前调用。
    enable_testing()
    add_subdirectory(test)
endif()

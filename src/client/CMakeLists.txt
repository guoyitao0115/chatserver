# 客户端当前集中在一个入口文件中，可靠性、排序和命令处理均由该目标编译。
add_executable(ChatClient main.cpp)

# 公共协议在 include/；model 目录中的数据对象用于解析登录响应。
target_include_directories(ChatClient PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/include/server"
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

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

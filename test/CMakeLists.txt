add_executable(chat_core_tests core_tests.cpp)
target_include_directories(chat_core_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/include/server"
)
target_link_libraries(chat_core_tests PRIVATE Threads::Threads)
target_compile_options(chat_core_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic>
)
add_test(NAME chat_core_tests COMMAND chat_core_tests)

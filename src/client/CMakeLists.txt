add_executable(ChatClient main.cpp)

target_include_directories(ChatClient PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/include/server/model"
)
target_include_directories(ChatClient SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/thirdparty")

target_link_libraries(ChatClient PRIVATE Threads::Threads)

target_compile_options(ChatClient PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic>
)

cmake_minimum_required(VERSION 3.16)

project(ChatServer VERSION 2.0.0 LANGUAGES CXX)

option(CHAT_BUILD_SERVER "Build the Muduo chat server" ON)
option(CHAT_BUILD_CLIENT "Build the command-line client" ON)
option(CHAT_BUILD_TESTS "Build unit tests" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

find_package(Threads REQUIRED)

add_subdirectory(src)

if(CHAT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()

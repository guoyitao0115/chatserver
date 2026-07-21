# 这是独立的 Muduo echo 学习样例，不加入根工程的默认 CTest 链路。
# 若单独在本目录配置构建，需要系统已经安装 muduo_net、muduo_base 和 pthread。

# 旧式全局 include_directories/link_directories 会影响整个目录；本样例没有额外头路径。
# include_directories()
# link_directories()

# 样例仅包含一个源文件，显式列出便于学习目标与源码的对应关系。
set(SRC_LIST ./muduo_server.cpp)

# 保留原学习工程的输出约定：二进制写入项目 bin，而非主工程 build/bin。
set(EXECUTABLE_OUTPUT_PATH ${PROJECT_SOURCE_DIR}/bin)

# 文件增多时可以使用 aux_source_directory；显式列表通常更可控。
# aux_source_directory(. SRC_LIST)

# 由 SRC_LIST 创建最小 echo server。
add_executable(server ${SRC_LIST})
# 链接顺序体现 muduo_net 对 muduo_base 和线程库的依赖。
target_link_libraries(server muduo_net muduo_base pthread)

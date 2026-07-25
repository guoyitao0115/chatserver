# `CMakeLists.txt` 讲解

## 作用概览

**Muduo 学习样例构建规则。** 单独构建一个最小回显服务器，用于理解 Muduo 回调模型，不参与正式 ChatServer 业务。

阅读位置：`test/testmuduo/CMakeLists.txt`。下文严格按源码顺序展示，每一行只出现一次；解释只针对紧邻的代码片段。

## 代码片段与详细讲解

### 片段 1：第 1-20 行

```cmake
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
```

这一构建片段把依赖发现、源文件范围和目标属性固定下来。配置阶段缺少依赖会在这里提前失败，而不是等链接或运行时才暴露；目标级 include/link 设置也避免污染无关程序。

## 面试重点

- 测试准备了什么外部状态或模拟组件，实际动作经过哪些模块？

- 每个断言证明的是返回值正确，还是“不丢、不重、不乱序、不可冒用”等系统性质？

- 如何避免测试自身的等待竞态和上轮残留状态造成假失败？

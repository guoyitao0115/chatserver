# `testmuduo/CMakeLists.txt` 讲解

## 作用概览

这是早期 Muduo echo 示例的独立构建配置，不在顶层当前测试树中，主要保留学习痕迹。

## 按学习顺序讲解

1. `SRC_LIST` 指向单个示例源码。
2. `EXECUTABLE_OUTPUT_PATH` 把旧示例输出到项目 `bin`。
3. `add_executable(server ...)` 生成 echo 服务。
4. 链接 `muduo_net`、`muduo_base` 和 `pthread`。
5. 被注释的 include/link/aux_source 配置展示了早期 CMake 写法，但当前正式目标使用更现代的 target 级命令。

## 面试重点

重要性较低。若简历仓库被追问，应明确它是学习样例而非生产主服务，正式构建位于 `src/server/CMakeLists.txt`。


# `Dockerfile` 讲解

## 作用概览

该镜像从 Debian 构建 Muduo 和项目本身，并在镜像构建阶段执行 CTest，最终默认启动 6000 端口的聊天服务。

## 按学习顺序讲解

1. 基础镜像与参数：`bookworm-slim` 控制体积，`MUDUO_REF` 允许指定 Muduo 版本。
2. 系统依赖：安装编译工具及 Boost、MySQL、hiredis、RabbitMQ、crypt 等开发库，并清理 apt 缓存。
3. Muduo 构建：浅克隆源码，Release 编译、安装并刷新动态库缓存。
4. 项目复制：只复制构建所需的 CMake、源码、头文件、测试和第三方头文件。
5. 项目验证：Release 构建并执行 `ctest --output-on-failure`；测试失败会使镜像构建失败。
6. 运行入口：声明 6000 端口，以 `0.0.0.0:6000` 启动 `ChatServer`。

## 面试重点

重要性中等。可能问题：当前为何不是多阶段构建？实现直观但最终镜像包含编译工具，生产可改为 builder/runtime 两阶段；为什么构建镜像时跑测试？尽早阻止不可用镜像进入部署流程。


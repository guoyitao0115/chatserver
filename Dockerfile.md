# `Dockerfile` 讲解

## 作用概览

**容器镜像构建说明。** 描述镜像中依赖安装、源码编译和启动命令。根目录版本构建 C++ 服务，`web/` 版本只封装 Node 网关；二者通过 Compose 组合。

阅读位置：`Dockerfile`。下文严格按源码顺序展示，每一行只出现一次；解释只针对紧邻的代码片段。

## 代码片段与详细讲解

### 片段 1：第 1-24 行

```dockerfile
# 使用精简 Debian 作为统一构建与运行环境；当前是单阶段镜像，便于面试项目复现。
FROM debian:bookworm-slim

# 禁止 apt 在无人值守构建时弹出交互界面。
ARG DEBIAN_FRONTEND=noninteractive
# Muduo 默认跟随 master；需要可重复构建时应传入固定 tag 或 commit。
ARG MUDUO_REF=master

# 安装编译工具以及 ChatServer 的全部原生依赖。--no-install-recommends 控制镜像体积。
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
       build-essential cmake git ca-certificates \
       libboost-dev zlib1g-dev \
       default-libmysqlclient-dev libhiredis-dev librabbitmq-dev libcrypt-dev \
    && rm -rf /var/lib/apt/lists/*

# Muduo 没有直接使用发行版包，因此从源码编译并安装到系统默认前缀。
WORKDIR /tmp
RUN git clone --depth 1 --branch "${MUDUO_REF}" https://github.com/chenshuo/muduo.git \
    && cmake -S muduo -B muduo/build -DCMAKE_BUILD_TYPE=Release -DBUILD_EXAMPLES=OFF \
    && cmake --build muduo/build --parallel 2 \
    && cmake --install muduo/build \
    && rm -rf muduo \
    && ldconfig
```

这些镜像层按“运行基础—依赖—源码/产物—启动入口”组织。前面稳定的依赖层可被缓存，后续源码变化只重建较小层；最终命令以前台进程运行，容器才能正确接收停止信号。

### 片段 2：第 25-42 行

```dockerfile

# 只复制构建所需文件；运行时配置通过环境变量注入，不写死在镜像中。
WORKDIR /app
COPY CMakeLists.txt ./
COPY include ./include
COPY src ./src
COPY test ./test
COPY thirdparty ./thirdparty

# Release 构建完成后立即运行 CTest，使协议/去重测试失败时镜像构建直接终止。
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCHAT_BUILD_TESTS=ON \
    && cmake --build build --parallel 2 \
    && ctest --test-dir build --output-on-failure

# EXPOSE 只描述容器监听端口，实际发布由 Compose 的 ports 或容器网络决定。
EXPOSE 6000
# 默认监听所有容器网卡；Compose 可覆盖 command 启动多个独立节点。
CMD ["/app/build/bin/ChatServer", "0.0.0.0", "6000"]
```

镜像在这里执行 `WORKDIR`、`COPY`、`RUN`、`EXPOSE`、`CMD`。依赖安装与源码复制分层以利用缓存，构建产物留在约定目录；容器最终以前台服务进程作为 CMD，Compose 才能正确观察退出并转发终止信号。

## 面试重点

- 从空环境到服务可访问，构建、配置、健康检查和启动依赖的顺序是什么？

- 哪些值应通过环境变量注入，哪些文件或产物不应进入版本库？

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

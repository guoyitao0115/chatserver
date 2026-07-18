FROM debian:bookworm-slim

ARG DEBIAN_FRONTEND=noninteractive
ARG MUDUO_REF=master

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
       build-essential cmake git ca-certificates \
       libboost-dev zlib1g-dev \
       default-libmysqlclient-dev libhiredis-dev librabbitmq-dev libcrypt-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /tmp
RUN git clone --depth 1 --branch "${MUDUO_REF}" https://github.com/chenshuo/muduo.git \
    && cmake -S muduo -B muduo/build -DCMAKE_BUILD_TYPE=Release -DBUILD_EXAMPLES=OFF \
    && cmake --build muduo/build --parallel 2 \
    && cmake --install muduo/build \
    && rm -rf muduo \
    && ldconfig

WORKDIR /app
COPY CMakeLists.txt ./
COPY include ./include
COPY src ./src
COPY test ./test
COPY thirdparty ./thirdparty

RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCHAT_BUILD_TESTS=ON \
    && cmake --build build --parallel 2 \
    && ctest --test-dir build --output-on-failure

EXPOSE 6000
CMD ["/app/build/bin/ChatServer", "0.0.0.0", "6000"]

# 服务端依赖 Muduo、MySQL、Redis、RabbitMQ 和 crypt；在轻量开发环境可单独关闭。
if(CHAT_BUILD_SERVER)
    add_subdirectory(server)
endif()

# 命令行客户端只依赖线程和公共协议头，可独立用于协议调试。
if(CHAT_BUILD_CLIENT)
    add_subdirectory(client)
endif()

# `package.json` 讲解

## 作用概览

**Web 网关工程清单。** 声明 ESM 运行方式和测试脚本。项目只使用 Node 内置模块，因此没有生产依赖，部署时无需安装大型框架。

阅读位置：`web/package.json`。下文严格按源码顺序展示，每一行只出现一次；解释只针对紧邻的代码片段。

## 代码片段与详细讲解

### 片段 1：第 1-20 行

```json
{
  "name": "chatserver-web",
  "version": "1.0.0",
  "private": true,
  "type": "module",
  "description": "Minimal WebSocket-to-TCP adapter and browser client for ChatServer",
  "scripts": {
    "dev": "node --watch gateway.mjs",
    "start": "node gateway.mjs",
    "test": "node --test test/*.test.mjs",
    "test:e2e": "node test/full-stack.mjs",
    "test:reliability": "node test/reliability.mjs",
    "test:load": "node test/load.mjs",
    "test:chaos:rabbitmq": "node test/chaos-rabbitmq.mjs",
    "test:chaos:mysql": "node test/chaos-mysql.mjs"
  },
  "engines": {
    "node": ">=20"
  }
}
```

这些配置项围绕 `name`、`chatserver-web`、`version`、`private`、`true`、`type`、`module`、`description` 建立当前运行条件。相邻组件使用同名值连接起来，修改时需要同时检查生产默认值、容器覆盖值和测试入口是否仍一致。

## 面试重点

- 从空环境到服务可访问，构建、配置、健康检查和启动依赖的顺序是什么？

- 哪些值应通过环境变量注入，哪些文件或产物不应进入版本库？

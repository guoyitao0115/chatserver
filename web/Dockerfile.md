# `web/Dockerfile` 讲解

## 作用概览

该镜像用 Node.js 20 运行零第三方运行依赖的 WebSocket/TCP 网关，并携带静态前端资源。

## 按学习顺序讲解

1. `node:20-bookworm-slim` 与 `package.json` 的 engines 保持一致。
2. 工作目录为 `/app`。
3. 复制包元数据、网关和 `public`；没有 `npm install`，因为运行时只使用 Node 内置模块。
4. 暴露 8080，执行 `node gateway.mjs`。

## 面试重点

重要性较低到中等。可能问题：为何前端容器不装依赖？网关和页面均为原生 API；好处是镜像简单，代价是很多协议细节需自己维护。


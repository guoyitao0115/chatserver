# `package.json` 讲解

## 作用概览

该文件声明 Web 子项目元数据、ES Module 模式、Node 版本和测试/运行命令。项目无 npm 依赖。

## 按学习顺序讲解

- `private: true`：防止误发布到 npm。
- `type: module`：让 `.mjs`/JS 使用标准 ESM import/export。
- `dev/start`：分别以 watch 模式和普通模式启动网关。
- `test`：运行 Node 内置测试框架下的网关测试。
- `test:e2e/reliability/load`：从主链路逐步加深到可靠性和压力。
- 两个 `test:chaos:*`：配合外部脚本暂停基础设施。
- `engines.node >=20`：约束 Buffer、test runner 等运行环境。

## 面试重点

重要性中等。可能问题：为什么不需要 webpack/Vite？当前页面是原生 ESM 和静态文件，保持对后端架构最小侵入；规模扩大后可独立引入构建工具。


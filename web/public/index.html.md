# `index.html` 讲解

## 作用概览

**聊天页面结构。** 定义登录注册区、会话列表、消息区和弹窗表单。页面只提供语义节点，连接与消息状态由 `app.js` 驱动，视觉布局由 `styles.css` 控制。

阅读位置：`web/public/index.html`。下文严格按源码顺序展示，每一行只出现一次；解释只针对紧邻的代码片段。

## 代码片段与详细讲解

### 片段 1：第 1-27 行

```html
<!doctype html>
<html lang="zh-CN">
  <head>
    <!-- UTF-8 保证中文昵称与消息提示正确显示；viewport 让移动端按设备宽度布局。 -->
    <meta charset="UTF-8" />
    <meta name="viewport" content="width=device-width, initial-scale=1" />
    <meta name="description" content="基于 Muduo、MySQL、Redis 与 RabbitMQ 的轻量聊天系统" />
    <title>微光 · ChatServer</title>
    <link rel="stylesheet" href="/styles.css" />
  </head>
  <body>
    <!-- 纯装饰背景不承载内容，CSS 已设置 pointer-events:none，不会阻挡表单操作。 -->
    <div class="ambient ambient-one"></div>
    <div class="ambient ambient-two"></div>

    <main class="shell">
      <!-- 未登录视图。登录和注册共用右侧卡片，由 app.js 切换 hidden 类。 -->
      <section id="auth-view" class="auth-view">
        <div class="brand-panel">
          <div class="brand-mark" aria-hidden="true">微</div>
          <p class="eyebrow">REAL-TIME CHAT</p>
          <h1>让每一次连接，<br />都有清晰的回响。</h1>
          <p class="brand-copy">一个轻量、可靠、可扩展的即时通信练习项目。浏览器通过适配网关复用原有 C++ 长连接协议。</p>
          <div class="feature-row" aria-label="项目特性">
            <span>消息确认</span><span>断线感知</span><span>离线消息</span>
          </div>
        </div>
```

这些语义节点为脚本提供稳定的 id/class 挂载点。表单收集身份或关系数据，列表容器由 JavaScript 重绘，消息区域独立滚动；结构本身不保存登录状态。

### 片段 2：第 28-56 行

```html

        <div class="auth-card">
          <!-- role/aria-selected 为读屏器表达页签状态；视觉状态由同名 active 类控制。 -->
          <div class="tabs" role="tablist" aria-label="账户操作">
            <button class="tab active" id="login-tab" type="button" role="tab" aria-selected="true">登录</button>
            <button class="tab" id="register-tab" type="button" role="tab" aria-selected="false">创建账户</button>
          </div>

          <!-- autocomplete 使用标准令牌，允许密码管理器安全识别账号和当前密码。 -->
          <form id="login-form" class="auth-form">
            <div>
              <p class="form-kicker">欢迎回来</p>
              <h2>登录聊天空间</h2>
            </div>
            <label>用户 ID<input id="login-id" inputmode="numeric" autocomplete="username" required placeholder="例如 12" /></label>
            <label>密码<input id="login-password" type="password" autocomplete="current-password" required placeholder="输入密码" /></label>
            <button class="primary" type="submit">进入聊天</button>
          </form>

          <!-- 密码长度与 bcrypt 的有效输入边界一致；后端仍会再次校验，不能只信任 HTML 约束。 -->
          <form id="register-form" class="auth-form hidden">
            <div>
              <p class="form-kicker">第一次见面</p>
              <h2>创建一个新账户</h2>
            </div>
            <label>昵称<input id="register-name" maxlength="50" autocomplete="nickname" required placeholder="你的昵称" /></label>
            <label>密码<input id="register-password" type="password" minlength="6" maxlength="72" autocomplete="new-password" required placeholder="6–72 位" /></label>
            <button class="primary" type="submit">创建账户</button>
          </form>
```

本段建立 `div`、`button`、`form`、`p`、`h2`、`label` 结构，脚本挂载点是 `login-tab`、`register-tab`、`login-form`、`login-id`、`login-password`、`register-form`、`register-name`、`register-password`。用户输入由表单事件读取，动态列表和提示内容由 app.js 写入这些容器，HTML 本身不判断登录或消息状态。

### 片段 3：第 57-83 行

```html

          <!-- role=status 让辅助技术朗读异步登录结果与连接状态。 -->
          <p id="auth-message" class="form-message" role="status"></p>
          <div class="connection-line"><span id="connection-dot" class="status-dot"></span><span id="connection-text">正在连接网关…</span></div>
        </div>
      </section>

      <!-- 登录成功后的主界面：左侧会话导航，右侧当前会话与发送区。 -->
      <section id="chat-view" class="chat-view hidden">
        <aside class="sidebar">
          <header class="sidebar-header">
            <div class="brand-small"><span class="brand-mark small">微</span><strong>微光</strong></div>
            <button id="logout-button" class="quiet-button" type="button">退出</button>
          </header>
          <div class="profile-card">
            <div id="profile-avatar" class="avatar">U</div>
            <div><strong id="profile-name">用户</strong><small id="profile-id">ID —</small></div>
            <span class="online-pill">在线</span>
          </div>
          <nav class="list-tabs" aria-label="会话类型">
            <button id="friends-tab" class="list-tab active" type="button">好友</button>
            <button id="groups-tab" class="list-tab" type="button">群组</button>
          </nav>
          <!-- 会话条目由 app.js 使用 createElement/textContent 创建，避免用户昵称被当成 HTML。 -->
          <div id="conversation-list" class="conversation-list"></div>
          <button id="new-conversation" class="secondary full" type="button">＋ 添加会话</button>
        </aside>
```

本段建立 `p`、`div`、`span`、`section`、`aside`、`header` 结构，脚本挂载点是 `auth-message`、`connection-dot`、`connection-text`、`chat-view`、`logout-button`、`profile-avatar`、`profile-name`、`profile-id`、`friends-tab`、`groups-tab`、`conversation-list`、`new-conversation`。用户输入由表单事件读取，动态列表和提示内容由 app.js 写入这些容器，HTML 本身不判断登录或消息状态。

### 片段 4：第 84-112 行

```html

        <section class="conversation">
          <header class="conversation-header">
            <div><p class="eyebrow">CURRENT CONVERSATION</p><h2 id="conversation-title">选择一个会话</h2></div>
            <span id="conversation-meta" class="conversation-meta">—</span>
          </header>
          <!-- polite 模式不会打断读屏器当前朗读；新消息在合适时机被播报。 -->
          <div id="message-list" class="message-list" aria-live="polite">
            <div class="empty-state"><span>聊</span><h3>从左侧选择好友或群组</h3><p>消息会通过 WebSocket 网关转发至原有 C++ 服务。</p></div>
          </div>
          <!-- 初始禁用，只有选中好友或群组后 app.js 才允许提交。maxlength 控制演示前端输入规模，后端仍负责最终校验。 -->
          <form id="composer" class="composer">
            <input id="message-input" autocomplete="off" maxlength="2000" placeholder="输入消息，按 Enter 发送" disabled />
            <button class="send-button" type="submit" disabled aria-label="发送消息">发送</button>
          </form>
        </section>
      </section>
    </main>

    <!-- 原生 dialog 复用于添加好友、创建群组和加入群组，字段显隐由当前左栏模式决定。 -->
    <dialog id="action-dialog">
      <form id="action-form" method="dialog">
        <div class="dialog-heading"><p class="form-kicker">新会话</p><h2 id="dialog-title">添加好友</h2></div>
        <label id="dialog-id-label">好友 ID<input id="dialog-id" inputmode="numeric" required /></label>
        <label id="dialog-name-label" class="hidden">群名称<input id="dialog-name" maxlength="50" /></label>
        <label id="dialog-desc-label" class="hidden">群描述<input id="dialog-desc" maxlength="200" /></label>
        <div class="dialog-actions"><button id="dialog-cancel" class="quiet-button" type="button">取消</button><button class="primary" type="submit">确认</button></div>
      </form>
    </dialog>
```

本段建立 `section`、`header`、`div`、`p`、`h2`、`span` 结构，脚本挂载点是 `conversation-title`、`conversation-meta`、`message-list`、`composer`、`message-input`、`action-dialog`、`action-form`、`dialog-title`、`dialog-id-label`、`dialog-id`、`dialog-name-label`、`dialog-name`、`dialog-desc-label`、`dialog-desc`、`dialog-cancel`。用户输入由表单事件读取，动态列表和提示内容由 app.js 写入这些容器，HTML 本身不判断登录或消息状态。

### 片段 5：第 113-119 行

```html

    <!-- 所有动态文本均由 textContent 写入；页面不为聊天正文开放 HTML 渲染能力。 -->
    <div id="toast" class="toast" role="status"></div>
    <!-- ES module 默认 defer：DOM 解析完成后执行，并可直接使用浏览器 crypto.randomUUID。 -->
    <script type="module" src="/app.js"></script>
  </body>
</html>
```

本段建立 `div`、`script` 结构，脚本挂载点是 `toast`。用户输入由表单事件读取，动态列表和提示内容由 app.js 写入这些容器，HTML 本身不判断登录或消息状态。

## 面试重点

- 这个文件处于哪一层，它保存的数据由谁创建、由谁消费？

- 如果删除或修改本文件，最先受影响的运行链路是什么？

# `app.js` 讲解

## 作用概览

**浏览器聊天客户端。** 维护 WebSocket、登录用户、会话消息、待 ACK 重试和每会话序号。服务端消息先归入会话，再由当前会话渲染；断线重连后仍保留页面内状态。

阅读位置：`web/public/app.js`。下文严格按源码顺序展示，每一行只出现一次；解释只针对紧邻的代码片段。

## 代码片段与详细讲解

### 片段 1：第 1-30 行

```javascript
import { MessageIdDeduplicator } from './message-dedup.js';

// 轻量浏览器客户端：不引入框架，用一个集中 state 管理连接、会话、消息与重试状态。
// 浏览器只通过同源 /ws 与网关通信；所有身份鉴权、权限判断、持久化和分布式去重仍由后端负责。

// 消息类型必须与后端 public.hpp 中的协议编号保持一致。Object.freeze 防止运行时误改协议常量。
const MSG = Object.freeze({
  LOGIN: 1,
  LOGIN_ACK: 2,
  LOGOUT: 3,
  REGISTER: 4,
  REGISTER_ACK: 5,
  ONE_CHAT: 6,
  ADD_FRIEND: 7,
  CREATE_GROUP: 8,
  ADD_GROUP: 9,
  GROUP_CHAT: 10,
  ACK: 11,
  HEARTBEAT: 12,
  HEARTBEAT_ACK: 13,
  ERROR: 14,
});

const state = {
  // 当前 WebSocket 和登录用户。断线后 socket 会替换；后端要求重新登录时 user 由用户退出流程清理。
  socket: null,
  user: null,
  // 登录响应携带的好友、群组快照，仅用于当前页面展示。
  friends: [],
  groups: [],
  // mode 决定左栏显示好友还是群；active 是正在打开的具体会话。
  mode: 'friends',
```

浏览器侧把尚未收到 ACK 的消息保存在 pending，定时器到期以相同 message_id 重发；独立心跳保持在线路由续期。ACK_OK/ACK_DEDUP 都会停止重试，ACK_FAIL 或超限则把消息状态标成失败供用户感知。

### 片段 2：第 31-59 行

```javascript
  active: null,
  // messages: conversationKey -> 消息数组；pending: message_id -> 等待 ACK 的发送上下文。
  messages: new Map(),
  pending: new Map(),
  // RabbitMQ 重投、确认超时后的离线兜底可能产生重复副本；最终展示层按全局 message_id 拦截。
  receivedMessageIds: new MessageIdDeduplicator(10_000),
  // sequence 为每个会话单独维护递增 client_seq，用于同一发送方、同一会话的顺序判断。
  sequence: new Map(),
  heartbeatTimer: null,
  retryTimer: null,
};

// 页面元素均为本地固定选择器；业务数据写入时统一使用 textContent，避免把消息内容解释成 HTML。
const $ = (selector) => document.querySelector(selector);
const authView = $('#auth-view');
const chatView = $('#chat-view');
const authMessage = $('#auth-message');
const messageList = $('#message-list');
const composer = $('#composer');
const messageInput = $('#message-input');
const sendButton = $('.send-button');

function showToast(message) {
  // 重复提示时先取消旧定时器，保证最后一条消息拥有完整展示时间。
  const toast = $('#toast');
  // textContent 不执行 <script>、事件属性等用户输入，是本页面最重要的 XSS 防线。
  toast.textContent = message;
  toast.classList.add('show');
  clearTimeout(showToast.timer);
  showToast.timer = setTimeout(() => toast.classList.remove('show'), 2400);
}
```

`receivedMessageIds` 独立于每个会话的消息数组：同一个 ID 即使通过 RabbitMQ 在线路径
和 MySQL 离线路径到达，或携带错误会话字段，也只能进入页面状态一次。容量限制避免
浏览器标签长期运行时集合无限增长。

### 片段 3：第 60-90 行

```javascript

function setConnection(online, text) {
  // 连接状态仅反映 WebSocket 是否可用，不等于用户已经通过后端身份认证。
  $('#connection-dot').classList.toggle('online', online);
  $('#connection-text').textContent = text;
}

function socketUrl() {
  // 跟随当前页面的安全级别：HTTPS 页面必须使用 WSS，避免浏览器拦截混合内容。
  const scheme = location.protocol === 'https:' ? 'wss' : 'ws';
  return `${scheme}://${location.host}/ws`;
}

function connect() {
  // CONNECTING(0) 和 OPEN(1) 时不重复创建连接，避免重连定时器造成连接风暴。
  if (state.socket && state.socket.readyState <= WebSocket.OPEN) return;
  setConnection(false, '正在连接聊天服务…');
  const socket = new WebSocket(socketUrl());
  state.socket = socket;

  socket.addEventListener('open', () => {
    setConnection(true, '聊天服务已连接');
    authMessage.textContent = '';
  });
  socket.addEventListener('message', ({ data }) => {
    try {
      // 网关保证收到的是一个 WebSocket 文本帧；这里再解析为业务 JSON。
      handleMessage(JSON.parse(data));
    } catch {
      showToast('收到无法解析的服务端消息');
    }
```

页面先修改内存中的登录、会话或消息状态，再把当前选中会话投影到 DOM。非当前会话的数据仍保存在 Map 中，切换列表后重新渲染；文本使用 textContent 创建，聊天内容不会被当成 HTML 执行。

### 片段 4：第 91-120 行

```javascript
  });
  socket.addEventListener('close', () => {
    // 断线时停止心跳和重试，防止定时器不断向 CLOSED socket 发送。
    setConnection(false, '连接已断开，正在重连…');
    stopBackgroundTasks();
    if (state.user) showToast('聊天连接已断开，请重新登录');
    // 固定短延迟适合演示环境；生产环境可进一步改为带随机抖动的指数退避。
    setTimeout(connect, 1600);
  });
  socket.addEventListener('error', () => setConnection(false, '聊天服务暂不可用'));
}

function send(payload) {
  // 统一发送入口只接受 OPEN 状态，调用者可通过布尔返回值决定是否建立本地“发送中”记录。
  if (!state.socket || state.socket.readyState !== WebSocket.OPEN) {
    showToast('聊天服务尚未连接');
    return false;
  }
  state.socket.send(JSON.stringify(payload));
  return true;
}

function parseEmbeddedList(values = []) {
  // C++ 登录响应中的 friends/groups/offlinemsg 可能是“JSON 字符串数组”，
  // 也兼容已经是对象的元素；单个损坏元素被跳过，不影响其余数据加载。
  return values.flatMap((item) => {
    try { return [typeof item === 'string' ? JSON.parse(item) : item]; }
    catch { return []; }
  });
}
```

这一状态段把连接存活、待确认消息和后台定时器分开保存。重连不会凭空确认旧消息，停止任务时也会清除 interval；ACK、超时和关闭事件分别修改自己负责的字段，避免一个布尔量同时代表多种状态。 发送前创建 UUID、递增当前会话序号并立即插入本地消息；随后登记 pending 和发送 payload，用户能即时看到“发送中”，ACK 再更新为已发送。

### 片段 5：第 121-154 行

```javascript

function handleMessage(message) {
  // 按 msgid 做协议分发。未知类型被忽略，避免前端因后端新增消息类型直接崩溃。
  switch (message.msgid) {
    case MSG.LOGIN_ACK:
      handleLoginAck(message);
      break;
    case MSG.REGISTER_ACK:
      if (message.errno === 0) {
        authMessage.style.color = '#1f6f61';
        authMessage.textContent = `账户创建成功，你的用户 ID 是 ${message.id}。请妥善保存。`;
        switchAuthTab('login');
        $('#login-id').value = message.id;
      } else {
        authMessage.style.color = '';
        authMessage.textContent = message.errmsg ?? '注册失败，请稍后重试';
      }
      break;
    case MSG.ONE_CHAT:
    case MSG.GROUP_CHAT:
      storeIncoming(message);
      break;
    case MSG.ACK:
      // ACK 只改变本地发送状态，不会把 ACK 本身显示成聊天消息。
      markAcknowledged(message);
      break;
    case MSG.HEARTBEAT_ACK:
      break;
    case MSG.ERROR:
      showToast(message.message ?? '请求被服务端拒绝');
      break;
    default:
      break;
  }
```

这部分属于“浏览器聊天客户端”的状态衔接代码。它只推进当前事件已经确认的结果；异步响应、未匹配消息或未完成缓冲仍保存在本模块中，后续事件到达后继续处理，不会被当作空结果丢弃。

### 片段 6：第 155-177 行

```javascript
}

function handleLoginAck(message) {
  // errno 非 0 时保持在登录页，不建立任何本地会话状态。
  if (message.errno !== 0) {
    authMessage.style.color = '';
    authMessage.textContent = message.errmsg ?? '登录失败';
    return;
  }

  // 登录成功后一次性加载后端返回的关系和离线消息快照。
  state.user = { id: Number(message.id), name: message.name };
  state.friends = parseEmbeddedList(message.friends);
  state.groups = parseEmbeddedList(message.groups).map((group) => ({
    ...group,
    users: parseEmbeddedList(group.users),
  }));
  authView.classList.add('hidden');
  chatView.classList.remove('hidden');
  $('#profile-name').textContent = state.user.name;
  $('#profile-id').textContent = `ID ${state.user.id}`;
  $('#profile-avatar').textContent = (state.user.name || 'U').slice(0, 1).toUpperCase();
  renderConversationList();
```

页面先修改内存中的登录、会话或消息状态，再把当前选中会话投影到 DOM。非当前会话的数据仍保存在 Map 中，切换列表后重新渲染；文本使用 textContent 创建，聊天内容不会被当成 HTML 执行。 登录成功后把用户、好友、群和离线消息一次写入页面状态；失败只显示错误，不启动心跳或切换到聊天界面。

### 片段 7：第 178-208 行

```javascript

  // 离线消息复用在线消息的入库逻辑，但批量恢复时不逐条弹出通知。
  for (const offline of parseEmbeddedList(message.offlinemsg)) storeIncoming(offline, false);
  startBackgroundTasks();
}

function conversationKey(message) {
  // 群聊天然由 groupid 定位；单聊则取“不是当前用户”的一方作为对端。
  // 统一 key 让发送消息和接收消息落入同一个会话桶。
  if (message.msgid === MSG.GROUP_CHAT) return `group:${message.groupid}`;
  const peerId = Number(message.id) === state.user?.id ? Number(message.toid) : Number(message.id);
  return `friend:${peerId}`;
}

function activeKey() {
  // 没有选中会话时返回 null，调用处通过空数组安全渲染初始状态。
  return state.active ? `${state.active.type}:${state.active.id}` : null;
}

function storeIncoming(message, notify = true) {
  if (!state.user) return;
  const key = conversationKey(message);
  if (!state.messages.has(key)) state.messages.set(key, []);
  const bucket = state.messages.get(key);
  // 使用独立有界集合而非扫描当前会话数组：同一 ID 即使从在线和离线两条路径到达，
  // 或携带异常会话字段，也只能进入展示状态一次。旧消息没有 ID 时继续兼容显示。
  if (state.receivedMessageIds.isDuplicate(message.message_id)) return;
  bucket.push({ ...message, delivery: '已送达' });
  if (activeKey() === key) renderMessages();
  else if (notify) showToast(`收到 ${message.name ?? message.id} 的新消息`);
}
```

消息先经过全局 ID 去重，再按会话 key 写入 `state.messages`。使用独立 Map 后，检查从
原来的会话数组 O(n) 扫描变成平均 O(1)，也能覆盖跨在线/离线路径的重复。

循环遍历 `messages`，把每个元素独立转换、投递或校验。结果按遍历顺序追加，某个元素失败时由本片段的状态变量或断言记录，不能用一次总体成功掩盖单项失败。

### 片段 8：第 209-237 行

```javascript

function markAcknowledged(message) {
  // 只处理仍在 pending 中的 ACK；迟到或重复 ACK 不会重复修改已经完成的消息。
  const pending = state.pending.get(message.message_id);
  if (!pending) return;
  const bucket = state.messages.get(pending.key) ?? [];
  const local = bucket.find((item) => item.message_id === message.message_id);
  // ack_state: 0=首次成功，1=服务端已去重（也视为成功），2=本次处理失败、允许同 ID 重试。
  if (message.ack_state === 2) {
    // 置零使下一轮定时扫描立即重试；保留相同 message_id，才能获得幂等语义。
    pending.sentAt = 0;
    if (local) local.delivery = '投递失败，正在重试';
  } else {
    state.pending.delete(message.message_id);
    if (local) local.delivery = message.ack_state === 1 ? '已去重' : '已送达';
  }
  if (activeKey() === pending.key) renderMessages();
}

function switchAuthTab(tab) {
  // 同时更新视觉类、ARIA 状态和表单可见性，保证键盘/读屏器语义与界面一致。
  const login = tab === 'login';
  $('#login-tab').classList.toggle('active', login);
  $('#register-tab').classList.toggle('active', !login);
  $('#login-tab').setAttribute('aria-selected', String(login));
  $('#register-tab').setAttribute('aria-selected', String(!login));
  $('#login-form').classList.toggle('hidden', !login);
  $('#register-form').classList.toggle('hidden', login);
}
```

页面先修改内存中的登录、会话或消息状态，再把当前选中会话投影到 DOM。非当前会话的数据仍保存在 Map 中，切换列表后重新渲染；文本使用 textContent 创建，聊天内容不会被当成 HTML 执行。 接收分发在这里处理统一错误和其他协议类型；ERROR_MSG 直接展示服务端 code/message，不能误触发登录注册的条件变量。

### 片段 9：第 238-263 行

```javascript

$('#login-tab').addEventListener('click', () => switchAuthTab('login'));
$('#register-tab').addEventListener('click', () => switchAuthTab('register'));

$('#login-form').addEventListener('submit', (event) => {
  // 阻止浏览器原生表单跳转，账号密码只通过当前 WebSocket JSON 请求发送。
  event.preventDefault();
  authMessage.style.color = '';
  authMessage.textContent = '正在验证账户…';
  send({
    msgid: MSG.LOGIN,
    id: Number($('#login-id').value),
    password: $('#login-password').value,
  });
});

$('#register-form').addEventListener('submit', (event) => {
  event.preventDefault();
  authMessage.style.color = '';
  authMessage.textContent = '正在创建账户…';
  send({
    msgid: MSG.REGISTER,
    name: $('#register-name').value.trim(),
    password: $('#register-password').value,
  });
});
```

这部分属于“浏览器聊天客户端”的状态衔接代码。它只推进当前事件已经确认的结果；异步响应、未匹配消息或未完成缓冲仍保存在本模块中，后续事件到达后继续处理，不会被当作空结果丢弃。 接收分发在这里处理统一错误和其他协议类型；ERROR_MSG 直接展示服务端 code/message，不能误触发登录注册的条件变量。

### 片段 10：第 264-287 行

```javascript

function switchList(mode) {
  // 切换分类不会清空 active 和历史消息；重新渲染时会按当前分类高亮匹配项。
  state.mode = mode;
  $('#friends-tab').classList.toggle('active', mode === 'friends');
  $('#groups-tab').classList.toggle('active', mode === 'groups');
  renderConversationList();
}

$('#friends-tab').addEventListener('click', () => switchList('friends'));
$('#groups-tab').addEventListener('click', () => switchList('groups'));

function renderConversationList() {
  // 每次依据 state 全量重建小型会话列表，减少手工维护 DOM 与状态不同步的风险。
  const root = $('#conversation-list');
  root.replaceChildren();
  const items = state.mode === 'friends' ? state.friends : state.groups;
  if (items.length === 0) {
    const empty = document.createElement('p');
    empty.className = 'conversation-meta';
    empty.textContent = state.mode === 'friends' ? '还没有好友' : '还没有加入群组';
    root.append(empty);
    return;
  }
```

页面先修改内存中的登录、会话或消息状态，再把当前选中会话投影到 DOM。非当前会话的数据仍保存在 Map 中，切换列表后重新渲染；文本使用 textContent 创建，聊天内容不会被当成 HTML 执行。 接收分发在这里处理统一错误和其他协议类型；ERROR_MSG 直接展示服务端 code/message，不能误触发登录注册的条件变量。 源码在这一段特别限定了“切换分类不会清空 active 和历史消息；重新渲染时会按当前分类高亮匹配项。”，因此解释范围止于该局部步骤。

### 片段 11：第 288-310 行

```javascript

  for (const item of items) {
    const type = state.mode === 'friends' ? 'friend' : 'group';
    const name = type === 'friend' ? item.name : item.groupname;
    const button = document.createElement('button');
    button.type = 'button';
    button.className = 'conversation-item';
    button.classList.toggle('active', state.active?.type === type && state.active?.id === Number(item.id));

    const avatar = document.createElement('div');
    avatar.className = 'avatar';
    // name 来自服务端，所有显示节点均通过 textContent 创建，不使用 innerHTML。
    avatar.textContent = (name || '?').slice(0, 1).toUpperCase();
    const text = document.createElement('span');
    const strong = document.createElement('strong');
    strong.textContent = name;
    const small = document.createElement('small');
    small.textContent = type === 'friend' ? `ID ${item.id} · ${item.state === 'online' ? '在线' : '离线'}` : `群 ID ${item.id}`;
    text.append(strong, small);
    button.append(avatar, text);
    button.addEventListener('click', () => selectConversation(type, item));
    root.append(button);
  }
```

页面先修改内存中的登录、会话或消息状态，再把当前选中会话投影到 DOM。非当前会话的数据仍保存在 Map 中，切换列表后重新渲染；文本使用 textContent 创建，聊天内容不会被当成 HTML 执行。 选择联系人或群后只更新 active 描述并重绘已有消息；历史消息早已按 key 存在 Map 中，不需要向服务器重新拉取。

### 片段 12：第 311-346 行

```javascript
}

function selectConversation(type, item) {
  // active 只保留渲染和构造消息所需字段，避免后续直接修改 friends/groups 原对象。
  state.active = {
    type,
    id: Number(item.id),
    name: type === 'friend' ? item.name : item.groupname,
  };
  $('#conversation-title').textContent = state.active.name;
  $('#conversation-meta').textContent = `${type === 'friend' ? '用户' : '群组'} ID ${item.id}`;
  messageInput.disabled = false;
  sendButton.disabled = false;
  renderConversationList();
  renderMessages();
  messageInput.focus();
}

function renderMessages() {
  // replaceChildren 清除旧 DOM 后按内存状态重绘；当前实现适合演示数据量，
  // 大规模历史记录可改为虚拟列表，但不影响消息协议与可靠性逻辑。
  messageList.replaceChildren();
  const messages = state.messages.get(activeKey()) ?? [];
  if (messages.length === 0) {
    const empty = document.createElement('div');
    empty.className = 'empty-state';
    const icon = document.createElement('span');
    icon.textContent = '始';
    const title = document.createElement('h3');
    title.textContent = '从第一句话开始';
    const copy = document.createElement('p');
    copy.textContent = '消息确认与失败重试会在后台自动完成。';
    empty.append(icon, title, copy);
    messageList.append(empty);
    return;
  }
```

页面先修改内存中的登录、会话或消息状态，再把当前选中会话投影到 DOM。非当前会话的数据仍保存在 Map 中，切换列表后重新渲染；文本使用 textContent 创建，聊天内容不会被当成 HTML 执行。 联系人与群组共用一个列表容器，mode 决定数据源和会话类型；重绘时保留当前 active key，使切换列表不会丢失已选会话。

### 片段 13：第 347-370 行

```javascript

  for (const message of messages) {
    const mine = Number(message.id) === state.user.id;
    const row = document.createElement('article');
    row.className = `message-row${mine ? ' mine' : ''}`;
    const bubble = document.createElement('div');
    bubble.className = 'bubble';
    const header = document.createElement('div');
    header.className = 'bubble-header';
    const author = document.createElement('span');
    author.textContent = mine ? '我' : (message.name ?? `用户 ${message.id}`);
    const time = document.createElement('span');
    time.textContent = message.time ?? '';
    const content = document.createElement('p');
    // 聊天正文可能包含引号、换行或看似 HTML 的文本；textContent 保证全部按纯文本展示。
    content.textContent = message.msg ?? '';
    header.append(author, time);
    bubble.append(header, content);
    if (mine) {
      const delivery = document.createElement('small');
      delivery.className = 'delivery';
      delivery.textContent = message.delivery ?? '发送中';
      bubble.append(delivery);
    }
```

页面先修改内存中的登录、会话或消息状态，再把当前选中会话投影到 DOM。非当前会话的数据仍保存在 Map 中，切换列表后重新渲染；文本使用 textContent 创建，聊天内容不会被当成 HTML 执行。 渲染根据发送者 id 决定左右方向，并把正文作为文本节点写入；pending、sent、failed 状态映射为不同提示，特殊字符串不会执行为 HTML。

### 片段 14：第 371-395 行

```javascript
    row.append(bubble);
    messageList.append(row);
  }
  messageList.scrollTop = messageList.scrollHeight;
}

function nextSequence(key) {
  // client_seq 只在当前浏览器登录周期内、按会话递增；它用于检测/展示顺序，
  // 真正的重复投递防护依赖全局唯一 message_id。
  const next = (state.sequence.get(key) ?? 0) + 1;
  state.sequence.set(key, next);
  return next;
}

function currentTime() {
  // time 是面向用户的可读时间；服务端可靠性判断不应把它当作可信的排序依据。
  return new Intl.DateTimeFormat('zh-CN', {
    hour: '2-digit', minute: '2-digit', second: '2-digit', hour12: false,
  }).format(new Date());
}

composer.addEventListener('submit', (event) => {
  event.preventDefault();
  const content = messageInput.value.trim();
  if (!content || !state.active || !state.user) return;
```

顺序只在同一发送者的同一会话内定义，`client_seq` 每发送一条递增。接收端可据此缓存较大的序号、丢弃已展示的旧序号；不同会话或不同发送者之间不强行建立全局顺序。

### 片段 15：第 396-419 行

```javascript

  const key = activeKey();
  // UUID 在一次逻辑发送及其所有重试中保持不变，服务端据此执行分布式去重。
  const messageId = crypto.randomUUID();
  const payload = {
    msgid: state.active.type === 'friend' ? MSG.ONE_CHAT : MSG.GROUP_CHAT,
    id: state.user.id,
    name: state.user.name,
    msg: content,
    time: currentTime(),
    client_seq: nextSequence(key),
    message_id: messageId,
  };
  if (state.active.type === 'friend') payload.toid = state.active.id;
  else payload.groupid = state.active.id;

  // 只有真正交给 WebSocket 后才进行乐观展示并登记 pending，避免未连接消息显示成已发送。
  if (!send(payload)) return;
  if (!state.messages.has(key)) state.messages.set(key, []);
  state.messages.get(key).push({ ...payload, delivery: '发送中' });
  state.pending.set(messageId, { payload, key, sentAt: Date.now(), retries: 0 });
  messageInput.value = '';
  renderMessages();
});
```

页面先修改内存中的登录、会话或消息状态，再把当前选中会话投影到 DOM。非当前会话的数据仍保存在 Map 中，切换列表后重新渲染；文本使用 textContent 创建，聊天内容不会被当成 HTML 执行。 群聊以 groupId 作为会话序号维度，生成唯一 message_id 后先登记 pending 再发帧；重试沿用同一 payload，服务端才能幂等。

### 片段 16：第 420-445 行

```javascript

function startBackgroundTasks() {
  // 每次登录前先清理旧定时器，确保同一页面最多只有一组心跳与重试任务。
  stopBackgroundTasks();
  state.heartbeatTimer = setInterval(() => {
    // 心跳让网关/后端刷新空闲超时，同时由后端验证连接绑定的用户身份。
    if (state.user) send({ msgid: MSG.HEARTBEAT, id: state.user.id, ts: Date.now() });
  }, 10_000);
  state.retryTimer = setInterval(() => {
    // 每秒扫描未确认消息；发送后 5 秒未获得 ACK 才重试，最多重发 3 次。
    const now = Date.now();
    for (const [messageId, pending] of state.pending) {
      if (now - pending.sentAt < 5_000) continue;
      if (pending.retries >= 3) {
        // 达到上限后停止自动重试并显式标记失败，避免永久占用内存或无限制造流量。
        state.pending.delete(messageId);
        const local = (state.messages.get(pending.key) ?? []).find((item) => item.message_id === messageId);
        if (local) local.delivery = '发送失败';
        if (activeKey() === pending.key) renderMessages();
        continue;
      }
      if (send(pending.payload)) {
        // payload（尤其 message_id/client_seq）原样复用，服务端才能把网络重试识别为同一逻辑消息。
        pending.sentAt = now;
        pending.retries += 1;
      }
```

循环每次只消费已经确认完整的字节或已经成功写出的部分。遇到正文尚未到齐便停在当前偏移，下一次收到数据后继续；发送短写则从剩余位置续发，这正是流式 socket 不能假设“一次调用完成一条消息”的原因。

### 片段 17：第 446-469 行

```javascript
    }
  }, 1_000);
}

function stopBackgroundTasks() {
  // clearInterval(null) 是安全操作，因此本函数可在断线、退出和重新登录时重复调用。
  clearInterval(state.heartbeatTimer);
  clearInterval(state.retryTimer);
  state.heartbeatTimer = null;
  state.retryTimer = null;
}

$('#logout-button').addEventListener('click', () => {
  // 先通知后端释放在线路由，再清除当前页面中的敏感会话数据与待确认消息。
  if (state.user) send({ msgid: MSG.LOGOUT, id: state.user.id });
  stopBackgroundTasks();
  state.user = null;
  state.active = null;
  state.messages.clear();
  state.pending.clear();
  state.receivedMessageIds.clear();
  chatView.classList.add('hidden');
  authView.classList.remove('hidden');
  authMessage.textContent = '已安全退出';
});
```

主动退出同时清空消息、pending 和接收去重集合，防止下一位用户在同一标签页继承前一位
用户的数据。短暂网络断线不会走此分支，所以重连后的重复消息仍可被识别。

### 片段 18：第 470-492 行

```javascript

const dialog = $('#action-dialog');
$('#new-conversation').addEventListener('click', () => {
  // 同一个 dialog 根据左栏模式复用：好友模式要求 ID，群组模式可填写 ID 加入或填写名称创建。
  const friendMode = state.mode === 'friends';
  $('#dialog-title').textContent = friendMode ? '添加好友' : '创建或加入群组';
  $('#dialog-id-label').childNodes[0].textContent = friendMode ? '好友 ID' : '群 ID（填写则加入）';
  $('#dialog-id').required = friendMode;
  $('#dialog-name-label').classList.toggle('hidden', friendMode);
  $('#dialog-desc-label').classList.toggle('hidden', friendMode);
  dialog.showModal();
});
$('#dialog-cancel').addEventListener('click', () => dialog.close());

$('#action-form').addEventListener('submit', (event) => {
  event.preventDefault();
  const id = Number($('#dialog-id').value);
  if (state.mode === 'friends') {
    if (id > 0 && send({ msgid: MSG.ADD_FRIEND, id: state.user.id, friendid: id })) {
      // 当前页面先加入占位项；真实名称和最终关系会在下次登录时从后端快照校准。
      state.friends.push({ id, name: `用户 ${id}`, state: 'offline' });
      showToast('好友请求已提交');
    }
```

页面先修改内存中的登录、会话或消息状态，再把当前选中会话投影到 DOM。非当前会话的数据仍保存在 Map 中，切换列表后重新渲染；文本使用 textContent 创建，聊天内容不会被当成 HTML 执行。 渲染根据发送者 id 决定左右方向，并把正文作为文本节点写入；pending、sent、failed 状态映射为不同提示，特殊字符串不会执行为 HTML。 源码在这一段特别限定了“同一个 dialog 根据左栏模式复用：好友模式要求 ID，群组模式可填写 ID 加入或填写名称创建。”，因此解释范围止于该局部步骤。

### 片段 19：第 493-512 行

```javascript
  } else if (id > 0) {
    send({ msgid: MSG.ADD_GROUP, id: state.user.id, groupid: id });
    state.groups.push({ id, groupname: `群组 ${id}`, groupdesc: '', users: [] });
    showToast('已提交入群请求');
  } else {
    const groupname = $('#dialog-name').value.trim();
    if (groupname && send({
      msgid: MSG.CREATE_GROUP,
      id: state.user.id,
      groupname,
      groupdesc: $('#dialog-desc').value.trim(),
    })) showToast('群组创建请求已提交，重新登录后刷新列表');
  }
  dialog.close();
  renderConversationList();
  $('#action-form').reset();
});

// 页面脚本加载完成后立即连接，但仍需用户提交登录/注册请求才能建立业务会话。
connect();
```

页面先修改内存中的登录、会话或消息状态，再把当前选中会话投影到 DOM。非当前会话的数据仍保存在 Map 中，切换列表后重新渲染；文本使用 textContent 创建，聊天内容不会被当成 HTML 执行。 联系人与群组共用一个列表容器，mode 决定数据源和会话类型；重绘时保留当前 active key，使切换列表不会丢失已选会话。 源码在这一段特别限定了“页面脚本加载完成后立即连接，但仍需用户提交登录/注册请求才能建立业务会话。”，因此解释范围止于该局部步骤。

## 面试重点

- 这个文件处于哪一层，它保存的数据由谁创建、由谁消费？

- 如果删除或修改本文件，最先受影响的运行链路是什么？

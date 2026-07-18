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
  socket: null,
  user: null,
  friends: [],
  groups: [],
  mode: 'friends',
  active: null,
  messages: new Map(),
  pending: new Map(),
  sequence: new Map(),
  heartbeatTimer: null,
  retryTimer: null,
};

const $ = (selector) => document.querySelector(selector);
const authView = $('#auth-view');
const chatView = $('#chat-view');
const authMessage = $('#auth-message');
const messageList = $('#message-list');
const composer = $('#composer');
const messageInput = $('#message-input');
const sendButton = $('.send-button');

function showToast(message) {
  const toast = $('#toast');
  toast.textContent = message;
  toast.classList.add('show');
  clearTimeout(showToast.timer);
  showToast.timer = setTimeout(() => toast.classList.remove('show'), 2400);
}

function setConnection(online, text) {
  $('#connection-dot').classList.toggle('online', online);
  $('#connection-text').textContent = text;
}

function socketUrl() {
  const scheme = location.protocol === 'https:' ? 'wss' : 'ws';
  return `${scheme}://${location.host}/ws`;
}

function connect() {
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
      handleMessage(JSON.parse(data));
    } catch {
      showToast('收到无法解析的服务端消息');
    }
  });
  socket.addEventListener('close', () => {
    setConnection(false, '连接已断开，正在重连…');
    stopBackgroundTasks();
    if (state.user) showToast('聊天连接已断开，请重新登录');
    setTimeout(connect, 1600);
  });
  socket.addEventListener('error', () => setConnection(false, '聊天服务暂不可用'));
}

function send(payload) {
  if (!state.socket || state.socket.readyState !== WebSocket.OPEN) {
    showToast('聊天服务尚未连接');
    return false;
  }
  state.socket.send(JSON.stringify(payload));
  return true;
}

function parseEmbeddedList(values = []) {
  return values.flatMap((item) => {
    try { return [typeof item === 'string' ? JSON.parse(item) : item]; }
    catch { return []; }
  });
}

function handleMessage(message) {
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
}

function handleLoginAck(message) {
  if (message.errno !== 0) {
    authMessage.style.color = '';
    authMessage.textContent = message.errmsg ?? '登录失败';
    return;
  }

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

  for (const offline of parseEmbeddedList(message.offlinemsg)) storeIncoming(offline, false);
  startBackgroundTasks();
}

function conversationKey(message) {
  if (message.msgid === MSG.GROUP_CHAT) return `group:${message.groupid}`;
  const peerId = Number(message.id) === state.user?.id ? Number(message.toid) : Number(message.id);
  return `friend:${peerId}`;
}

function activeKey() {
  return state.active ? `${state.active.type}:${state.active.id}` : null;
}

function storeIncoming(message, notify = true) {
  if (!state.user) return;
  const key = conversationKey(message);
  if (!state.messages.has(key)) state.messages.set(key, []);
  const bucket = state.messages.get(key);
  if (message.message_id && bucket.some((item) => item.message_id === message.message_id)) return;
  bucket.push({ ...message, delivery: '已送达' });
  if (activeKey() === key) renderMessages();
  else if (notify) showToast(`收到 ${message.name ?? message.id} 的新消息`);
}

function markAcknowledged(message) {
  const pending = state.pending.get(message.message_id);
  if (!pending) return;
  const bucket = state.messages.get(pending.key) ?? [];
  const local = bucket.find((item) => item.message_id === message.message_id);
  if (message.ack_state === 2) {
    pending.sentAt = 0;
    if (local) local.delivery = '投递失败，正在重试';
  } else {
    state.pending.delete(message.message_id);
    if (local) local.delivery = message.ack_state === 1 ? '已去重' : '已送达';
  }
  if (activeKey() === pending.key) renderMessages();
}

function switchAuthTab(tab) {
  const login = tab === 'login';
  $('#login-tab').classList.toggle('active', login);
  $('#register-tab').classList.toggle('active', !login);
  $('#login-tab').setAttribute('aria-selected', String(login));
  $('#register-tab').setAttribute('aria-selected', String(!login));
  $('#login-form').classList.toggle('hidden', !login);
  $('#register-form').classList.toggle('hidden', login);
}

$('#login-tab').addEventListener('click', () => switchAuthTab('login'));
$('#register-tab').addEventListener('click', () => switchAuthTab('register'));

$('#login-form').addEventListener('submit', (event) => {
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

function switchList(mode) {
  state.mode = mode;
  $('#friends-tab').classList.toggle('active', mode === 'friends');
  $('#groups-tab').classList.toggle('active', mode === 'groups');
  renderConversationList();
}

$('#friends-tab').addEventListener('click', () => switchList('friends'));
$('#groups-tab').addEventListener('click', () => switchList('groups'));

function renderConversationList() {
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

  for (const item of items) {
    const type = state.mode === 'friends' ? 'friend' : 'group';
    const name = type === 'friend' ? item.name : item.groupname;
    const button = document.createElement('button');
    button.type = 'button';
    button.className = 'conversation-item';
    button.classList.toggle('active', state.active?.type === type && state.active?.id === Number(item.id));

    const avatar = document.createElement('div');
    avatar.className = 'avatar';
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
}

function selectConversation(type, item) {
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
    content.textContent = message.msg ?? '';
    header.append(author, time);
    bubble.append(header, content);
    if (mine) {
      const delivery = document.createElement('small');
      delivery.className = 'delivery';
      delivery.textContent = message.delivery ?? '发送中';
      bubble.append(delivery);
    }
    row.append(bubble);
    messageList.append(row);
  }
  messageList.scrollTop = messageList.scrollHeight;
}

function nextSequence(key) {
  const next = (state.sequence.get(key) ?? 0) + 1;
  state.sequence.set(key, next);
  return next;
}

function currentTime() {
  return new Intl.DateTimeFormat('zh-CN', {
    hour: '2-digit', minute: '2-digit', second: '2-digit', hour12: false,
  }).format(new Date());
}

composer.addEventListener('submit', (event) => {
  event.preventDefault();
  const content = messageInput.value.trim();
  if (!content || !state.active || !state.user) return;

  const key = activeKey();
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

  if (!send(payload)) return;
  if (!state.messages.has(key)) state.messages.set(key, []);
  state.messages.get(key).push({ ...payload, delivery: '发送中' });
  state.pending.set(messageId, { payload, key, sentAt: Date.now(), retries: 0 });
  messageInput.value = '';
  renderMessages();
});

function startBackgroundTasks() {
  stopBackgroundTasks();
  state.heartbeatTimer = setInterval(() => {
    if (state.user) send({ msgid: MSG.HEARTBEAT, id: state.user.id, ts: Date.now() });
  }, 10_000);
  state.retryTimer = setInterval(() => {
    const now = Date.now();
    for (const [messageId, pending] of state.pending) {
      if (now - pending.sentAt < 5_000) continue;
      if (pending.retries >= 3) {
        state.pending.delete(messageId);
        const local = (state.messages.get(pending.key) ?? []).find((item) => item.message_id === messageId);
        if (local) local.delivery = '发送失败';
        if (activeKey() === pending.key) renderMessages();
        continue;
      }
      if (send(pending.payload)) {
        pending.sentAt = now;
        pending.retries += 1;
      }
    }
  }, 1_000);
}

function stopBackgroundTasks() {
  clearInterval(state.heartbeatTimer);
  clearInterval(state.retryTimer);
  state.heartbeatTimer = null;
  state.retryTimer = null;
}

$('#logout-button').addEventListener('click', () => {
  if (state.user) send({ msgid: MSG.LOGOUT, id: state.user.id });
  stopBackgroundTasks();
  state.user = null;
  state.active = null;
  state.messages.clear();
  state.pending.clear();
  chatView.classList.add('hidden');
  authView.classList.remove('hidden');
  authMessage.textContent = '已安全退出';
});

const dialog = $('#action-dialog');
$('#new-conversation').addEventListener('click', () => {
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
      state.friends.push({ id, name: `用户 ${id}`, state: 'offline' });
      showToast('好友请求已提交');
    }
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

connect();

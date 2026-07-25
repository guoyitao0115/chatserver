# `full-stack.mjs` 讲解

## 作用概览

**全栈冒烟测试。** 通过真实 WebSocket 网关注册两个用户、登录并发送消息，验证最短的浏览器协议到 C++ 后端往返链路。

阅读位置：`web/test/full-stack.mjs`。下文严格按源码顺序展示，每一行只出现一次；解释只针对紧邻的代码片段。

## 代码片段与详细讲解

### 片段 1：第 1-24 行

```javascript
import assert from 'node:assert/strict';
import crypto from 'node:crypto';
import net from 'node:net';

// 基础全链路冒烟测试：从真实 WebSocket 网关进入，依次经过 C++ 服务、Redis 路由、
// RabbitMQ 跨节点转发和 MySQL 离线存储。port1/port2 相同也可退化为单节点验证。
const host = process.env.CHAT_E2E_HOST ?? '127.0.0.1';
const port1 = Number(process.env.CHAT_E2E_PORT_1 ?? 8080);
const port2 = Number(process.env.CHAT_E2E_PORT_2 ?? 8081);

function encodeClientFrame(value) {
  // 生成浏览器语义的 FIN 文本帧，并按协议对客户端负载做随机掩码。
  const body = Buffer.from(JSON.stringify(value));
  const mask = crypto.randomBytes(4);
  const header = body.length < 126 ? Buffer.allocUnsafe(6) : Buffer.allocUnsafe(8);
  header[0] = 0x81;
  if (body.length < 126) {
    header[1] = 0x80 | body.length;
    mask.copy(header, 2);
  } else {
    header[1] = 0x80 | 126;
    header.writeUInt16BE(body.length, 2);
    mask.copy(header, 4);
  }
```

这些依赖明确了本片段所在层的边界：JSON、MySQL C API、Redis、RabbitMQ、密码或随机数、断言框架、TCP socket。项目内部头文件提供协议和领域对象，外部库只承担基础能力；业务数据如何流转仍由当前模块决定。

本片段读取 `CHAT_E2E_HOST`、`CHAT_E2E_PORT_1`、`CHAT_E2E_PORT_2`。未设置时采用紧邻的本机默认值；容器部署则覆盖这些值，因此同一二进制可以作为不同节点运行，无需重新编译。

### 片段 2：第 25-48 行

```javascript
  const masked = Buffer.from(body);
  for (let i = 0; i < masked.length; i += 1) masked[i] ^= mask[i % 4];
  return Buffer.concat([header, masked]);
}

function parseFrames(buffer, onMessage) {
  // 增量解析服务端无掩码帧；数据不足时保留尾部，覆盖 TCP 半包和粘包。
  let offset = 0;
  while (buffer.length - offset >= 2) {
    let length = buffer[offset + 1] & 0x7f;
    let header = 2;
    if (length === 126) {
      if (buffer.length - offset < 4) break;
      length = buffer.readUInt16BE(offset + 2);
      header = 4;
    } else if (length === 127) {
      if (buffer.length - offset < 10) break;
      length = Number(buffer.readBigUInt64BE(offset + 2));
      header = 10;
    }
    if (buffer.length - offset < header + length) break;
    onMessage(JSON.parse(buffer.subarray(offset + header, offset + header + length).toString()));
    offset += header + length;
  }
```

循环每次只消费已经确认完整的字节或已经成功写出的部分。遇到正文尚未到齐便停在当前偏移，下一次收到数据后继续；发送短写则从剩余位置续发，这正是流式 socket 不能假设“一次调用完成一条消息”的原因。

### 片段 3：第 49-79 行

```javascript
  return buffer.subarray(offset);
}

async function createClient(port) {
  // 直接使用 net 完成 WebSocket 握手，保证测试没有绕过待测网关。
  const socket = net.createConnection({ host, port });
  const key = crypto.randomBytes(16).toString('base64');
  let upgraded = false;
  let raw = Buffer.alloc(0);
  let frames = Buffer.alloc(0);
  const messages = [];
  const waiters = [];

  const deliver = (message) => {
    // 支持“先收到后等待”和“先等待后收到”两种时序，避免异步测试自身丢消息。
    const index = waiters.findIndex((waiter) => waiter.predicate(message));
    if (index >= 0) waiters.splice(index, 1)[0].resolve(message);
    else messages.push(message);
  };
  socket.on('data', (chunk) => {
    if (!upgraded) {
      // 握手响应可能被拆分；完整响应后的剩余字节可能已经是第一条业务消息。
      raw = Buffer.concat([raw, chunk]);
      const marker = raw.indexOf('\r\n\r\n');
      if (marker < 0) return;
      assert.match(raw.subarray(0, marker).toString(), /^HTTP\/1\.1 101/);
      upgraded = true;
      frames = raw.subarray(marker + 4);
    } else frames = Buffer.concat([frames, chunk]);
    frames = parseFrames(frames, deliver);
  });
```

断言读取的是本次操作产生的协议响应和可观察状态。因此通过意味着当前场景的业务后果正确；例如收到 ACK 但目标端数量不足，仍会被后续数量断言判为消息丢失。

### 片段 4：第 80-107 行

```javascript

  await new Promise((resolve, reject) => {
    // 限定 5 秒握手时间，服务未启动或网关不可用时快速给出明确失败原因。
    socket.once('connect', () => socket.write([
      'GET /ws HTTP/1.1', `Host: ${host}:${port}`, 'Upgrade: websocket',
      'Connection: Upgrade', `Sec-WebSocket-Key: ${key}`, 'Sec-WebSocket-Version: 13', '\r\n',
    ].join('\r\n')));
    const deadline = setTimeout(() => reject(new Error(`handshake timeout on ${port}`)), 5_000);
    const poll = setInterval(() => {
      if (upgraded) { clearInterval(poll); clearTimeout(deadline); resolve(); }
    }, 10);
    socket.once('error', reject);
  });

  return {
    send(value) { socket.write(encodeClientFrame(value)); },
    waitFor(predicate) {
      // 谓词按 msgid/message_id 选择目标消息，因此 ACK 和接收消息交错也不会误配。
      const index = messages.findIndex(predicate);
      if (index >= 0) return Promise.resolve(messages.splice(index, 1)[0]);
      return new Promise((resolve, reject) => {
        const timer = setTimeout(() => reject(new Error(`message timeout on ${port}`)), 8_000);
        waiters.push({ predicate, resolve(value) { clearTimeout(timer); resolve(value); } });
      });
    },
    close() { socket.destroy(); },
  };
}
```

接收消息可能早于测试开始等待，所以代码先尝试匹配已有等待器，未命中的消息进入积压队列。删除等待器时同时清理超时器；按谓词而不是 FIFO 匹配，允许 ACK、投递和心跳响应交错到达而不串场。

### 片段 5：第 108-129 行

```javascript

const suffix = `${Date.now()}-${crypto.randomInt(1000, 9999)}`;
// 每次生成唯一用户名，避免重复执行测试时撞到数据库已有记录。
const password = 'interview-demo-123';
const client1 = await createClient(port1);
const client2 = await createClient(port2);
let reconnectedClient2 = null;

try {
  // 先在两个入口各注册并登录一个用户，建立跨节点投递所需的在线路由。
  client1.send({ msgid: 4, name: `用户甲-${suffix}`, password });
  const reg1 = await client1.waitFor((m) => m.msgid === 5);
  assert.equal(reg1.errno, 0);

  client2.send({ msgid: 4, name: `用户乙-${suffix}`, password });
  const reg2 = await client2.waitFor((m) => m.msgid === 5);
  assert.equal(reg2.errno, 0);

  client1.send({ msgid: 1, id: reg1.id, password });
  assert.equal((await client1.waitFor((m) => m.msgid === 2)).errno, 0);
  client2.send({ msgid: 1, id: reg2.id, password });
  assert.equal((await client2.waitFor((m) => m.msgid === 2)).errno, 0);
```

断言读取的是注册或登录结果码。因此通过意味着当前场景的业务后果正确；例如收到 ACK 但目标端数量不足，仍会被后续数量断言判为消息丢失。

### 片段 6：第 130-153 行

```javascript

  // message_id 是幂等键，client_seq 表示该发送方会话内顺序；先验证成功 ACK 和接收端正文。
  const messageId = `cross-node-${suffix}`;
  // 故意把 id 伪造成另一用户，确认后端依据“连接绑定身份”拒绝越权，而非信任 JSON id。
  client1.send({
    msgid: 6, id: reg1.id, toid: reg2.id, name: `用户甲-${suffix}`,
    msg: '跨节点全链路消息', time: new Date().toISOString(),
    message_id: messageId, client_seq: 1,
  });
  const ack = await client1.waitFor((m) => m.msgid === 11 && m.message_id === messageId);
  assert.equal(ack.ack_state, 0);
  const delivered = await client2.waitFor((m) => m.msgid === 6 && m.message_id === messageId);
  assert.equal(delivered.msg, '跨节点全链路消息');

  client1.send({
    msgid: 6, id: reg2.id, toid: reg1.id, name: '冒用身份', msg: '不应送达',
    message_id: `spoof-${suffix}`, client_seq: 2,
  });
  const authError = await client1.waitFor((m) => m.msgid === 14);
  assert.equal(authError.code, 401);

  // 心跳验证长连接存活和用户路由续期的完整请求/响应路径。
  client1.send({ msgid: 12, id: reg1.id, ts: Date.now() });
  assert.equal((await client1.waitFor((m) => m.msgid === 13)).msgid, 13);
```

这一组先验证稳定在线后的第二次登录被拒绝，再让 Alice 连接在 JSON 中冒充 Carol，必须收到 401。好友和建群操作后主动登出重登，登录响应中仍能查到关系，证明数据确实写入 MySQL 而非只留在进程内存。

`message_id` 贯穿发送、ACK、重试和接收去重：同一业务消息重发时 id 不变，服务端才能识别重复；`ack_state` 则告诉发送者是已接受、已去重还是处理失败，而不是仅凭 TCP 写成功判断业务成功。

### 片段 7：第 154-177 行

```javascript

  // 验证离线兜底与重新登录拉取：先正常退出并等待路由清理，再发送消息。
  client2.send({ msgid: 3, id: reg2.id });
  await new Promise((resolve) => setTimeout(resolve, 300));
  client2.close();
  const offlineMessageId = `offline-${suffix}`;
  client1.send({
    msgid: 6, id: reg1.id, toid: reg2.id, name: `用户甲-${suffix}`,
    msg: '离线消息', time: new Date().toISOString(),
    message_id: offlineMessageId, client_seq: 3,
  });
  assert.equal((await client1.waitFor(
    (m) => m.msgid === 11 && m.message_id === offlineMessageId,
  )).ack_state, 0);

  reconnectedClient2 = await createClient(port2);
  reconnectedClient2.send({ msgid: 1, id: reg2.id, password });
  const relogin = await reconnectedClient2.waitFor((m) => m.msgid === 2);
  assert.equal(relogin.errno, 0);
  // 登录响应必须包含刚才的 message_id，说明跨节点在线路径不可用时正确落入 MySQL 离线表。
  assert.ok(
    (relogin.offlinemsg ?? []).some((item) => JSON.parse(item).message_id === offlineMessageId),
    'offline message should be returned after relogin',
  );
```

场景断言同时观察协议响应和对端实际结果：仅有发送成功或 ACK 并不足以证明消息送达。数量、字段、错误码或“观察窗口内没有额外消息”共同限定了本片段要验证的系统性质。

### 片段 8：第 178-189 行

```javascript

  console.log(JSON.stringify({
    status: 'passed',
    users: [reg1.id, reg2.id],
    checks: ['register', 'login', 'websocket', 'tcp-frame', 'redis-route', 'rabbitmq-cross-node', 'ack', 'auth-rejection', 'heartbeat', 'offline-message'],
  }, null, 2));
} finally {
  // 无论断言成功与否均关闭连接，避免失败用例影响下一次测试。
  client1.close();
  client2.close();
  reconnectedClient2?.close();
}
```

`finally` 会遍历本轮登记的客户端并关闭连接。即使中途断言抛错，服务端也能触发断线清理，避免残留在线路由让下一轮重复登录或离线场景得到假结果。

## 面试重点

- 测试准备了什么外部状态或模拟组件，实际动作经过哪些模块？

- 每个断言证明的是返回值正确，还是“不丢、不重、不乱序、不可冒用”等系统性质？

- 如何避免测试自身的等待竞态和上轮残留状态造成假失败？

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

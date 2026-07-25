# `gateway.test.mjs` 讲解

## 作用概览

**网关隔离测试。** 用本地模拟 TCP 后端验证 HTTP 健康检查、WebSocket 握手、双向帧转换和非法负载拦截，不让数据库或消息队列故障干扰网关定位。

阅读位置：`web/test/gateway.test.mjs`。下文严格按源码顺序展示，每一行只出现一次；解释只针对紧邻的代码片段。

## 代码片段与详细讲解

### 片段 1：第 1-33 行

```javascript
import assert from 'node:assert/strict';
import crypto from 'node:crypto';
import net from 'node:net';
import test from 'node:test';

import { createGateway } from '../gateway.mjs';

// 网关隔离测试：用本地模拟 TCP 后端验证 HTTP、WebSocket 握手以及双向协议桥接，
// 不依赖 MySQL、Redis、RabbitMQ 或真实 C++ 服务。

function tcpFrame(value) {
  // 模拟后端的“4 字节大端长度 + JSON”帧。
  const body = Buffer.from(JSON.stringify(value));
  const frame = Buffer.allocUnsafe(4 + body.length);
  frame.writeUInt32BE(body.length, 0);
  body.copy(frame, 4);
  return frame;
}

function maskedWebSocketFrame(value) {
  // 模拟浏览器发送的带掩码文本帧；字符串参数允许构造合法 JSON 但缺失业务字段的负例。
  const body = Buffer.from(typeof value === 'string' ? value : JSON.stringify(value));
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

模拟后端时按真实协议生成“4 字节大端长度 + JSON”。网关测试由此验证的不是简单字符串转发，而是它是否能正确识别 C++ 服务端实际返回的帧边界。

测试端模拟浏览器规则：客户端到服务器的帧必须带随机 mask，每个正文字节与四字节掩码循环异或。短正文与 16 位扩展长度分别覆盖两种 WebSocket 头格式。

这些依赖明确了本片段所在层的边界：JSON、MySQL C API、Redis、RabbitMQ、密码或随机数、断言框架、TCP socket。项目内部头文件提供协议和领域对象，外部库只承担基础能力；业务数据如何流转仍由当前模块决定。

### 片段 2：第 34-58 行

```javascript
  const masked = Buffer.from(body);
  for (let i = 0; i < masked.length; i += 1) masked[i] ^= mask[i % 4];
  return Buffer.concat([header, masked]);
}

function parseServerFrames(buffer, onMessage) {
  // 网关发向浏览器的帧无掩码，按长度增量解析并返回未完成的尾部数据。
  let offset = 0;
  while (buffer.length - offset >= 2) {
    const second = buffer[offset + 1];
    let length = second & 0x7f;
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
    onMessage(buffer.subarray(offset + header, offset + header + length).toString('utf8'));
    offset += header + length;
  }
```

网关发给客户端的帧不带 mask。解析器根据 7/16/64 位长度字段只取完整帧，不足部分原样返回给下一次 data 事件，从而验证响应被 TCP 拆分时仍可恢复。

循环每次只消费已经确认完整的字节或已经成功写出的部分。遇到正文尚未到齐便停在当前偏移，下一次收到数据后继续；发送短写则从剩余位置续发，这正是流式 socket 不能假设“一次调用完成一条消息”的原因。

### 片段 3：第 59-83 行

```javascript
  return buffer.subarray(offset);
}

async function startMockBackend(received) {
  // received 记录网关真正转发到 TCP 的消息，可证明非法 WebSocket 负载在网关层已被拦截。
  const server = net.createServer((socket) => {
    let buffer = Buffer.alloc(0);
    socket.on('data', (chunk) => {
      buffer = Buffer.concat([buffer, chunk]);
      while (buffer.length >= 4) {
        // 模拟真实服务端对 TCP 粘包/半包的解析方式。
        const length = buffer.readUInt32BE(0);
        if (buffer.length < 4 + length) break;
        const message = JSON.parse(buffer.subarray(4, 4 + length).toString('utf8'));
        received.push(message);
        buffer = buffer.subarray(4 + length);

        // 为登录、聊天和心跳返回最小但协议合法的响应，隔离验证桥接方向。
        if (message.msgid === 1) {
          socket.write(tcpFrame({ msgid: 2, errno: 0, id: message.id, name: '测试用户' }));
        } else if (message.msgid === 6) {
          socket.write(tcpFrame({ msgid: 11, message_id: message.message_id, ack_state: 0 }));
        } else if (message.msgid === 12) {
          socket.write(tcpFrame({ msgid: 13, echo_ts: message.ts }));
        }
```

只有已认证连接才能续租自己的在线路由。Redis 条件续期失败可能表示路由已被新会话接管，此时当前连接不应覆盖它；心跳响应回显客户端时间戳，客户端同时获得存活确认和简单 RTT 依据。

读取先确认长度头完整，再判断正文是否已经收齐；不足时保留 Buffer 原状等待下一批字节。只有长度合法且正文完整才前移读指针，这同时处理了半包和一次到达多帧的粘包情况。

`message_id` 贯穿发送、ACK、重试和接收去重：同一业务消息重发时 id 不变，服务端才能识别重复；`ack_state` 则告诉发送者是已接受、已去重还是处理失败，而不是仅凭 TCP 写成功判断业务成功。

### 片段 4：第 84-107 行

```javascript
      }
    });
  });
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  return server;
}

async function connectWebSocket(port) {
  // 测试客户端直接使用 net，避免 WebSocket 库替我们隐藏协议错误。
  const socket = net.createConnection({ host: '127.0.0.1', port });
  const key = crypto.randomBytes(16).toString('base64');
  const messages = [];
  const waiters = [];
  let handshakeBuffer = Buffer.alloc(0);
  let frameBuffer = Buffer.alloc(0);
  let upgraded = false;

  const deliver = (text) => {
    // 本用例请求与响应严格串行，FIFO waiter 足够；没有 waiter 时先缓存防止竞态丢失。
    const value = JSON.parse(text);
    const waiter = waiters.shift();
    if (waiter) waiter.resolve(value);
    else messages.push(value);
  };
```

接收消息可能早于测试开始等待，所以代码先尝试匹配已有等待器，未命中的消息进入积压队列。删除等待器时同时清理超时器；按谓词而不是 FIFO 匹配，允许 ACK、投递和心跳响应交错到达而不串场。

### 片段 5：第 108-145 行

```javascript

  socket.on('data', (chunk) => {
    if (!upgraded) {
      // HTTP 101 与首个 WebSocket 帧可能处于同一 TCP chunk，因此保留 header 之后的字节。
      handshakeBuffer = Buffer.concat([handshakeBuffer, chunk]);
      const marker = handshakeBuffer.indexOf('\r\n\r\n');
      if (marker === -1) return;
      const header = handshakeBuffer.subarray(0, marker).toString('utf8');
      assert.match(header, /^HTTP\/1\.1 101/);
      upgraded = true;
      frameBuffer = handshakeBuffer.subarray(marker + 4);
      handshakeBuffer = Buffer.alloc(0);
    } else {
      frameBuffer = Buffer.concat([frameBuffer, chunk]);
    }
    frameBuffer = parseServerFrames(frameBuffer, deliver);
  });

  await new Promise((resolve, reject) => {
    // 发送标准 Upgrade 头并设置短超时，使握手回归测试失败时快速结束。
    socket.once('connect', () => {
      socket.write([
        'GET /ws HTTP/1.1',
        `Host: 127.0.0.1:${port}`,
        'Upgrade: websocket',
        'Connection: Upgrade',
        `Sec-WebSocket-Key: ${key}`,
        'Sec-WebSocket-Version: 13',
        '\r\n',
      ].join('\r\n'));
    });
    const timer = setTimeout(() => reject(new Error('websocket handshake timeout')), 2_000);
    const check = setInterval(() => {
      if (upgraded) {
        clearInterval(check);
        clearTimeout(timer);
        resolve();
      }
```

断言读取的是本次操作产生的协议响应和可观察状态。因此通过意味着当前场景的业务后果正确；例如收到 ACK 但目标端数量不足，仍会被后续数量断言判为消息丢失。

### 片段 6：第 146-172 行

```javascript
    }, 5);
    socket.once('error', reject);
  });

  return {
    send(value) { socket.write(maskedWebSocketFrame(value)); },
    nextMessage() {
      if (messages.length > 0) return Promise.resolve(messages.shift());
      return new Promise((resolve, reject) => {
        const timer = setTimeout(() => reject(new Error('message timeout')), 2_000);
        waiters.push({
          resolve(value) { clearTimeout(timer); resolve(value); },
        });
      });
    },
    close() { socket.destroy(); },
  };
}

test('HTTP health and WebSocket/TCP full bridge flow', async (t) => {
  // 使用随机端口避免与开发中的 8080/6000 冲突；t.after 统一按反向依赖顺序清理。
  const received = [];
  const backend = await startMockBackend(received);
  const backendPort = backend.address().port;
  const gateway = createGateway({ port: 0, backendHost: '127.0.0.1', backendPort });
  const address = await gateway.listen();
  const client = await connectWebSocket(address.port);
```

接收消息可能早于测试开始等待，所以代码先尝试匹配已有等待器，未命中的消息进入积压队列。删除等待器时同时清理超时器；按谓词而不是 FIFO 匹配，允许 ACK、投递和心跳响应交错到达而不串场。 这里补齐测试客户端的错误和发送接口：socket 错误会拒绝全部等待者，普通 send 负责编码 JSON，sendRawText 专门构造畸形或缺字段负例。

### 片段 7：第 173-202 行

```javascript

  t.after(async () => {
    client.close();
    await gateway.close();
    await new Promise((resolve) => backend.close(resolve));
  });

  // 健康检查只证明网关进程可响应，并返回它当前配置的后端地址。
  const health = await fetch(`http://127.0.0.1:${address.port}/health`);
  assert.equal(health.status, 200);
  assert.equal((await health.json()).status, 'ok');

  // 浏览器 -> WebSocket -> TCP 后端 -> TCP 长度帧 -> WebSocket 的双向登录路径。
  client.send({ msgid: 1, id: 42, password: 'secret12' });
  const loginAck = await client.nextMessage();
  assert.deepEqual(loginAck, { msgid: 2, errno: 0, id: 42, name: '测试用户' });
  assert.equal(received[0].msgid, 1);

  // 聊天请求额外验证 message_id 和 ACK 字段不会在桥接时丢失或改写。
  client.send({
    msgid: 6,
    id: 42,
    toid: 7,
    name: '测试用户',
    msg: '全链路消息',
    message_id: 'e2e-1',
    client_seq: 1,
  });
  assert.deepEqual(await client.nextMessage(), { msgid: 11, message_id: 'e2e-1', ack_state: 0 });
  assert.equal(received[1].msg, '全链路消息');
```

`t.after` 按依赖反向关闭测试客户端、网关和模拟后端；即使中途断言失败，随机监听端口也会释放，不影响下一次 Node 测试。

### 片段 8：第 203-213 行

```javascript

  client.send({ msgid: 12, id: 42, ts: 12345 });
  assert.deepEqual(await client.nextMessage(), { msgid: 13, echo_ts: 12345 });

  // 缺失整数 msgid 的 JSON 应由网关返回 400，并且绝不能进入模拟后端。
  client.send('{"missing":"msgid"}');
  const validationError = await client.nextMessage();
  assert.equal(validationError.msgid, 14);
  assert.equal(validationError.code, 400);
  assert.equal(received.length, 3, 'invalid payload must not reach the TCP backend');
});
```

场景断言同时观察协议响应和对端实际结果：仅有发送成功或 ACK 并不足以证明消息送达。数量、字段、错误码或“观察窗口内没有额外消息”共同限定了本片段要验证的系统性质。

## 面试重点

- TCP 为什么必须自行处理半包和粘包，4 字节长度头如何完成增量解码？

- WebSocket 帧与后端 TCP 长度帧的边界分别在哪里，网关为什么不能承担最终鉴权？

- 最大帧长、半包超时和空闲超时各自防什么问题？

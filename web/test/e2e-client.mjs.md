# `e2e-client.mjs` 讲解

## 作用概览

**端到端测试客户端。** 不用第三方 WebSocket 库，直接构造握手和帧，提供带谓词的等待、批量收集、无消息断言、注册与登录助手。可靠性和负载测试共享它以避免各自实现不同的接收语义。

阅读位置：`web/test/e2e-client.mjs`。下文严格按源码顺序展示，每一行只出现一次；解释只针对紧邻的代码片段。

## 代码片段与详细讲解

### 片段 1：第 1-24 行

```javascript
import assert from 'node:assert/strict';
import crypto from 'node:crypto';
import net from 'node:net';

// 端到端测试专用的最小 WebSocket 客户端。它刻意基于 net 手写握手和帧解析，
// 从而不依赖第三方 WebSocket 库，并能真实覆盖网关的掩码、半包、粘包处理。

// 与生产协议一致的消息编号，供可靠性、压测和故障注入脚本共享。
export const MSG = Object.freeze({
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
```

这些依赖明确了本片段所在层的边界：密码或随机数、断言框架、TCP socket。项目内部头文件提供协议和领域对象，外部库只承担基础能力；业务数据如何流转仍由当前模块决定。

### 片段 2：第 25-46 行

```javascript

function encodeClientFrame(value, opcode = 0x1) {
  // 浏览器到服务端的 WebSocket 帧必须携带随机 4 字节掩码；测试客户端遵守同一规则。
  const body = Buffer.from(typeof value === 'string' ? value : JSON.stringify(value));
  const mask = crypto.randomBytes(4);
  let header;
  // 覆盖 WebSocket 的 7 位、16 位和 64 位长度格式，长消息测试不会被测试工具本身截断。
  if (body.length < 126) {
    header = Buffer.allocUnsafe(6);
    header[1] = 0x80 | body.length;
    mask.copy(header, 2);
  } else if (body.length <= 0xffff) {
    header = Buffer.allocUnsafe(8);
    header[1] = 0x80 | 126;
    header.writeUInt16BE(body.length, 2);
    mask.copy(header, 4);
  } else {
    header = Buffer.allocUnsafe(14);
    header[1] = 0x80 | 127;
    header.writeBigUInt64BE(BigInt(body.length), 2);
    mask.copy(header, 10);
  }
```

这部分属于“端到端测试客户端”的状态衔接代码。它只推进当前事件已经确认的结果；异步响应、未匹配消息或未完成缓冲仍保存在本模块中，后续事件到达后继续处理，不会被当作空结果丢弃。

### 片段 3：第 47-68 行

```javascript
  header[0] = 0x80 | opcode;
  const masked = Buffer.from(body);
  for (let i = 0; i < masked.length; i += 1) masked[i] ^= mask[i % 4];
  return Buffer.concat([header, masked]);
}

function parseFrames(buffer, onMessage) {
  // 服务端帧不掩码。函数按增量方式解析完整帧并返回残留半帧，以适应 TCP 任意切包。
  let offset = 0;
  while (buffer.length - offset >= 2) {
    const opcode = buffer[offset] & 0x0f;
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
```

循环每次只消费已经确认完整的字节或已经成功写出的部分。遇到正文尚未到齐便停在当前偏移，下一次收到数据后继续；发送短写则从剩余位置续发，这正是流式 socket 不能假设“一次调用完成一条消息”的原因。

### 片段 4：第 69-95 行

```javascript
    if (buffer.length - offset < header + length) break;
    if (opcode === 0x1) {
      const message = JSON.parse(buffer.subarray(offset + header, offset + header + length).toString());
      // 收包时间用于端到端延迟统计；设为不可枚举，避免污染深比较和重新序列化结果。
      Object.defineProperty(message, '__receivedAt', { value: Date.now(), enumerable: false });
      onMessage(message);
    }
    offset += header + length;
  }
  return buffer.subarray(offset);
}

export async function createClient(port, options = {}) {
  // host/timeout 可从参数覆盖，便于同一工具连接本地或容器中的一个/多个网关节点。
  const host = options.host ?? process.env.CHAT_E2E_HOST ?? '127.0.0.1';
  const defaultTimeoutMs = options.timeoutMs ?? 15_000;
  const socket = net.createConnection({ host, port });
  socket.setNoDelay(true);
  const key = crypto.randomBytes(16).toString('base64');
  let upgraded = false;
  let closed = false;
  let raw = Buffer.alloc(0);
  let frames = Buffer.alloc(0);
  // messages 缓存先到达但暂时无人等待的消息；waiters 保存按谓词等待的异步断言。
  // 这种设计允许 ACK、业务消息交错到达，而无需假设全局响应顺序。
  const messages = [];
  const waiters = [];
```

测试客户端把未被当前等待条件消费的消息留在 backlog，并给每个 waiter 保存谓词和超时。先检查积压再注册等待，可覆盖“响应早于 waitFor 调用”的竞态；collect 收满指定数量，expectNoMessage 则在观察窗口内专门捕获重复。

本片段读取 `CHAT_E2E_HOST`。未设置时采用紧邻的本机默认值；容器部署则覆盖这些值，因此同一二进制可以作为不同节点运行，无需重新编译。

### 片段 5：第 96-127 行

```javascript

  const removeWaiter = (waiter) => {
    // 超时或“应当无消息”成功后必须移除 waiter，防止后续消息触发已经结束的 Promise。
    const index = waiters.indexOf(waiter);
    if (index >= 0) waiters.splice(index, 1);
  };

  const deliver = (message) => {
    // 将消息交给第一个匹配谓词的等待者；没有匹配者时先缓存，避免先到消息丢失。
    const index = waiters.findIndex((waiter) => waiter.predicate(message));
    if (index >= 0) {
      const waiter = waiters.splice(index, 1)[0];
      clearTimeout(waiter.timer);
      waiter.resolve(message);
    } else {
      messages.push(message);
    }
  };

  socket.on('data', (chunk) => {
    if (!upgraded) {
      // HTTP 握手头也可能分多次到达；以 CRLF CRLF 作为完整头部边界。
      raw = Buffer.concat([raw, chunk]);
      const marker = raw.indexOf('\r\n\r\n');
      if (marker < 0) return;
      assert.match(raw.subarray(0, marker).toString(), /^HTTP\/1\.1 101/);
      upgraded = true;
      // 同一 TCP 包中可能紧跟首个 WebSocket 帧，必须保留头部后的所有字节。
      frames = raw.subarray(marker + 4);
    } else {
      frames = Buffer.concat([frames, chunk]);
    }
```

接收消息可能早于测试开始等待，所以代码先尝试匹配已有等待器，未命中的消息进入积压队列。删除等待器时同时清理超时器；按谓词而不是 FIFO 匹配，允许 ACK、投递和心跳响应交错到达而不串场。

### 片段 6：第 128-158 行

```javascript
    frames = parseFrames(frames, deliver);
  });

  socket.on('close', () => {
    // 连接关闭时一次性拒绝所有等待者，避免测试只能等到各自超时才失败。
    closed = true;
    for (const waiter of waiters.splice(0)) {
      clearTimeout(waiter.timer);
      waiter.reject(new Error(`connection closed on port ${port}`));
    }
  });

  await new Promise((resolve, reject) => {
    // 手写 RFC 6455 Upgrade 请求，以便端到端覆盖网关握手实现。
    socket.once('connect', () => socket.write([
      'GET /ws HTTP/1.1',
      `Host: ${host}:${port}`,
      'Upgrade: websocket',
      'Connection: Upgrade',
      `Sec-WebSocket-Key: ${key}`,
      'Sec-WebSocket-Version: 13',
      '\r\n',
    ].join('\r\n')));
    const deadline = setTimeout(() => reject(new Error(`handshake timeout on ${port}`)), defaultTimeoutMs);
    // data 回调负责设置 upgraded；这里轮询仅用于把事件状态转换为 awaitable Promise。
    const poll = setInterval(() => {
      if (upgraded) {
        clearInterval(poll);
        clearTimeout(deadline);
        resolve();
      }
```

接收消息可能早于测试开始等待，所以代码先尝试匹配已有等待器，未命中的消息进入积压队列。删除等待器时同时清理超时器；按谓词而不是 FIFO 匹配，允许 ACK、投递和心跳响应交错到达而不串场。 退出登录或连接关闭时同时清除心跳和重试定时器，避免旧用户 id 继续续租路由或重发上一会话消息。

### 片段 7：第 159-194 行

```javascript
    }, 10);
    socket.once('error', reject);
  });

  const api = {
    port,
    send(value) {
      // 对象默认 JSON 序列化；sendRawText 可发送故意损坏或字段缺失的原始文本。
      if (closed) throw new Error(`cannot send on closed connection ${port}`);
      socket.write(encodeClientFrame(value));
    },
    sendRawText(value) {
      if (typeof value !== 'string') throw new TypeError('raw WebSocket payload must be a string');
      api.send(value);
    },
    waitFor(predicate, timeoutMs = defaultTimeoutMs) {
      // 先检索历史缓存，解决“消息到达早于 waitFor 注册”的竞态。
      const index = messages.findIndex(predicate);
      if (index >= 0) return Promise.resolve(messages.splice(index, 1)[0]);
      return new Promise((resolve, reject) => {
        const waiter = { predicate, resolve, reject, timer: null };
        waiter.timer = setTimeout(() => {
          removeWaiter(waiter);
          reject(new Error(`message timeout on port ${port} after ${timeoutMs}ms`));
        }, timeoutMs);
        waiters.push(waiter);
      });
    },
    async collect(predicate, count, timeoutMs = defaultTimeoutMs) {
      // 多消息收集共享一个总截止时间，而不是让每一条消息都重新获得完整 timeout。
      const deadline = Date.now() + timeoutMs;
      const collected = [];
      while (collected.length < count) {
        const remaining = Math.max(1, deadline - Date.now());
        collected.push(await api.waitFor(predicate, remaining));
      }
```

接收消息可能早于测试开始等待，所以代码先尝试匹配已有等待器，未命中的消息进入积压队列。删除等待器时同时清理超时器；按谓词而不是 FIFO 匹配，允许 ACK、投递和心跳响应交错到达而不串场。 发送前创建 UUID、递增当前会话序号并立即插入本地消息；随后登记 pending 和发送 payload，用户能即时看到“发送中”，ACK 再更新为已发送。

### 片段 8：第 195-227 行

```javascript
      return collected;
    },
    drain(predicate) {
      // 压测完成后排出迟到的匹配消息，用于检测预期数量之外的重复投递。
      const drained = [];
      for (let i = messages.length - 1; i >= 0; i -= 1) {
        if (predicate(messages[i])) drained.unshift(...messages.splice(i, 1));
      }
      return drained;
    },
    async expectNoMessage(predicate, durationMs = 500) {
      // 负向断言同时检查既有缓存和未来时间窗口，专门验证“去重后不再投递”。
      const existing = messages.find(predicate);
      if (existing) throw new Error(`unexpected duplicate message on port ${port}`);
      await new Promise((resolve, reject) => {
        const waiter = {
          predicate,
          resolve(message) { reject(new Error(`unexpected message: ${JSON.stringify(message)}`)); },
          reject,
          timer: null,
        };
        waiter.timer = setTimeout(() => {
          removeWaiter(waiter);
          resolve();
        }, durationMs);
        waiters.push(waiter);
      });
    },
    close() {
      if (!closed && !socket.destroyed) {
        // 发送标准 WebSocket Close 帧并等待底层写缓冲刷出，确保网关立即关闭后端 TCP。
        socket.end(encodeClientFrame('', 0x8));
      }
```

循环遍历 `messages`，把每个元素独立转换、投递或校验。结果按遍历顺序追加，某个元素失败时由本片段的状态变量或断言记录，不能用一次总体成功掩盖单项失败。

### 片段 9：第 228-253 行

```javascript
    },
  };
  return api;
}

export async function register(client, name, password, timeoutMs = 30_000) {
  // 注册辅助函数直接断言成功并返回新 ID，让测试场景把注意力放在后续可靠性行为上。
  client.send({ msgid: MSG.REGISTER, name, password });
  const response = await client.waitFor((message) => message.msgid === MSG.REGISTER_ACK, timeoutMs);
  assert.equal(response.errno, 0, `register failed for ${name}: ${response.errmsg ?? 'unknown'}`);
  return response.id;
}

export async function login(client, id, password, timeoutMs = 30_000) {
  // 登录可能被场景故意期待失败（例如并发登录），因此这里返回响应而不强制 errno=0。
  client.send({ msgid: MSG.LOGIN, id, password });
  return client.waitFor((message) => message.msgid === MSG.LOGIN_ACK, timeoutMs);
}

export function parseEmbedded(values = []) {
  // 与后端当前登录响应兼容：关系和离线消息可能以 JSON 字符串形式嵌在数组中。
  return values.map((value) => (typeof value === 'string' ? JSON.parse(value) : value));
}

// 统一的可 await 延迟，用于等待退出路由传播或人工故障注入窗口。
export const delay = (milliseconds) => new Promise((resolve) => setTimeout(resolve, milliseconds));
```

场景断言同时观察协议响应和对端实际结果：仅有发送成功或 ACK 并不足以证明消息送达。数量、字段、错误码或“观察窗口内没有额外消息”共同限定了本片段要验证的系统性质。

## 面试重点

- 测试准备了什么外部状态或模拟组件，实际动作经过哪些模块？

- 每个断言证明的是返回值正确，还是“不丢、不重、不乱序、不可冒用”等系统性质？

- 如何避免测试自身的等待竞态和上轮残留状态造成假失败？

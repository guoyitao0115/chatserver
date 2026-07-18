import assert from 'node:assert/strict';
import crypto from 'node:crypto';
import net from 'node:net';

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

function encodeClientFrame(value, opcode = 0x1) {
  const body = Buffer.from(typeof value === 'string' ? value : JSON.stringify(value));
  const mask = crypto.randomBytes(4);
  let header;
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
  header[0] = 0x80 | opcode;
  const masked = Buffer.from(body);
  for (let i = 0; i < masked.length; i += 1) masked[i] ^= mask[i % 4];
  return Buffer.concat([header, masked]);
}

function parseFrames(buffer, onMessage) {
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
    if (buffer.length - offset < header + length) break;
    if (opcode === 0x1) {
      const message = JSON.parse(buffer.subarray(offset + header, offset + header + length).toString());
      Object.defineProperty(message, '__receivedAt', { value: Date.now(), enumerable: false });
      onMessage(message);
    }
    offset += header + length;
  }
  return buffer.subarray(offset);
}

export async function createClient(port, options = {}) {
  const host = options.host ?? process.env.CHAT_E2E_HOST ?? '127.0.0.1';
  const defaultTimeoutMs = options.timeoutMs ?? 15_000;
  const socket = net.createConnection({ host, port });
  socket.setNoDelay(true);
  const key = crypto.randomBytes(16).toString('base64');
  let upgraded = false;
  let closed = false;
  let raw = Buffer.alloc(0);
  let frames = Buffer.alloc(0);
  const messages = [];
  const waiters = [];

  const removeWaiter = (waiter) => {
    const index = waiters.indexOf(waiter);
    if (index >= 0) waiters.splice(index, 1);
  };

  const deliver = (message) => {
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
      raw = Buffer.concat([raw, chunk]);
      const marker = raw.indexOf('\r\n\r\n');
      if (marker < 0) return;
      assert.match(raw.subarray(0, marker).toString(), /^HTTP\/1\.1 101/);
      upgraded = true;
      frames = raw.subarray(marker + 4);
    } else {
      frames = Buffer.concat([frames, chunk]);
    }
    frames = parseFrames(frames, deliver);
  });

  socket.on('close', () => {
    closed = true;
    for (const waiter of waiters.splice(0)) {
      clearTimeout(waiter.timer);
      waiter.reject(new Error(`connection closed on port ${port}`));
    }
  });

  await new Promise((resolve, reject) => {
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
    const poll = setInterval(() => {
      if (upgraded) {
        clearInterval(poll);
        clearTimeout(deadline);
        resolve();
      }
    }, 10);
    socket.once('error', reject);
  });

  const api = {
    port,
    send(value) {
      if (closed) throw new Error(`cannot send on closed connection ${port}`);
      socket.write(encodeClientFrame(value));
    },
    sendRawText(value) {
      if (typeof value !== 'string') throw new TypeError('raw WebSocket payload must be a string');
      api.send(value);
    },
    waitFor(predicate, timeoutMs = defaultTimeoutMs) {
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
      const deadline = Date.now() + timeoutMs;
      const collected = [];
      while (collected.length < count) {
        const remaining = Math.max(1, deadline - Date.now());
        collected.push(await api.waitFor(predicate, remaining));
      }
      return collected;
    },
    drain(predicate) {
      const drained = [];
      for (let i = messages.length - 1; i >= 0; i -= 1) {
        if (predicate(messages[i])) drained.unshift(...messages.splice(i, 1));
      }
      return drained;
    },
    async expectNoMessage(predicate, durationMs = 500) {
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
    },
  };
  return api;
}

export async function register(client, name, password, timeoutMs = 30_000) {
  client.send({ msgid: MSG.REGISTER, name, password });
  const response = await client.waitFor((message) => message.msgid === MSG.REGISTER_ACK, timeoutMs);
  assert.equal(response.errno, 0, `register failed for ${name}: ${response.errmsg ?? 'unknown'}`);
  return response.id;
}

export async function login(client, id, password, timeoutMs = 30_000) {
  client.send({ msgid: MSG.LOGIN, id, password });
  return client.waitFor((message) => message.msgid === MSG.LOGIN_ACK, timeoutMs);
}

export function parseEmbedded(values = []) {
  return values.map((value) => (typeof value === 'string' ? JSON.parse(value) : value));
}

export const delay = (milliseconds) => new Promise((resolve) => setTimeout(resolve, milliseconds));

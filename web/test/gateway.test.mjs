import assert from 'node:assert/strict';
import crypto from 'node:crypto';
import net from 'node:net';
import test from 'node:test';

import { createGateway } from '../gateway.mjs';

function tcpFrame(value) {
  const body = Buffer.from(JSON.stringify(value));
  const frame = Buffer.allocUnsafe(4 + body.length);
  frame.writeUInt32BE(body.length, 0);
  body.copy(frame, 4);
  return frame;
}

function maskedWebSocketFrame(value) {
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
  const masked = Buffer.from(body);
  for (let i = 0; i < masked.length; i += 1) masked[i] ^= mask[i % 4];
  return Buffer.concat([header, masked]);
}

function parseServerFrames(buffer, onMessage) {
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
  return buffer.subarray(offset);
}

async function startMockBackend(received) {
  const server = net.createServer((socket) => {
    let buffer = Buffer.alloc(0);
    socket.on('data', (chunk) => {
      buffer = Buffer.concat([buffer, chunk]);
      while (buffer.length >= 4) {
        const length = buffer.readUInt32BE(0);
        if (buffer.length < 4 + length) break;
        const message = JSON.parse(buffer.subarray(4, 4 + length).toString('utf8'));
        received.push(message);
        buffer = buffer.subarray(4 + length);

        if (message.msgid === 1) {
          socket.write(tcpFrame({ msgid: 2, errno: 0, id: message.id, name: '测试用户' }));
        } else if (message.msgid === 6) {
          socket.write(tcpFrame({ msgid: 11, message_id: message.message_id, ack_state: 0 }));
        } else if (message.msgid === 12) {
          socket.write(tcpFrame({ msgid: 13, echo_ts: message.ts }));
        }
      }
    });
  });
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  return server;
}

async function connectWebSocket(port) {
  const socket = net.createConnection({ host: '127.0.0.1', port });
  const key = crypto.randomBytes(16).toString('base64');
  const messages = [];
  const waiters = [];
  let handshakeBuffer = Buffer.alloc(0);
  let frameBuffer = Buffer.alloc(0);
  let upgraded = false;

  const deliver = (text) => {
    const value = JSON.parse(text);
    const waiter = waiters.shift();
    if (waiter) waiter.resolve(value);
    else messages.push(value);
  };

  socket.on('data', (chunk) => {
    if (!upgraded) {
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
  const received = [];
  const backend = await startMockBackend(received);
  const backendPort = backend.address().port;
  const gateway = createGateway({ port: 0, backendHost: '127.0.0.1', backendPort });
  const address = await gateway.listen();
  const client = await connectWebSocket(address.port);

  t.after(async () => {
    client.close();
    await gateway.close();
    await new Promise((resolve) => backend.close(resolve));
  });

  const health = await fetch(`http://127.0.0.1:${address.port}/health`);
  assert.equal(health.status, 200);
  assert.equal((await health.json()).status, 'ok');

  client.send({ msgid: 1, id: 42, password: 'secret12' });
  const loginAck = await client.nextMessage();
  assert.deepEqual(loginAck, { msgid: 2, errno: 0, id: 42, name: '测试用户' });
  assert.equal(received[0].msgid, 1);

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

  client.send({ msgid: 12, id: 42, ts: 12345 });
  assert.deepEqual(await client.nextMessage(), { msgid: 13, echo_ts: 12345 });

  client.send('{"missing":"msgid"}');
  const validationError = await client.nextMessage();
  assert.equal(validationError.msgid, 14);
  assert.equal(validationError.code, 400);
  assert.equal(received.length, 3, 'invalid payload must not reach the TCP backend');
});

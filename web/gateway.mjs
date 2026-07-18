import crypto from 'node:crypto';
import fs from 'node:fs/promises';
import http from 'node:http';
import net from 'node:net';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const MAX_PAYLOAD = 4 * 1024 * 1024;
const DEFAULT_PUBLIC_DIR = fileURLToPath(new URL('./public', import.meta.url));

const STATIC_FILES = new Map([
  ['/', ['index.html', 'text/html; charset=utf-8']],
  ['/index.html', ['index.html', 'text/html; charset=utf-8']],
  ['/app.js', ['app.js', 'text/javascript; charset=utf-8']],
  ['/styles.css', ['styles.css', 'text/css; charset=utf-8']],
]);

function encodeTcpFrame(payload) {
  const body = Buffer.from(payload, 'utf8');
  if (body.length === 0 || body.length > MAX_PAYLOAD) {
    throw new RangeError('payload size is invalid');
  }
  const frame = Buffer.allocUnsafe(4 + body.length);
  frame.writeUInt32BE(body.length, 0);
  body.copy(frame, 4);
  return frame;
}

function encodeWebSocketFrame(payload, opcode = 0x1) {
  const body = Buffer.isBuffer(payload) ? payload : Buffer.from(payload, 'utf8');
  let header;
  if (body.length < 126) {
    header = Buffer.from([0x80 | opcode, body.length]);
  } else if (body.length <= 0xffff) {
    header = Buffer.allocUnsafe(4);
    header[0] = 0x80 | opcode;
    header[1] = 126;
    header.writeUInt16BE(body.length, 2);
  } else {
    header = Buffer.allocUnsafe(10);
    header[0] = 0x80 | opcode;
    header[1] = 127;
    header.writeBigUInt64BE(BigInt(body.length), 2);
  }
  return Buffer.concat([header, body]);
}

function parseWebSocketFrames(buffer, onFrame) {
  let offset = 0;
  while (buffer.length - offset >= 2) {
    const first = buffer[offset];
    const second = buffer[offset + 1];
    const fin = (first & 0x80) !== 0;
    const opcode = first & 0x0f;
    const masked = (second & 0x80) !== 0;
    let length = second & 0x7f;
    let headerLength = 2;

    if (!fin || !masked) throw new Error('fragmented or unmasked client frame');
    if (length === 126) {
      if (buffer.length - offset < 4) break;
      length = buffer.readUInt16BE(offset + 2);
      headerLength = 4;
    } else if (length === 127) {
      if (buffer.length - offset < 10) break;
      const longLength = buffer.readBigUInt64BE(offset + 2);
      if (longLength > BigInt(MAX_PAYLOAD)) throw new Error('websocket frame too large');
      length = Number(longLength);
      headerLength = 10;
    }
    if (length > MAX_PAYLOAD) throw new Error('websocket frame too large');

    const frameLength = headerLength + 4 + length;
    if (buffer.length - offset < frameLength) break;

    const mask = buffer.subarray(offset + headerLength, offset + headerLength + 4);
    const payload = Buffer.from(
      buffer.subarray(offset + headerLength + 4, offset + frameLength),
    );
    for (let i = 0; i < payload.length; i += 1) payload[i] ^= mask[i % 4];
    onFrame(opcode, payload);
    offset += frameLength;
  }
  return buffer.subarray(offset);
}

function isValidBusinessPayload(text) {
  try {
    const value = JSON.parse(text);
    return value !== null && typeof value === 'object'
      && Number.isInteger(value.msgid);
  } catch {
    return false;
  }
}

export function createGateway(options = {}) {
  const port = Number(options.port ?? process.env.CHAT_WEB_PORT ?? 8080);
  const backendHost = options.backendHost ?? process.env.CHAT_BACKEND_HOST ?? '127.0.0.1';
  const backendPort = Number(options.backendPort ?? process.env.CHAT_BACKEND_PORT ?? 6000);
  const publicDir = options.publicDir ?? DEFAULT_PUBLIC_DIR;
  const activeSockets = new Set();

  const server = http.createServer(async (request, response) => {
    const pathname = new URL(request.url ?? '/', 'http://localhost').pathname;
    if (pathname === '/health') {
      response.writeHead(200, { 'content-type': 'application/json; charset=utf-8' });
      response.end(JSON.stringify({ status: 'ok', backend: `${backendHost}:${backendPort}` }));
      return;
    }
    if (pathname === '/favicon.ico') {
      response.writeHead(204);
      response.end();
      return;
    }

    const file = STATIC_FILES.get(pathname);
    if (!file) {
      response.writeHead(404, { 'content-type': 'text/plain; charset=utf-8' });
      response.end('Not found');
      return;
    }

    try {
      const body = await fs.readFile(path.join(publicDir, file[0]));
      response.writeHead(200, {
        'content-type': file[1],
        'cache-control': 'no-store',
        'x-content-type-options': 'nosniff',
      });
      response.end(body);
    } catch {
      response.writeHead(500, { 'content-type': 'text/plain; charset=utf-8' });
      response.end('Static file unavailable');
    }
  });

  server.on('upgrade', (request, browserSocket, head) => {
    const pathname = new URL(request.url ?? '/', 'http://localhost').pathname;
    const key = request.headers['sec-websocket-key'];
    if (pathname !== '/ws' || typeof key !== 'string') {
      browserSocket.end('HTTP/1.1 400 Bad Request\r\n\r\n');
      return;
    }

    const backendSocket = net.createConnection({ host: backendHost, port: backendPort });
    activeSockets.add(browserSocket);
    activeSockets.add(backendSocket);
    let upgraded = false;
    let browserBuffer = Buffer.alloc(0);
    let backendBuffer = Buffer.alloc(0);

    const closeBoth = () => {
      activeSockets.delete(browserSocket);
      activeSockets.delete(backendSocket);
      if (!browserSocket.destroyed) browserSocket.destroy();
      if (!backendSocket.destroyed) backendSocket.destroy();
    };

    backendSocket.setTimeout(50_000, () => backendSocket.destroy(new Error('backend idle timeout')));
    backendSocket.once('connect', () => {
      upgraded = true;
      const accept = crypto.createHash('sha1')
        .update(`${key}258EAFA5-E914-47DA-95CA-C5AB0DC85B11`)
        .digest('base64');
      browserSocket.write([
        'HTTP/1.1 101 Switching Protocols',
        'Upgrade: websocket',
        'Connection: Upgrade',
        `Sec-WebSocket-Accept: ${accept}`,
        '\r\n',
      ].join('\r\n'));
      if (head.length > 0) browserSocket.emit('data', head);
    });

    backendSocket.on('data', (chunk) => {
      backendSocket.setTimeout(50_000);
      backendBuffer = Buffer.concat([backendBuffer, chunk]);
      while (backendBuffer.length >= 4) {
        const length = backendBuffer.readUInt32BE(0);
        if (length === 0 || length > MAX_PAYLOAD) {
          closeBoth();
          return;
        }
        if (backendBuffer.length < 4 + length) break;
        const payload = backendBuffer.subarray(4, 4 + length);
        browserSocket.write(encodeWebSocketFrame(payload));
        backendBuffer = backendBuffer.subarray(4 + length);
      }
    });

    backendSocket.on('error', (error) => {
      if (!upgraded) {
        browserSocket.end('HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\n\r\n');
      } else if (!browserSocket.destroyed) {
        browserSocket.write(encodeWebSocketFrame(JSON.stringify({
          msgid: 14,
          code: 502,
          message: `聊天服务不可用：${error.message}`,
        })));
      }
      closeBoth();
    });
    backendSocket.on('close', () => closeBoth());

    browserSocket.on('data', (chunk) => {
      if (!upgraded) return;
      try {
        browserBuffer = Buffer.concat([browserBuffer, chunk]);
        browserBuffer = parseWebSocketFrames(browserBuffer, (opcode, payload) => {
          if (opcode === 0x8) {
            closeBoth();
          } else if (opcode === 0x9) {
            browserSocket.write(encodeWebSocketFrame(payload, 0xA));
          } else if (opcode === 0x1) {
            const text = payload.toString('utf8');
            if (!isValidBusinessPayload(text)) {
              browserSocket.write(encodeWebSocketFrame(JSON.stringify({
                msgid: 14,
                code: 400,
                message: '消息必须是包含整数 msgid 的 JSON 对象',
              })));
              return;
            }
            backendSocket.setTimeout(50_000);
            backendSocket.write(encodeTcpFrame(text));
          }
        });
      } catch {
        closeBoth();
      }
    });
    browserSocket.on('error', closeBoth);
    browserSocket.on('close', () => closeBoth());
  });

  return {
    server,
    listen() {
      return new Promise((resolve, reject) => {
        server.once('error', reject);
        server.listen(port, () => {
          server.off('error', reject);
          resolve(server.address());
        });
      });
    },
    close() {
      return new Promise((resolve, reject) => {
        for (const socket of activeSockets) socket.destroy();
        activeSockets.clear();
        server.close((error) => (error ? reject(error) : resolve()));
      });
    },
  };
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const gateway = createGateway();
  const address = await gateway.listen();
  console.log(`Chat Web: http://127.0.0.1:${address.port}`);
}

export const protocolInternals = { encodeTcpFrame, encodeWebSocketFrame, parseWebSocketFrames };

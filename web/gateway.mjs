import crypto from 'node:crypto';
import fs from 'node:fs/promises';
import http from 'node:http';
import net from 'node:net';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

// 浏览器不能直接连接后端的裸 TCP 服务。本文件同时承担三个职责：
// 1. 提供静态页面和健康检查；
// 2. 完成 WebSocket 握手及控制帧处理；
// 3. 在“WebSocket 帧”和后端“4 字节大端长度 + JSON 正文”之间双向转换。
// 网关只校验消息的基本形状，不理解登录、聊天等业务状态，避免和 C++ 服务端重复实现业务逻辑。

// 与 C++ FrameCodec 保持一致的单条业务消息上限。限制长度既能避免异常内存分配，
// 也能阻止客户端通过声明超大帧实施简单的内存消耗攻击。
const MAX_PAYLOAD = 4 * 1024 * 1024;
const DEFAULT_PUBLIC_DIR = fileURLToPath(new URL('./public', import.meta.url));

// 使用白名单映射静态资源，而不是把 URL 直接拼成文件路径。
// 因此请求无法通过 ../ 等路径穿越语法读取 publicDir 之外的文件。
const STATIC_FILES = new Map([
  ['/', ['index.html', 'text/html; charset=utf-8']],
  ['/index.html', ['index.html', 'text/html; charset=utf-8']],
  ['/app.js', ['app.js', 'text/javascript; charset=utf-8']],
  ['/message-dedup.js', ['message-dedup.js', 'text/javascript; charset=utf-8']],
  ['/styles.css', ['styles.css', 'text/css; charset=utf-8']],
]);

function encodeTcpFrame(payload) {
  // TCP 是字节流，没有天然消息边界；前置固定 4 字节长度可让后端正确处理粘包和半包。
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
  // 服务端发送给浏览器的 WebSocket 帧按协议不需要掩码。FIN 固定为 1，表示本实现不拆分消息。
  const body = Buffer.isBuffer(payload) ? payload : Buffer.from(payload, 'utf8');
  let header;
  // WebSocket 根据正文长度使用 7 位、16 位或 64 位三种长度编码。
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
  // 每次 data 事件可能只收到半个帧，也可能一次收到多个帧。
  // offset 之前是本轮已消费数据；函数末尾返回未消费尾部，供下一次增量解析。
  let offset = 0;
  while (buffer.length - offset >= 2) {
    const first = buffer[offset];
    const second = buffer[offset + 1];
    const fin = (first & 0x80) !== 0;
    const opcode = first & 0x0f;
    const masked = (second & 0x80) !== 0;
    let length = second & 0x7f;
    let headerLength = 2;

    // RFC 6455 要求浏览器到服务端的帧必须掩码。本项目不支持分片帧，尽早拒绝可简化状态机。
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

    // 客户端帧在长度字段后还有 4 字节掩码。数据不完整时保留原缓冲区，不提前消费。
    const frameLength = headerLength + 4 + length;
    if (buffer.length - offset < frameLength) break;

    const mask = buffer.subarray(offset + headerLength, offset + headerLength + 4);
    const payload = Buffer.from(
      buffer.subarray(offset + headerLength + 4, offset + frameLength),
    );
    // 掩码按 4 字节循环异或；复制 payload 后再修改，避免污染仍被 buffer 引用的内存。
    for (let i = 0; i < payload.length; i += 1) payload[i] ^= mask[i % 4];
    onFrame(opcode, payload);
    offset += frameLength;
  }
  return buffer.subarray(offset);
}

function isValidBusinessPayload(text) {
  // 网关仅保证消息是 JSON 对象且有整数 msgid。字段权限、身份绑定和业务取值由后端校验，
  // 这样浏览器即使绕过页面直接发包，也不能依赖前端校验获得额外权限。
  try {
    const value = JSON.parse(text);
    return value !== null && typeof value === 'object'
      && Number.isInteger(value.msgid);
  } catch {
    return false;
  }
}

export function createGateway(options = {}) {
  // options 便于单元测试注入随机端口和模拟后端；环境变量用于容器部署；最后才使用本地默认值。
  const port = Number(options.port ?? process.env.CHAT_WEB_PORT ?? 8080);
  const backendHost = options.backendHost ?? process.env.CHAT_BACKEND_HOST ?? '127.0.0.1';
  const backendPort = Number(options.backendPort ?? process.env.CHAT_BACKEND_PORT ?? 6000);
  const publicDir = options.publicDir ?? DEFAULT_PUBLIC_DIR;
  // 保存升级连接的两端 socket，使 close() 能主动清理测试或进程退出前仍存活的长连接。
  const activeSockets = new Set();

  const server = http.createServer(async (request, response) => {
    // URL 解析只使用 pathname，查询参数不会影响静态文件白名单匹配。
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
        // 开发和演示环境不缓存，保证刷新后立即看到最新资源。
        'cache-control': 'no-store',
        // 禁止浏览器 MIME 嗅探，降低内容类型混淆风险。
        'x-content-type-options': 'nosniff',
      });
      response.end(body);
    } catch {
      response.writeHead(500, { 'content-type': 'text/plain; charset=utf-8' });
      response.end('Static file unavailable');
    }
  });

  server.on('upgrade', (request, browserSocket, head) => {
    // 只接受 /ws，且握手必须包含浏览器生成的 Sec-WebSocket-Key。
    const pathname = new URL(request.url ?? '/', 'http://localhost').pathname;
    const key = request.headers['sec-websocket-key'];
    if (pathname !== '/ws' || typeof key !== 'string') {
      browserSocket.end('HTTP/1.1 400 Bad Request\r\n\r\n');
      return;
    }

    // 每条浏览器 WebSocket 连接对应一条独立后端 TCP 连接。
    // 这种一一映射使后端原有“连接即会话”模型无需改造。
    const backendSocket = net.createConnection({ host: backendHost, port: backendPort });
    activeSockets.add(browserSocket);
    activeSockets.add(backendSocket);
    let upgraded = false;
    // 两个方向分别维护残留缓冲区，解决任意 TCP 分片、粘包问题。
    let browserBuffer = Buffer.alloc(0);
    let backendBuffer = Buffer.alloc(0);

    const closeBoth = () => {
      // 任意一端失败都关闭整条桥接链路，防止另一端成为泄漏的半开连接。
      // destroy() 和 Set.delete() 可重复调用，因此多个 error/close 事件同时到达也安全。
      activeSockets.delete(browserSocket);
      activeSockets.delete(backendSocket);
      if (!browserSocket.destroyed) browserSocket.destroy();
      if (!backendSocket.destroyed) backendSocket.destroy();
    };

    // 空闲超时比前端 20 秒心跳周期长，正常登录会话会被心跳持续刷新。
    backendSocket.setTimeout(50_000, () => backendSocket.destroy(new Error('backend idle timeout')));
    backendSocket.once('connect', () => {
      upgraded = true;
      // WebSocket 握手规定：固定 GUID 与客户端 key 拼接后计算 SHA-1，再做 Base64。
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
      // Node upgrade 事件可能把 HTTP 头之后已到达的数据放在 head 中；重新投递，避免首帧丢失。
      if (head.length > 0) browserSocket.emit('data', head);
    });

    backendSocket.on('data', (chunk) => {
      backendSocket.setTimeout(50_000);
      backendBuffer = Buffer.concat([backendBuffer, chunk]);
      // 循环提取完整后端帧：不足 4 字节或正文不完整时等待下一次 data。
      while (backendBuffer.length >= 4) {
        const length = backendBuffer.readUInt32BE(0);
        if (length === 0 || length > MAX_PAYLOAD) {
          closeBoth();
          return;
        }
        if (backendBuffer.length < 4 + length) break;
        const payload = backendBuffer.subarray(4, 4 + length);
        // 后端正文已经是 UTF-8 JSON 字节，直接封装成 WebSocket 文本帧，无需再次序列化。
        browserSocket.write(encodeWebSocketFrame(payload));
        backendBuffer = backendBuffer.subarray(4 + length);
      }
    });

    backendSocket.on('error', (error) => {
      // 握手前连接失败用标准 HTTP 502；握手后只能通过 WebSocket 业务错误帧通知浏览器。
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
          // 0x8=关闭，0x9=Ping，0xA=Pong，0x1=文本。二进制及未知控制帧不承载业务消息。
          if (opcode === 0x8) {
            closeBoth();
          } else if (opcode === 0x9) {
            // Pong 必须原样携带 Ping 的负载。
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
            // 合法 JSON 被包装为后端长度帧；不修改消息内容，message_id/client_seq 等可靠性字段原样透传。
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
        // 先销毁长连接，否则 http.Server.close() 会一直等待它们自然结束。
        for (const socket of activeSockets) socket.destroy();
        activeSockets.clear();
        server.close((error) => (error ? reject(error) : resolve()));
      });
    },
  };
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  // 只有直接执行 node gateway.mjs 时才启动监听；被测试 import 时不会产生端口副作用。
  const gateway = createGateway();
  const address = await gateway.listen();
  console.log(`Chat Web: http://127.0.0.1:${address.port}`);
}

// 暴露纯协议函数供单元测试覆盖边界长度、粘包、半包和掩码解析，不属于页面公开 API。
export const protocolInternals = { encodeTcpFrame, encodeWebSocketFrame, parseWebSocketFrames };

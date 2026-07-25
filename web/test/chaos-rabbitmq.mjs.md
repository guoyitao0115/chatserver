# `chaos-rabbitmq.mjs` 讲解

## 作用概览

**RabbitMQ 故障测试。** 让跨节点在线投递在消息队列不可用时退化到离线存储，再通过接收者重新登录验证消息仍可恢复。

阅读位置：`web/test/chaos-rabbitmq.mjs`。下文严格按源码顺序展示，每一行只出现一次；解释只针对紧邻的代码片段。

## 代码片段与详细讲解

### 片段 1：第 1-31 行

```javascript
import assert from 'node:assert/strict';
import crypto from 'node:crypto';

import { MSG, createClient, delay, login, parseEmbedded, register } from './e2e-client.mjs';

// RabbitMQ 故障测试覆盖跨节点消息总线不可用时的降级行为。
// 外部脚本读取 READY_FOR_RABBITMQ_STOP 日志后停止 RabbitMQ 容器。
const port1 = Number(process.env.CHAT_E2E_PORT_1 ?? 8080);
const port2 = Number(process.env.CHAT_E2E_PORT_2 ?? 8081);
const timeoutMs = Number(process.env.CHAT_CHAOS_TIMEOUT_MS ?? 30_000);
const outageWaitMs = Number(process.env.CHAT_CHAOS_OUTAGE_WAIT_MS ?? 7_000);
const suffix = `${Date.now()}-${crypto.randomInt(1000, 9999)}`;
const password = 'chaos-demo-123';
const clients = new Set();

// 所有连接都登记到 clients，测试失败时也能统一关闭，避免在线状态污染后续用例。
async function trackedClient(port) {
  const client = await createClient(port, { timeoutMs });
  clients.add(client);
  return client;
}

try {
  // sender/receiver 分别连接不同 Web 端口，用于尽量触发跨节点路径。
  // 如果 RabbitMQ 断开，系统应尝试本地可达投递或退化为离线消息兜底。
  const sender = await trackedClient(port1);
  let receiver = await trackedClient(port2);
  const senderId = await register(sender, `故障发送-${suffix}`, password, timeoutMs);
  const receiverId = await register(receiver, `故障接收-${suffix}`, password, timeoutMs);
  assert.equal((await login(sender, senderId, password, timeoutMs)).errno, 0);
  assert.equal((await login(receiver, receiverId, password, timeoutMs)).errno, 0);
```

新建连接后立即加入统一集合，保证测试在任意断言处抛错时仍能在 finally 中找到并关闭它；这避免失败用例留下在线用户影响下一轮重复登录判断。

这些依赖明确了本片段所在层的边界：RabbitMQ、密码或随机数、断言框架。项目内部头文件提供协议和领域对象，外部库只承担基础能力；业务数据如何流转仍由当前模块决定。

本片段读取 `CHAT_E2E_PORT_1`、`CHAT_E2E_PORT_2`、`CHAT_CHAOS_TIMEOUT_MS`、`CHAT_CHAOS_OUTAGE_WAIT_MS`。未设置时采用紧邻的本机默认值；容器部署则覆盖这些值，因此同一二进制可以作为不同节点运行，无需重新编译。

### 片段 2：第 32-60 行

```javascript

  // 给编排脚本留出停止 RabbitMQ 的时间，避免消息在故障前就被正常投递。
  console.log(`READY_FOR_RABBITMQ_STOP wait_ms=${outageWaitMs}`);
  await delay(outageWaitMs);

  // 使用唯一 message_id 便于判断这条故障窗口内的消息最终是在线到达还是离线恢复。
  const messageId = `rabbit-outage-${suffix}`;
  sender.send({
    msgid: MSG.ONE_CHAT,
    id: senderId,
    toid: receiverId,
    name: `故障发送-${suffix}`,
    msg: 'RabbitMQ 中断期间的消息',
    time: new Date().toISOString(),
    sent_at_ms: Date.now(),
    client_seq: 1,
    message_id: messageId,
  });
  const ack = await sender.waitFor((message) => message.msgid === MSG.ACK && message.message_id === messageId, timeoutMs);

  // 先短暂等待在线投递。如果 RabbitMQ 中断导致跨节点通知失败，接收者可能收不到，
  // 此时再走离线登录路径确认消息没有丢。
  let onlineDelivery = null;
  try {
    onlineDelivery = await receiver.waitFor(
      (message) => message.msgid === MSG.ONE_CHAT && message.message_id === messageId,
      3_000,
    );
  }
```

READY 日志是与外部编排脚本的同步点：打印后等待容器被停，再发送唯一消息。测试先给在线投递 3 秒机会；若超时，只吞掉预期的 message timeout，其他错误仍抛出，然后重登接收者从 offlinemsg 查同一 id。

### 片段 3：第 61-90 行

```javascript
  catch (error) {
    if (!String(error.message).includes('message timeout')) throw error;
  }

  // 在线未收到时，主动登出再登录，检查消息是否被保存到了离线消息中。
  // 这条断言对应“总线不可用时不丢消息”的业务要求。
  let offlineDelivery = null;
  if (!onlineDelivery) {
    receiver.send({ msgid: MSG.LOGOUT, id: receiverId });
    await delay(250);
    receiver.close();
    clients.delete(receiver);
    receiver = await trackedClient(port2);
    const relogin = await login(receiver, receiverId, password, timeoutMs);
    assert.equal(relogin.errno, 0);
    offlineDelivery = parseEmbedded(relogin.offlinemsg).find((message) => message.message_id === messageId) ?? null;
  }

  // 允许在线成功或离线兜底成功，但不允许两条路径都失败。
  assert.ok(onlineDelivery || offlineDelivery, 'message was neither delivered online nor stored offline');
  console.log(JSON.stringify({
    status: 'passed',
    ackState: ack.ack_state,
    delivery: onlineDelivery ? 'online' : 'offline-fallback',
    messageId,
  }, null, 2));
} finally {
  // 无论 RabbitMQ 是否恢复，都先清掉测试连接，避免服务端在线表残留。
  for (const client of clients) client.close();
}
```

`message_id` 贯穿发送、ACK、重试和接收去重：同一业务消息重发时 id 不变，服务端才能识别重复；`ack_state` 则告诉发送者是已接受、已去重还是处理失败，而不是仅凭 TCP 写成功判断业务成功。

`finally` 会遍历本轮登记的客户端并关闭连接。即使中途断言抛错，服务端也能触发断线清理，避免残留在线路由让下一轮重复登录或离线场景得到假结果。

循环每次只消费已经确认完整的字节或已经成功写出的部分。遇到正文尚未到齐便停在当前偏移，下一次收到数据后继续；发送短写则从剩余位置续发，这正是流式 socket 不能假设“一次调用完成一条消息”的原因。

## 面试重点

- 能否沿着一条单聊消息说明本地直发、跨节点路由、离线落库、ACK 与重试之间的成功语义？

- Redis 或 RabbitMQ 故障时系统如何降级，哪些保证仍成立，哪些保证会变弱？

- 为什么“至少一次发送 + message_id 幂等”不等于严格 Exactly Once？

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

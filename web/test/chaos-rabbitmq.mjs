import assert from 'node:assert/strict';
import crypto from 'node:crypto';

import { MSG, createClient, delay, login, parseEmbedded, register } from './e2e-client.mjs';

const port1 = Number(process.env.CHAT_E2E_PORT_1 ?? 8080);
const port2 = Number(process.env.CHAT_E2E_PORT_2 ?? 8081);
const timeoutMs = Number(process.env.CHAT_CHAOS_TIMEOUT_MS ?? 30_000);
const outageWaitMs = Number(process.env.CHAT_CHAOS_OUTAGE_WAIT_MS ?? 7_000);
const suffix = `${Date.now()}-${crypto.randomInt(1000, 9999)}`;
const password = 'chaos-demo-123';
const clients = new Set();

async function trackedClient(port) {
  const client = await createClient(port, { timeoutMs });
  clients.add(client);
  return client;
}

try {
  const sender = await trackedClient(port1);
  let receiver = await trackedClient(port2);
  const senderId = await register(sender, `故障发送-${suffix}`, password, timeoutMs);
  const receiverId = await register(receiver, `故障接收-${suffix}`, password, timeoutMs);
  assert.equal((await login(sender, senderId, password, timeoutMs)).errno, 0);
  assert.equal((await login(receiver, receiverId, password, timeoutMs)).errno, 0);

  console.log(`READY_FOR_RABBITMQ_STOP wait_ms=${outageWaitMs}`);
  await delay(outageWaitMs);

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

  assert.ok(onlineDelivery || offlineDelivery, 'message was neither delivered online nor stored offline');
  console.log(JSON.stringify({
    status: 'passed',
    ackState: ack.ack_state,
    delivery: onlineDelivery ? 'online' : 'offline-fallback',
    messageId,
  }, null, 2));
} finally {
  for (const client of clients) client.close();
}

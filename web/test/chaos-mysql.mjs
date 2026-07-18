import assert from 'node:assert/strict';
import crypto from 'node:crypto';

import { MSG, createClient, delay, login, parseEmbedded, register } from './e2e-client.mjs';

const port1 = Number(process.env.CHAT_E2E_PORT_1 ?? 8080);
const port2 = Number(process.env.CHAT_E2E_PORT_2 ?? 8081);
const timeoutMs = Number(process.env.CHAT_CHAOS_TIMEOUT_MS ?? 30_000);
const outageWaitMs = Number(process.env.CHAT_CHAOS_OUTAGE_WAIT_MS ?? 15_000);
const recoveryWaitMs = Number(process.env.CHAT_CHAOS_RECOVERY_WAIT_MS ?? 20_000);
const suffix = `${Date.now()}-${crypto.randomInt(1000, 9999)}`;
const password = 'mysql-chaos-demo-123';
const clients = new Set();

async function trackedClient(port) {
  const client = await createClient(port, { timeoutMs });
  clients.add(client);
  return client;
}

try {
  const sender = await trackedClient(port1);
  let receiver = await trackedClient(port2);
  const senderId = await register(sender, `数据库故障发送-${suffix}`, password, timeoutMs);
  const receiverId = await register(receiver, `数据库故障接收-${suffix}`, password, timeoutMs);
  assert.equal((await login(sender, senderId, password, timeoutMs)).errno, 0);
  assert.equal((await login(receiver, receiverId, password, timeoutMs)).errno, 0);
  receiver.send({ msgid: MSG.LOGOUT, id: receiverId });
  await delay(300);
  receiver.close();
  clients.delete(receiver);

  console.log(`READY_FOR_MYSQL_STOP wait_ms=${outageWaitMs}`);
  await delay(outageWaitMs);

  const messageId = `mysql-outage-${suffix}`;
  const payload = {
    msgid: MSG.ONE_CHAT,
    id: senderId,
    toid: receiverId,
    name: `数据库故障发送-${suffix}`,
    msg: '数据库中断后应返回失败并允许重试',
    time: new Date().toISOString(),
    sent_at_ms: Date.now(),
    client_seq: 1,
    message_id: messageId,
  };
  sender.send(payload);
  const failedAck = await sender.waitFor(
    (message) => message.msgid === MSG.ACK && message.message_id === messageId,
    timeoutMs,
  );
  assert.equal(failedAck.ack_state, 2, 'database write failure must return ACK_FAIL');

  console.log(`READY_FOR_MYSQL_START wait_ms=${recoveryWaitMs}`);
  await delay(recoveryWaitMs);

  sender.send(payload);
  const recoveredAck = await sender.waitFor(
    (message) => message.msgid === MSG.ACK && message.message_id === messageId,
    timeoutMs,
  );
  assert.equal(recoveredAck.ack_state, 0, 'same message id must be accepted after storage recovers');

  receiver = await trackedClient(port2);
  const relogin = await login(receiver, receiverId, password, timeoutMs);
  assert.equal(relogin.errno, 0);
  const recoveredMessages = parseEmbedded(relogin.offlinemsg).filter(
    (message) => message.message_id === messageId,
  );
  assert.equal(recoveredMessages.length, 1, 'recovered message must be stored exactly once');

  console.log(JSON.stringify({
    status: 'passed',
    failedAckState: failedAck.ack_state,
    recoveredAckState: recoveredAck.ack_state,
    recoveredCopies: recoveredMessages.length,
    messageId,
  }, null, 2));
} finally {
  for (const client of clients) client.close();
}

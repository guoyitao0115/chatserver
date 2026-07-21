import assert from 'node:assert/strict';
import crypto from 'node:crypto';

import { MSG, createClient, delay, login, parseEmbedded, register } from './e2e-client.mjs';

// MySQL 故障测试需要配合外部脚本停止/启动数据库容器。
// 本脚本通过 READY_FOR_MYSQL_STOP/START 日志把“停库窗口”暴露给编排脚本。
const port1 = Number(process.env.CHAT_E2E_PORT_1 ?? 8080);
const port2 = Number(process.env.CHAT_E2E_PORT_2 ?? 8081);
const timeoutMs = Number(process.env.CHAT_CHAOS_TIMEOUT_MS ?? 30_000);
const outageWaitMs = Number(process.env.CHAT_CHAOS_OUTAGE_WAIT_MS ?? 15_000);
const recoveryWaitMs = Number(process.env.CHAT_CHAOS_RECOVERY_WAIT_MS ?? 20_000);
const suffix = `${Date.now()}-${crypto.randomInt(1000, 9999)}`;
const password = 'mysql-chaos-demo-123';
const clients = new Set();

// 记录连接用于 finally 统一清理，避免故障测试异常退出后留下在线用户。
async function trackedClient(port) {
  const client = await createClient(port, { timeoutMs });
  clients.add(client);
  return client;
}

try {
  // 先准备一个在线发送者和一个即将离线的接收者。
  // 接收者离线后，单聊必须写入离线消息表，因此能直接验证数据库写入失败处理。
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

  // 通知外部脚本可以停止 MySQL，然后等待数据库真正进入不可用状态。
  console.log(`READY_FOR_MYSQL_STOP wait_ms=${outageWaitMs}`);
  await delay(outageWaitMs);

  // 数据库不可用时发送离线消息，服务端应该返回 ACK_FAIL，而不是假装成功。
  // 这能避免“发送方看到成功，但消息实际上没有落库”的数据一致性问题。
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

  // 通知外部脚本恢复 MySQL。使用相同 message_id 重试是关键：失败请求不能提前写入去重表，
  // 否则恢复后会被误判成重复消息。
  console.log(`READY_FOR_MYSQL_START wait_ms=${recoveryWaitMs}`);
  await delay(recoveryWaitMs);

  sender.send(payload);
  const recoveredAck = await sender.waitFor(
    (message) => message.msgid === MSG.ACK && message.message_id === messageId,
    timeoutMs,
  );
  assert.equal(recoveredAck.ack_state, 0, 'same message id must be accepted after storage recovers');

  // 接收者重新登录后应该只看到一份恢复后的离线消息，证明重试成功且没有重复落库。
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
  // 故障路径上任何一步失败都要释放连接，让下一轮测试能从干净在线状态开始。
  for (const client of clients) client.close();
}

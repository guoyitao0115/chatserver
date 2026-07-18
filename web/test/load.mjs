import assert from 'node:assert/strict';
import crypto from 'node:crypto';

import { MSG, createClient, delay, login, register } from './e2e-client.mjs';

const port1 = Number(process.env.CHAT_E2E_PORT_1 ?? 8080);
const port2 = Number(process.env.CHAT_E2E_PORT_2 ?? 8081);
const pairCount = Number(process.env.CHAT_LOAD_PAIRS ?? 30);
const messagesPerPair = Number(process.env.CHAT_LOAD_MESSAGES ?? 50);
const timeoutMs = Number(process.env.CHAT_LOAD_TIMEOUT_MS ?? 180_000);
const crossNode = port1 !== port2;
const password = 'load-demo-123';
const suffix = `${Date.now()}-${crypto.randomInt(1000, 9999)}`;
const clients = [];

function percentile(sorted, ratio) {
  if (sorted.length === 0) return 0;
  return sorted[Math.min(sorted.length - 1, Math.ceil(sorted.length * ratio) - 1)];
}

try {
  const setupStartedAt = Date.now();
  const pairs = await Promise.all(Array.from({ length: pairCount }, async (_, index) => {
    const sender = await createClient(port1, { timeoutMs });
    const receiver = await createClient(port2, { timeoutMs });
    clients.push(sender, receiver);
    const [senderId, receiverId] = await Promise.all([
      register(sender, `压测发送-${suffix}-${index}`, password, timeoutMs),
      register(receiver, `压测接收-${suffix}-${index}`, password, timeoutMs),
    ]);
    const [senderLogin, receiverLogin] = await Promise.all([
      login(sender, senderId, password, timeoutMs),
      login(receiver, receiverId, password, timeoutMs),
    ]);
    assert.equal(senderLogin.errno, 0);
    assert.equal(receiverLogin.errno, 0);
    return { index, sender, receiver, senderId, receiverId };
  }));
  const setupMs = Date.now() - setupStartedAt;
  const prefix = `load-${suffix}-`;
  const collectors = pairs.map(({ index, sender, receiver }) => ({
    ack: sender.collect((m) => m.msgid === MSG.ACK && String(m.message_id).startsWith(`${prefix}${index}-`), messagesPerPair, timeoutMs),
    delivery: receiver.collect((m) => m.msgid === MSG.ONE_CHAT && String(m.message_id).startsWith(`${prefix}${index}-`), messagesPerPair, timeoutMs),
  }));

  const startedAt = Date.now();
  for (const pair of pairs) {
    for (let sequence = 1; sequence <= messagesPerPair; sequence += 1) {
      const sentAt = Date.now();
      pair.sender.send({
        msgid: MSG.ONE_CHAT,
        id: pair.senderId,
        toid: pair.receiverId,
        name: `压测发送-${pair.index}`,
        msg: `并发消息-${sequence}`,
        time: new Date(sentAt).toISOString(),
        sent_at_ms: sentAt,
        client_seq: sequence,
        message_id: `${prefix}${pair.index}-${sequence}`,
      });
    }
  }

  const settled = await Promise.all(collectors.flatMap(({ ack, delivery }) => [ack, delivery]));
  const elapsedMs = Date.now() - startedAt;
  const ackGroups = settled.filter((_, index) => index % 2 === 0);
  const deliveryGroups = settled.filter((_, index) => index % 2 === 1);
  await delay(500);

  const allAcks = ackGroups.flat();
  const allDeliveries = deliveryGroups.flat();
  const extraDeliveries = pairs.flatMap(({ index, receiver }) => receiver.drain(
    (m) => m.msgid === MSG.ONE_CHAT && String(m.message_id).startsWith(`${prefix}${index}-`),
  ));
  const expected = pairCount * messagesPerPair;
  const uniqueIds = new Set(allDeliveries.map((message) => message.message_id));
  const orderViolations = deliveryGroups.reduce((total, messages) => total + messages.reduce(
    (violations, message, index) => violations + (message.client_seq === index + 1 ? 0 : 1), 0,
  ), 0);
  const latencies = allDeliveries
    .map((message) => message.__receivedAt - message.sent_at_ms)
    .sort((a, b) => a - b);

  assert.equal(allAcks.length, expected, 'all messages must have an ACK');
  assert.ok(allAcks.every((ack) => ack.ack_state === 0), 'all load-test ACK states must be OK');
  assert.equal(allDeliveries.length, expected, 'all messages must arrive');
  assert.equal(uniqueIds.size, expected, 'received message ids must be unique');
  assert.equal(extraDeliveries.length, 0, 'no delayed duplicate messages are allowed');
  assert.equal(orderViolations, 0, 'per-sender order must be preserved');

  console.log(JSON.stringify({
    status: 'passed',
    topology: { serviceNodes: crossNode ? 2 : 1, concurrentWebSockets: pairCount * 2, pairs: pairCount },
    workload: { messagesPerPair, totalMessages: expected, crossNode },
    result: {
      setupMs,
      elapsedMs,
      throughputMessagesPerSecond: Number((expected / (elapsedMs / 1000)).toFixed(2)),
      acked: allAcks.length,
      received: allDeliveries.length,
      lost: expected - uniqueIds.size,
      duplicates: allDeliveries.length + extraDeliveries.length - uniqueIds.size,
      orderViolations,
      latencyMs: {
        min: latencies[0],
        p50: percentile(latencies, 0.5),
        p95: percentile(latencies, 0.95),
        p99: percentile(latencies, 0.99),
        max: latencies.at(-1),
      },
    },
  }, null, 2));
} finally {
  for (const client of clients) client.close();
}

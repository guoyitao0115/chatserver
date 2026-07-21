import assert from 'node:assert/strict';
import crypto from 'node:crypto';

import { MSG, createClient, delay, login, register } from './e2e-client.mjs';

// 负载测试默认同时打到两个 Web 网关端口。两个端口相同时验证单节点吞吐，
// 不同时验证跨 Web 网关、跨后端节点时 Redis/RabbitMQ 协同是否仍然稳定。
const port1 = Number(process.env.CHAT_E2E_PORT_1 ?? 8080);
const port2 = Number(process.env.CHAT_E2E_PORT_2 ?? 8081);

// pairCount 控制并发发送/接收用户对数，messagesPerPair 控制每对用户的消息量。
// 面试里常被追问“高并发下是否丢消息/乱序/重复”，这两个参数就是调压入口。
const pairCount = Number(process.env.CHAT_LOAD_PAIRS ?? 30);
const messagesPerPair = Number(process.env.CHAT_LOAD_MESSAGES ?? 50);
const timeoutMs = Number(process.env.CHAT_LOAD_TIMEOUT_MS ?? 180_000);
const crossNode = port1 !== port2;
const password = 'load-demo-123';

// 后缀和 message_id 前缀保证每次测试数据互不干扰，即使数据库里保留旧记录，
// 本轮断言也只统计本轮产生的消息。
const suffix = `${Date.now()}-${crypto.randomInt(1000, 9999)}`;
const clients = [];

// 输入数组在调用前已经排序。使用 ceil 可以得到常见监控口径中的 p50/p95/p99，
// 空数组返回 0，避免测试失败时统计阶段再抛出二次异常遮蔽真正原因。
function percentile(sorted, ratio) {
  if (sorted.length === 0) return 0;
  return sorted[Math.min(sorted.length - 1, Math.ceil(sorted.length * ratio) - 1)];
}

try {
  // 先批量完成注册和登录，把用户创建耗时与真正的消息吞吐耗时分开统计。
  // 每个 pair 使用独立发送者和接收者，可以暴露连接数增长时的状态隔离问题。
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

  // collect 在发送前启动，防止高并发下服务端太快返回 ACK 或投递消息，
  // 导致测试代码还没开始等待就错过事件。
  const collectors = pairs.map(({ index, sender, receiver }) => ({
    ack: sender.collect((m) => m.msgid === MSG.ACK && String(m.message_id).startsWith(`${prefix}${index}-`), messagesPerPair, timeoutMs),
    delivery: receiver.collect((m) => m.msgid === MSG.ONE_CHAT && String(m.message_id).startsWith(`${prefix}${index}-`), messagesPerPair, timeoutMs),
  }));

  // 同步快速写入所有 pair 的消息，模拟客户端短时间突发发送。
  // client_seq 和 sent_at_ms 是后续顺序、延迟断言的依据。
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

  // 同时等待 ACK 和投递完成：ACK 证明服务端已接受/处理，delivery 证明接收端实际收到。
  // 两者都检查才能覆盖“发送方以为成功但接收方没收到”的故障形态。
  const settled = await Promise.all(collectors.flatMap(({ ack, delivery }) => [ack, delivery]));
  const elapsedMs = Date.now() - startedAt;
  const ackGroups = settled.filter((_, index) => index % 2 === 0);
  const deliveryGroups = settled.filter((_, index) => index % 2 === 1);

  // 多等半秒再 drain，是为了捕获正常收满之后才迟到的重复投递。
  await delay(500);

  const allAcks = ackGroups.flat();
  const allDeliveries = deliveryGroups.flat();
  const extraDeliveries = pairs.flatMap(({ index, receiver }) => receiver.drain(
    (m) => m.msgid === MSG.ONE_CHAT && String(m.message_id).startsWith(`${prefix}${index}-`),
  ));
  const expected = pairCount * messagesPerPair;
  const uniqueIds = new Set(allDeliveries.map((message) => message.message_id));

  // 这里检查的是“同一个发送者到同一个接收者”的顺序。不同 pair 之间没有全局顺序要求，
  // 因为并发连接天然会交错，要求全局有序反而会误判系统设计。
  const orderViolations = deliveryGroups.reduce((total, messages) => total + messages.reduce(
    (violations, message, index) => violations + (message.client_seq === index + 1 ? 0 : 1), 0,
  ), 0);
  const latencies = allDeliveries
    .map((message) => message.__receivedAt - message.sent_at_ms)
    .sort((a, b) => a - b);

  // 负载测试的核心可靠性断言：ACK 不丢、投递不丢、message_id 不重复、
  // 稳定窗口内没有额外重复消息、同一发送者的消息保持顺序。
  assert.equal(allAcks.length, expected, 'all messages must have an ACK');
  assert.ok(allAcks.every((ack) => ack.ack_state === 0), 'all load-test ACK states must be OK');
  assert.equal(allDeliveries.length, expected, 'all messages must arrive');
  assert.equal(uniqueIds.size, expected, 'received message ids must be unique');
  assert.equal(extraDeliveries.length, 0, 'no delayed duplicate messages are allowed');
  assert.equal(orderViolations, 0, 'per-sender order must be preserved');

  // 输出机器可读 JSON，方便复制进测试报告，也方便后续接入 CI 做趋势对比。
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
  // 无论断言在哪一步失败，都主动关闭 WebSocket，避免压测连接悬挂影响下一轮测试。
  for (const client of clients) client.close();
}

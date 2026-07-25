# `load.mjs` 讲解

## 作用概览

**并发负载测试。** 创建多组独立发送/接收连接，突发发送带序号消息，同时统计 ACK、实际投递、重复、乱序、吞吐和延迟分位数。

阅读位置：`web/test/load.mjs`。下文严格按源码顺序展示，每一行只出现一次；解释只针对紧邻的代码片段。

## 代码片段与详细讲解

### 片段 1：第 1-22 行

```javascript
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
```

这些依赖明确了本片段所在层的边界：Redis、RabbitMQ、密码或随机数、断言框架。项目内部头文件提供协议和领域对象，外部库只承担基础能力；业务数据如何流转仍由当前模块决定。

`CHAT_E2E_PORT_1`、`CHAT_E2E_PORT_2`、`CHAT_LOAD_PAIRS`、`CHAT_LOAD_MESSAGES`、`CHAT_LOAD_TIMEOUT_MS` 把拓扑、并发用户对数、每对消息量和总超时暴露给运行者。同一脚本既能以两个不同端口压跨节点链路，也能把端口设成相同值只测单节点。

并发操作在等待器或收集器就绪后同时启动，避免响应太快而被测试代码错过。这里关注的是共享状态竞争：例如重复登录只能有一个赢家，或多连接突发发送后每组仍必须收齐自己的消息。

### 片段 2：第 23-52 行

```javascript

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
```

延迟数组已升序排列，函数按 nearest-rank 口径选择 p50/p95/p99；使用上取整可确保分位点代表至少相应比例样本不超过该值。

### 片段 3：第 53-78 行

```javascript

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
```

每个用户对在发送前建立 ACK 与 delivery 收集器，然后突发写入独立 message_id/序号。完成后把各组结果展平：总数衡量丢失，Set 衡量重复，逐组 client_seq 衡量会话顺序，接收时间减 sent_at_ms 形成延迟样本。

循环每次只消费已经确认完整的字节或已经成功写出的部分。遇到正文尚未到齐便停在当前偏移，下一次收到数据后继续；发送短写则从剩余位置续发，这正是流式 socket 不能假设“一次调用完成一条消息”的原因。

### 片段 4：第 79-106 行

```javascript
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
```

这部分属于“并发负载测试”的状态衔接代码。它只推进当前事件已经确认的结果；异步响应、未匹配消息或未完成缓冲仍保存在本模块中，后续事件到达后继续处理，不会被当作空结果丢弃。

### 片段 5：第 107-136 行

```javascript

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
```

场景断言同时观察协议响应和对端实际结果：仅有发送成功或 ACK 并不足以证明消息送达。数量、字段、错误码或“观察窗口内没有额外消息”共同限定了本片段要验证的系统性质。

### 片段 6：第 137-143 行

```javascript
      },
    },
  }, null, 2));
} finally {
  // 无论断言在哪一步失败，都主动关闭 WebSocket，避免压测连接悬挂影响下一轮测试。
  for (const client of clients) client.close();
}
```

`finally` 会遍历本轮登记的客户端并关闭连接。即使中途断言抛错，服务端也能触发断线清理，避免残留在线路由让下一轮重复登录或离线场景得到假结果。

## 面试重点

- 能否沿着一条单聊消息说明本地直发、跨节点路由、离线落库、ACK 与重试之间的成功语义？

- Redis 或 RabbitMQ 故障时系统如何降级，哪些保证仍成立，哪些保证会变弱？

- 为什么“至少一次发送 + message_id 幂等”不等于严格 Exactly Once？

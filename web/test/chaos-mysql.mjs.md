# `chaos-mysql.mjs` 讲解

## 作用概览

**MySQL 故障测试。** 在数据库不可用窗口执行依赖持久化的操作，确认失败被明确反馈且服务进程不崩溃；恢复后再次操作，验证连接是按请求重建而非永久失效。

阅读位置：`web/test/chaos-mysql.mjs`。下文严格按源码顺序展示，每一行只出现一次；解释只针对紧邻的代码片段。

## 代码片段与详细讲解

### 片段 1：第 1-22 行

```javascript
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
```

新建连接后立即加入统一集合，保证测试在任意断言处抛错时仍能在 finally 中找到并关闭它；这避免失败用例留下在线用户影响下一轮重复登录判断。

外部脚本在 READY 标记后停止或恢复 MySQL。本段故意执行需要落库/查询的操作，要求故障时得到明确失败且进程仍可响应；恢复窗口结束后重新建立客户端操作，证明模型按请求新建连接而非永久持有坏连接。

这些依赖明确了本片段所在层的边界：MySQL C API、密码或随机数、断言框架。项目内部头文件提供协议和领域对象，外部库只承担基础能力；业务数据如何流转仍由当前模块决定。

本片段读取 `CHAT_E2E_PORT_1`、`CHAT_E2E_PORT_2`、`CHAT_CHAOS_TIMEOUT_MS`、`CHAT_CHAOS_OUTAGE_WAIT_MS`、`CHAT_CHAOS_RECOVERY_WAIT_MS`。未设置时采用紧邻的本机默认值；容器部署则覆盖这些值，因此同一二进制可以作为不同节点运行，无需重新编译。

### 片段 2：第 23-52 行

```javascript

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
```

场景断言同时观察协议响应和对端实际结果：仅有发送成功或 ACK 并不足以证明消息送达。数量、字段、错误码或“观察窗口内没有额外消息”共同限定了本片段要验证的系统性质。

### 片段 3：第 53-82 行

```javascript
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
```

`message_id` 贯穿发送、ACK、重试和接收去重：同一业务消息重发时 id 不变，服务端才能识别重复；`ack_state` 则告诉发送者是已接受、已去重还是处理失败，而不是仅凭 TCP 写成功判断业务成功。

### 片段 4：第 83-94 行

```javascript

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
```

`finally` 会遍历本轮登记的客户端并关闭连接。即使中途断言抛错，服务端也能触发断线清理，避免残留在线路由让下一轮重复登录或离线场景得到假结果。

## 面试重点

- 测试准备了什么外部状态或模拟组件，实际动作经过哪些模块？

- 每个断言证明的是返回值正确，还是“不丢、不重、不乱序、不可冒用”等系统性质？

- 如何避免测试自身的等待竞态和上轮残留状态造成假失败？

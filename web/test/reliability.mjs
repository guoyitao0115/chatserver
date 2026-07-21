import assert from 'node:assert/strict';
import crypto from 'node:crypto';

import { MSG, createClient, delay, login, parseEmbedded, register } from './e2e-client.mjs';

// 可靠性测试覆盖“面试中容易被追问”的端到端场景：非法输入、重复登录、
// 身份伪造、跨节点消息、群聊去重、在线/离线顺序、离线消息不重放。
const port1 = Number(process.env.CHAT_E2E_PORT_1 ?? 8080);
const port2 = Number(process.env.CHAT_E2E_PORT_2 ?? 8081);
const timeoutMs = Number(process.env.CHAT_RELIABILITY_TIMEOUT_MS ?? 60_000);

// orderCount/online 和 offlineCount/offline 分别压在线顺序链路和离线存储链路。
// 这两个值可以通过环境变量调大，用于定位高并发下乱序或漏存问题。
const orderCount = Number(process.env.CHAT_ORDER_MESSAGES ?? 200);
const offlineCount = Number(process.env.CHAT_OFFLINE_MESSAGES ?? 100);
const suffix = `${Date.now()}-${crypto.randomInt(1000, 9999)}`;
const password = 'reliability-demo-123';
const clients = new Set();

// 统一记录测试打开的连接，让 finally 能完整清理；这比每段手写 close 更可靠。
async function trackedClient(port) {
  const client = await createClient(port, { timeoutMs });
  clients.add(client);
  return client;
}

// 先发业务登出再关闭连接，避免服务端只感知 TCP 断开而没及时清理在线状态。
async function logoutAndClose(client, id) {
  client.send({ msgid: MSG.LOGOUT, id });
  await delay(200);
  client.close();
  clients.delete(client);
}

// 顺序断言同时检查三件事：message_id 不重复、client_seq 连续递增、
// 时间戳单调。这样可以分别暴露重复投递、乱序投递和离线读取排序错误。
function assertOrdered(messages, startSequence) {
  const ids = new Set(messages.map((message) => message.message_id));
  assert.equal(ids.size, messages.length, 'received message ids must be unique');
  for (let i = 0; i < messages.length; i += 1) {
    assert.equal(messages[i].client_seq, startSequence + i, `out-of-order message at index ${i}`);
    if (i > 0) {
      assert.ok(messages[i].sent_at_ms >= messages[i - 1].sent_at_ms, 'message timestamps must be monotonic');
    }
  }
}

let alice;
let bob;
let carol;

try {
  // 第一组是输入边界：空用户名、畸形 JSON、缺少登录字段都应该被明确拒绝，
  // 不能让异常消息污染业务状态或导致服务端崩溃。
  const invalidClient = await trackedClient(port1);
  invalidClient.send({ msgid: MSG.REGISTER, name: '', password: '123' });
  assert.notEqual((await invalidClient.waitFor((m) => m.msgid === MSG.REGISTER_ACK)).errno, 0);
  invalidClient.sendRawText('{not-json');
  assert.equal((await invalidClient.waitFor((m) => m.msgid === MSG.ERROR)).code, 400);
  invalidClient.send({ msgid: MSG.LOGIN });
  assert.equal((await invalidClient.waitFor((m) => m.msgid === MSG.ERROR)).code, 400);
  invalidClient.close();
  clients.delete(invalidClient);

  alice = await trackedClient(port1);
  bob = await trackedClient(port1);
  carol = await trackedClient(port2);
  const aliceName = `可靠性-甲-${suffix}`;
  const bobName = `可靠性-乙-${suffix}`;
  const carolName = `可靠性-丙-${suffix}`;
  const aliceId = await register(alice, aliceName, password, timeoutMs);
  const bobId = await register(bob, bobName, password, timeoutMs);
  const carolId = await register(carol, carolName, password, timeoutMs);

  // 并发重复登录用于验证在线状态的原子性：同一个用户同时从两个节点登录，
  // 只能有一个连接赢得登录权，否则后续消息会出现双收或状态覆盖。
  const raceSeed = await trackedClient(port1);
  const raceId = await register(raceSeed, `并发登录-${suffix}`, password, timeoutMs);
  raceSeed.close();
  clients.delete(raceSeed);
  const race1 = await trackedClient(port1);
  const race2 = await trackedClient(port2);
  race1.send({ msgid: MSG.LOGIN, id: raceId, password });
  race2.send({ msgid: MSG.LOGIN, id: raceId, password });
  const raceResponses = await Promise.all([
    race1.waitFor((m) => m.msgid === MSG.LOGIN_ACK, timeoutMs),
    race2.waitFor((m) => m.msgid === MSG.LOGIN_ACK, timeoutMs),
  ]);
  assert.equal(raceResponses.filter((response) => response.errno === 0).length, 1,
    'concurrent duplicate login must have exactly one winner');
  const raceWinner = raceResponses[0].errno === 0 ? race1 : race2;
  const raceLoser = raceWinner === race1 ? race2 : race1;
  await logoutAndClose(raceWinner, raceId);
  raceLoser.close();
  clients.delete(raceLoser);

  // 常规登录后，再尝试登录 Alice 的第二个连接，验证重复登录保护在稳定状态也生效。
  assert.equal((await login(alice, aliceId, password, timeoutMs)).errno, 0);
  assert.equal((await login(bob, bobId, password, timeoutMs)).errno, 0);
  assert.equal((await login(carol, carolId, password, timeoutMs)).errno, 0);

  const duplicateLogin = await trackedClient(port2);
  assert.notEqual((await login(duplicateLogin, aliceId, password, timeoutMs)).errno, 0);
  duplicateLogin.close();
  clients.delete(duplicateLogin);

  // 心跳证明连接可用；随后故意让 Alice 冒充 Carol 发消息，验证服务端不会相信客户端传来的 id。
  alice.send({ msgid: MSG.HEARTBEAT, id: aliceId, ts: Date.now() });
  assert.equal((await alice.waitFor((m) => m.msgid === MSG.HEARTBEAT_ACK)).msgid, MSG.HEARTBEAT_ACK);
  alice.send({ msgid: MSG.ONE_CHAT, id: carolId, toid: bobId, name: '伪造用户', msg: '不应送达', message_id: `spoof-${suffix}` });
  assert.equal((await alice.waitFor((m) => m.msgid === MSG.ERROR)).code, 401);

  // 好友和建群需要落库。这里登出再登录，是为了确认这些关系不是只存在内存里。
  const groupName = `可靠性群-${suffix}`;
  alice.send({ msgid: MSG.ADD_FRIEND, id: aliceId, friendid: carolId });
  alice.send({ msgid: MSG.CREATE_GROUP, id: aliceId, groupname: groupName, groupdesc: '完整测试群组' });
  await delay(300);
  await logoutAndClose(alice, aliceId);
  alice = await trackedClient(port1);
  const aliceRelogin = await login(alice, aliceId, password, timeoutMs);
  assert.equal(aliceRelogin.errno, 0);
  assert.ok(parseEmbedded(aliceRelogin.friends).some((friend) => Number(friend.id) === Number(carolId)), 'friend relation must persist');
  const group = parseEmbedded(aliceRelogin.groups).find((item) => item.groupname === groupName);
  assert.ok(group, 'created group must be returned after relogin');

  carol.send({ msgid: MSG.ADD_GROUP, id: carolId, groupid: group.id });
  await delay(300);
  await logoutAndClose(carol, carolId);
  carol = await trackedClient(port2);
  const carolRelogin = await login(carol, carolId, password, timeoutMs);
  assert.equal(carolRelogin.errno, 0);
  assert.ok(parseEmbedded(carolRelogin.groups).some((item) => Number(item.id) === Number(group.id)), 'joined group must persist');

  // 单聊分两段：同节点直投和跨节点投递。跨节点路径会走 Redis/RabbitMQ 等协作链路，
  // 是判断多实例部署是否真的可用的关键。
  const localId = `local-${suffix}`;
  alice.send({ msgid: MSG.ONE_CHAT, id: aliceId, toid: bobId, name: aliceName, msg: '同节点消息', time: new Date().toISOString(), sent_at_ms: Date.now(), message_id: localId, client_seq: 1 });
  assert.equal((await alice.waitFor((m) => m.msgid === MSG.ACK && m.message_id === localId)).ack_state, 0);
  assert.equal((await bob.waitFor((m) => m.msgid === MSG.ONE_CHAT && m.message_id === localId)).msg, '同节点消息');

  const crossId = `cross-${suffix}`;
  alice.send({ msgid: MSG.ONE_CHAT, id: aliceId, toid: carolId, name: aliceName, msg: '跨节点消息', time: new Date().toISOString(), sent_at_ms: Date.now(), message_id: crossId, client_seq: 1 });
  assert.equal((await alice.waitFor((m) => m.msgid === MSG.ACK && m.message_id === crossId)).ack_state, 0);
  assert.equal((await carol.waitFor((m) => m.msgid === MSG.ONE_CHAT && m.message_id === crossId)).msg, '跨节点消息');

  // 群聊先验证正常送达，再用相同 message_id 重发，确认幂等表/去重逻辑能阻断重复投递。
  const groupMessageId = `group-${suffix}`;
  alice.send({ msgid: MSG.GROUP_CHAT, id: aliceId, groupid: group.id, name: aliceName, msg: '群聊消息', time: new Date().toISOString(), sent_at_ms: Date.now(), message_id: groupMessageId, client_seq: 1 });
  assert.equal((await alice.waitFor((m) => m.msgid === MSG.ACK && m.message_id === groupMessageId)).ack_state, 0);
  assert.equal((await carol.waitFor((m) => m.msgid === MSG.GROUP_CHAT && m.message_id === groupMessageId)).msg, '群聊消息');
  alice.send({ msgid: MSG.GROUP_CHAT, id: aliceId, groupid: group.id, name: aliceName, msg: '群聊消息', time: new Date().toISOString(), sent_at_ms: Date.now(), message_id: groupMessageId, client_seq: 1 });
  assert.equal((await alice.waitFor((m) => m.msgid === MSG.ACK && m.message_id === groupMessageId)).ack_state, 1);
  await carol.expectNoMessage((m) => m.message_id === groupMessageId, 800);

  // 特殊字符和长消息用于防 SQL/JSON/HTML 边界问题：服务端应原样保存和转发，
  // 前端再负责展示时转义，不能在中间链路截断或错误解释内容。
  const specialId = `special-${suffix}`;
  const specialContent = `引号'、反斜杠\\、中文、换行\n、<script>不是标签</script>-${'长消息'.repeat(300)}`;
  alice.send({ msgid: MSG.ONE_CHAT, id: aliceId, toid: carolId, name: aliceName, msg: specialContent, time: new Date().toISOString(), sent_at_ms: Date.now(), message_id: specialId, client_seq: 2 });
  assert.equal((await alice.waitFor((m) => m.msgid === MSG.ACK && m.message_id === specialId)).ack_state, 0);
  assert.equal((await carol.waitFor((m) => m.msgid === MSG.ONE_CHAT && m.message_id === specialId)).msg, specialContent);

  // 分布式去重场景：第一次发送成功，第二次携带同一个 message_id 应返回重复 ACK，
  // 接收者也不应该再次收到同一条消息。
  const duplicateId = `duplicate-${suffix}`;
  const duplicatePayload = { msgid: MSG.ONE_CHAT, id: aliceId, toid: carolId, name: aliceName, msg: '只应收到一次', time: new Date().toISOString(), sent_at_ms: Date.now(), message_id: duplicateId, client_seq: 3 };
  alice.send(duplicatePayload);
  assert.equal((await alice.waitFor((m) => m.msgid === MSG.ACK && m.message_id === duplicateId)).ack_state, 0);
  await carol.waitFor((m) => m.msgid === MSG.ONE_CHAT && m.message_id === duplicateId);
  alice.send(duplicatePayload);
  assert.equal((await alice.waitFor((m) => m.msgid === MSG.ACK && m.message_id === duplicateId)).ack_state, 1);
  await carol.expectNoMessage((m) => m.message_id === duplicateId, 800);

  // 在线顺序压测：同一连接连续发送 orderCount 条消息，接收端必须按 client_seq 收到。
  // 如果服务端多线程处理没有保持单连接顺序，这里会立刻暴露。
  const orderPrefix = `order-${suffix}-`;
  const orderStart = Date.now();
  const ackPromise = alice.collect((m) => m.msgid === MSG.ACK && String(m.message_id).startsWith(orderPrefix), orderCount, timeoutMs);
  const deliveryPromise = carol.collect((m) => m.msgid === MSG.ONE_CHAT && String(m.message_id).startsWith(orderPrefix), orderCount, timeoutMs);
  for (let i = 0; i < orderCount; i += 1) {
    alice.send({ msgid: MSG.ONE_CHAT, id: aliceId, toid: carolId, name: aliceName, msg: `顺序消息-${i + 1}`, time: new Date(orderStart + i).toISOString(), sent_at_ms: orderStart + i, message_id: `${orderPrefix}${i + 1}`, client_seq: i + 4 });
  }
  const [orderAcks, orderedMessages] = await Promise.all([ackPromise, deliveryPromise]);
  assert.ok(orderAcks.every((ack) => ack.ack_state === 0), 'all ordered messages must be acknowledged');
  assertOrdered(orderedMessages, 4);

  // 离线顺序压测：让 Carol 下线后连续发送，随后登录时从 offlinemsg 中恢复。
  // 这里能覆盖数据库写入、读取排序、登录后清理离线消息三段逻辑。
  await logoutAndClose(carol, carolId);
  const offlinePrefix = `offline-burst-${suffix}-`;
  const offlineStart = orderStart + orderCount + 1;
  const offlineAckPromise = alice.collect((m) => m.msgid === MSG.ACK && String(m.message_id).startsWith(offlinePrefix), offlineCount, timeoutMs);
  for (let i = 0; i < offlineCount; i += 1) {
    alice.send({ msgid: MSG.ONE_CHAT, id: aliceId, toid: carolId, name: aliceName, msg: `离线消息-${i + 1}`, time: new Date(offlineStart + i).toISOString(), sent_at_ms: offlineStart + i, message_id: `${offlinePrefix}${i + 1}`, client_seq: orderCount + 4 + i });
  }
  assert.ok((await offlineAckPromise).every((ack) => ack.ack_state === 0));
  carol = await trackedClient(port2);
  const offlineLogin = await login(carol, carolId, password, timeoutMs);
  assert.equal(offlineLogin.errno, 0);
  const offlineMessages = parseEmbedded(offlineLogin.offlinemsg).filter((message) => String(message.message_id).startsWith(offlinePrefix));
  assert.equal(offlineMessages.length, offlineCount, 'all offline messages must be restored');
  assertOrdered(offlineMessages, orderCount + 4);

  // 再次登录确认“已消费离线消息不重放”。这是用户最容易感知的重复收消息问题。
  await logoutAndClose(carol, carolId);
  carol = await trackedClient(port2);
  const secondOfflineLogin = await login(carol, carolId, password, timeoutMs);
  assert.equal(secondOfflineLogin.errno, 0);
  const replayed = parseEmbedded(secondOfflineLogin.offlinemsg).filter((message) => String(message.message_id).startsWith(offlinePrefix));
  assert.equal(replayed.length, 0, 'consumed offline messages must not replay on normal relogin');

  console.log(JSON.stringify({
    status: 'passed',
    users: { aliceId, bobId, carolId, raceId },
    counts: { orderedOnline: orderCount, orderedOffline: offlineCount },
    checks: [
      'invalid-input', 'malformed-json', 'register', 'login', 'concurrent-login',
      'duplicate-login', 'heartbeat', 'auth-spoof-rejection', 'friend', 'create-group',
      'join-group', 'local-delivery', 'cross-node-delivery', 'group-delivery',
      'group-dedup', 'special-character-long-message', 'distributed-dedup',
      'no-duplicate-receive', 'online-order', 'online-no-loss',
      'offline-order', 'offline-no-loss', 'offline-no-replay',
    ],
  }, null, 2));
} finally {
  // 测试失败时也清理所有连接，避免在线状态残留影响下一轮可靠性测试。
  for (const client of clients) client.close();
}

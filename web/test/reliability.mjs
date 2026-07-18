import assert from 'node:assert/strict';
import crypto from 'node:crypto';

import { MSG, createClient, delay, login, parseEmbedded, register } from './e2e-client.mjs';

const port1 = Number(process.env.CHAT_E2E_PORT_1 ?? 8080);
const port2 = Number(process.env.CHAT_E2E_PORT_2 ?? 8081);
const timeoutMs = Number(process.env.CHAT_RELIABILITY_TIMEOUT_MS ?? 60_000);
const orderCount = Number(process.env.CHAT_ORDER_MESSAGES ?? 200);
const offlineCount = Number(process.env.CHAT_OFFLINE_MESSAGES ?? 100);
const suffix = `${Date.now()}-${crypto.randomInt(1000, 9999)}`;
const password = 'reliability-demo-123';
const clients = new Set();

async function trackedClient(port) {
  const client = await createClient(port, { timeoutMs });
  clients.add(client);
  return client;
}

async function logoutAndClose(client, id) {
  client.send({ msgid: MSG.LOGOUT, id });
  await delay(200);
  client.close();
  clients.delete(client);
}

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

  assert.equal((await login(alice, aliceId, password, timeoutMs)).errno, 0);
  assert.equal((await login(bob, bobId, password, timeoutMs)).errno, 0);
  assert.equal((await login(carol, carolId, password, timeoutMs)).errno, 0);

  const duplicateLogin = await trackedClient(port2);
  assert.notEqual((await login(duplicateLogin, aliceId, password, timeoutMs)).errno, 0);
  duplicateLogin.close();
  clients.delete(duplicateLogin);

  alice.send({ msgid: MSG.HEARTBEAT, id: aliceId, ts: Date.now() });
  assert.equal((await alice.waitFor((m) => m.msgid === MSG.HEARTBEAT_ACK)).msgid, MSG.HEARTBEAT_ACK);
  alice.send({ msgid: MSG.ONE_CHAT, id: carolId, toid: bobId, name: '伪造用户', msg: '不应送达', message_id: `spoof-${suffix}` });
  assert.equal((await alice.waitFor((m) => m.msgid === MSG.ERROR)).code, 401);

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

  const localId = `local-${suffix}`;
  alice.send({ msgid: MSG.ONE_CHAT, id: aliceId, toid: bobId, name: aliceName, msg: '同节点消息', time: new Date().toISOString(), sent_at_ms: Date.now(), message_id: localId, client_seq: 1 });
  assert.equal((await alice.waitFor((m) => m.msgid === MSG.ACK && m.message_id === localId)).ack_state, 0);
  assert.equal((await bob.waitFor((m) => m.msgid === MSG.ONE_CHAT && m.message_id === localId)).msg, '同节点消息');

  const crossId = `cross-${suffix}`;
  alice.send({ msgid: MSG.ONE_CHAT, id: aliceId, toid: carolId, name: aliceName, msg: '跨节点消息', time: new Date().toISOString(), sent_at_ms: Date.now(), message_id: crossId, client_seq: 1 });
  assert.equal((await alice.waitFor((m) => m.msgid === MSG.ACK && m.message_id === crossId)).ack_state, 0);
  assert.equal((await carol.waitFor((m) => m.msgid === MSG.ONE_CHAT && m.message_id === crossId)).msg, '跨节点消息');

  const groupMessageId = `group-${suffix}`;
  alice.send({ msgid: MSG.GROUP_CHAT, id: aliceId, groupid: group.id, name: aliceName, msg: '群聊消息', time: new Date().toISOString(), sent_at_ms: Date.now(), message_id: groupMessageId, client_seq: 1 });
  assert.equal((await alice.waitFor((m) => m.msgid === MSG.ACK && m.message_id === groupMessageId)).ack_state, 0);
  assert.equal((await carol.waitFor((m) => m.msgid === MSG.GROUP_CHAT && m.message_id === groupMessageId)).msg, '群聊消息');
  alice.send({ msgid: MSG.GROUP_CHAT, id: aliceId, groupid: group.id, name: aliceName, msg: '群聊消息', time: new Date().toISOString(), sent_at_ms: Date.now(), message_id: groupMessageId, client_seq: 1 });
  assert.equal((await alice.waitFor((m) => m.msgid === MSG.ACK && m.message_id === groupMessageId)).ack_state, 1);
  await carol.expectNoMessage((m) => m.message_id === groupMessageId, 800);

  const specialId = `special-${suffix}`;
  const specialContent = `引号'、反斜杠\\、中文、换行\n、<script>不是标签</script>-${'长消息'.repeat(300)}`;
  alice.send({ msgid: MSG.ONE_CHAT, id: aliceId, toid: carolId, name: aliceName, msg: specialContent, time: new Date().toISOString(), sent_at_ms: Date.now(), message_id: specialId, client_seq: 2 });
  assert.equal((await alice.waitFor((m) => m.msgid === MSG.ACK && m.message_id === specialId)).ack_state, 0);
  assert.equal((await carol.waitFor((m) => m.msgid === MSG.ONE_CHAT && m.message_id === specialId)).msg, specialContent);

  const duplicateId = `duplicate-${suffix}`;
  const duplicatePayload = { msgid: MSG.ONE_CHAT, id: aliceId, toid: carolId, name: aliceName, msg: '只应收到一次', time: new Date().toISOString(), sent_at_ms: Date.now(), message_id: duplicateId, client_seq: 3 };
  alice.send(duplicatePayload);
  assert.equal((await alice.waitFor((m) => m.msgid === MSG.ACK && m.message_id === duplicateId)).ack_state, 0);
  await carol.waitFor((m) => m.msgid === MSG.ONE_CHAT && m.message_id === duplicateId);
  alice.send(duplicatePayload);
  assert.equal((await alice.waitFor((m) => m.msgid === MSG.ACK && m.message_id === duplicateId)).ack_state, 1);
  await carol.expectNoMessage((m) => m.message_id === duplicateId, 800);

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
  for (const client of clients) client.close();
}

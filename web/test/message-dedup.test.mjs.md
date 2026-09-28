# `message-dedup.test.mjs` 讲解

## 作用概览

这个测试验证浏览器最终展示去重器的四个关键场景：首次/重复、旧协议兼容、容量淘汰和
退出清空。测试不需要 DOM、WebSocket 或后端服务，失败时可以直接定位去重数据结构。

## 测试代码与场景

```javascript
test('receiver keeps only the first copy of a message_id', () => {
  const dedup = new MessageIdDeduplicator(3);
  assert.equal(dedup.isDuplicate('message-1'), false);
  assert.equal(dedup.isDuplicate('message-1'), true);
});
```

准备一个容量为 3 的空去重器，第一次登记同一 ID 应视为新消息，第二次必须判为重复。
这个断言对应 RabbitMQ 重投或 online/offline 双路径副本。

```javascript
test('receiver accepts legacy messages without message_id', () => {
  const dedup = new MessageIdDeduplicator(3);
  assert.equal(dedup.isDuplicate(''), false);
  assert.equal(dedup.isDuplicate(undefined), false);
});
```

旧客户端消息可能没有 ID。测试要求它们继续进入展示流程，不能因为缺少幂等字段被误删。

```javascript
test('receiver evicts the oldest id when capacity is exceeded', () => {
  const dedup = new MessageIdDeduplicator(2);
  assert.equal(dedup.isDuplicate('message-1'), false);
  assert.equal(dedup.isDuplicate('message-2'), false);
  assert.equal(dedup.isDuplicate('message-3'), false);
  assert.equal(dedup.isDuplicate('message-1'), false);
});
```

容量为 2 时连续插入三个 ID，最早的 `message-1` 应被淘汰；再次出现时被视为新消息。
该断言同时验证了内存上限和淘汰顺序。

```javascript
test('clear starts a new authenticated browser session', () => {
  const dedup = new MessageIdDeduplicator(2);
  dedup.isDuplicate('message-1');
  dedup.clear();
  assert.equal(dedup.isDuplicate('message-1'), false);
});
```

先登记再清空，最后确认相同 ID 可以重新进入。它对应用户主动退出后清理本地敏感会话
状态，而不是网络短暂断线。

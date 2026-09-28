import assert from 'node:assert/strict';
import test from 'node:test';

import { MessageIdDeduplicator } from '../public/message-dedup.js';

test('receiver keeps only the first copy of a message_id', () => {
  const dedup = new MessageIdDeduplicator(3);
  assert.equal(dedup.isDuplicate('message-1'), false);
  assert.equal(dedup.isDuplicate('message-1'), true);
});

test('receiver accepts legacy messages without message_id', () => {
  const dedup = new MessageIdDeduplicator(3);
  assert.equal(dedup.isDuplicate(''), false);
  assert.equal(dedup.isDuplicate(undefined), false);
});

test('receiver evicts the oldest id when capacity is exceeded', () => {
  const dedup = new MessageIdDeduplicator(2);
  assert.equal(dedup.isDuplicate('message-1'), false);
  assert.equal(dedup.isDuplicate('message-2'), false);
  assert.equal(dedup.isDuplicate('message-3'), false);
  assert.equal(dedup.isDuplicate('message-1'), false);
});

test('clear starts a new authenticated browser session', () => {
  const dedup = new MessageIdDeduplicator(2);
  dedup.isDuplicate('message-1');
  dedup.clear();
  assert.equal(dedup.isDuplicate('message-1'), false);
});

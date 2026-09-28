// 浏览器会话内的有界 message_id 去重器。
// Map 保留插入顺序，因此容量超限时可以 O(1) 取得并删除最早记录。
export class MessageIdDeduplicator {
  constructor(maxSize = 10_000) {
    if (!Number.isInteger(maxSize) || maxSize <= 0) {
      throw new RangeError('maxSize must be a positive integer');
    }
    this.maxSize = maxSize;
    this.ids = new Map();
  }

  // 返回 true 表示该 ID 已出现过。空 ID 属于旧协议消息，不做去重。
  isDuplicate(messageId) {
    if (typeof messageId !== 'string' || messageId.length === 0) return false;
    if (this.ids.has(messageId)) return true;

    this.ids.set(messageId, true);
    if (this.ids.size > this.maxSize) {
      this.ids.delete(this.ids.keys().next().value);
    }
    return false;
  }

  clear() {
    this.ids.clear();
  }
}

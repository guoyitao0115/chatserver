# `message-dedup.js` 讲解

## 作用概览

这个文件提供浏览器接收端的有界 `message_id` 去重器。RabbitMQ manual ACK 丢失会
重投消息，publisher confirm 超时也可能让同一消息同时进入节点队列和离线表，因此
最终页面不能假设服务端只会推送一次。

## 代码与讲解

```javascript
export class MessageIdDeduplicator {
  constructor(maxSize = 10_000) {
    if (!Number.isInteger(maxSize) || maxSize <= 0) {
      throw new RangeError('maxSize must be a positive integer');
    }
    this.maxSize = maxSize;
    this.ids = new Map();
  }
```

构造函数强制容量为正整数，防止错误配置导致每次插入都淘汰或集合无界增长。JavaScript
`Map` 保留插入顺序，所以它既能平均 O(1) 查重，也能直接找到最早插入的 ID。

```javascript
  isDuplicate(messageId) {
    if (typeof messageId !== 'string' || messageId.length === 0) return false;
    if (this.ids.has(messageId)) return true;

    this.ids.set(messageId, true);
    if (this.ids.size > this.maxSize) {
      this.ids.delete(this.ids.keys().next().value);
    }
    return false;
  }
```

空 ID 属于旧协议消息，不能可靠判断是否重复，所以继续显示。有效 ID 第一次出现时登记
并返回 false，后续副本返回 true。容量超限时删除最早记录，使内存上限可预测；代价是
被淘汰的极老消息以后再次出现会被当作新消息，这也是“有界幂等”而非 Exactly Once。

```javascript
  clear() {
    this.ids.clear();
  }
}
```

用户主动退出时清空集合，使下一位在同一浏览器标签登录的用户不会继承上一会话状态。
网络短暂重连不会调用 `clear()`，因此同一登录会话内的重投仍能被识别。

## 面试重点

- 为什么服务端 Redis 去重不能替代最终接收端去重？
- 为什么选择有界 Map，而不是无限增长的 Set 或每次扫描消息数组？
- 容量淘汰带来了什么语义边界？

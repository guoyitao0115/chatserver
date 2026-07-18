# change2：去重机制解耦改造（内存 -> Redis优先）

> 日期：2026-03-26
> 背景：你提出“内存去重太复杂且服务重启会丢，能否用 Redis/数据库解耦”

---

## 1. 问题与方案结论

### 问题
当前 `MsgDedup` 是进程内 LRU+TTL：
- 服务重启后内存状态丢失
- 多实例部署时各实例去重状态不共享

### 方案选择
我采用了：**Redis 去重优先 + 本地内存去重兜底**。

为什么选 Redis 而不是数据库：
1. 去重是高频短时判断，Redis `SET NX EX` 是 O(1) 且延迟低；数据库写压更高。
2. 去重只需要 TTL 窗口，不需要长期存储；Redis 天然适合临时键。
3. Redis 可以跨实例共享状态，解决多节点重复处理。
4. Redis 挂掉时可自动回退本地去重，服务不中断。

---

## 2. 这次具体修改了什么

### 修改A：给 Redis 封装去重原子操作

#### 文件与行号
- `include/server/redis/redis.hpp:27`
- `src/server/redis/redis.cpp:67`
- `src/server/redis/redis.cpp:76`

#### 新增接口
```cpp
int markMessageIfFirst(const string &key, int ttlSeconds);
```

#### 核心实现（Redis 原子命令）
```cpp
"SET %s 1 EX %d NX"
```

#### 语义
- 返回 `1`：首次写入（新消息）
- 返回 `0`：键已存在（重复消息）
- 返回 `-1`：执行失败（Redis异常）

#### 解决了什么
把“是否重复”判断从进程内 map 解耦到 Redis，共享于所有服务实例，并支持 TTL 过期自动清理。

---

### 修改B：在 ChatService 中增加“Redis优先去重”策略

#### 文件与行号
- `include/server/chatservice.hpp:33`（去重 key 前缀）
- `include/server/chatservice.hpp:97`（新方法声明）
- `src/server/chatservice.cpp:79`（新方法实现）
- `src/server/chatservice.cpp:89`（调用 Redis 去重）

#### 新增成员/方法
```cpp
static const string REDIS_DEDUP_KEY_PREFIX = "chat:dedup:";
bool isDuplicateWithFallback(const string &msgId);
```

#### 方法逻辑
1. `msgId` 为空：兼容旧客户端，不去重。
2. 先调用 Redis `markMessageIfFirst(key, 120)`：
   - `1` => 新消息
   - `0` => 重复
3. Redis 失败（`-1`）时，回退 `_dedup.isDuplicate(msgId)` 本地去重。

#### 解决了什么
- 正常情况下：去重跨实例共享，可跨重启窗口（依赖 Redis 持久化配置）
- 异常情况下：Redis 不可用也不会彻底失去去重能力

---

### 修改C：将 oneChat/groupChat 的去重入口切换为新策略

#### 文件与行号
- `src/server/chatservice.cpp:411`
- `src/server/chatservice.cpp:501`

#### 修改前
```cpp
if (_dedup.isDuplicate(msgId))
```

#### 修改后
```cpp
if (isDuplicateWithFallback(msgId))
```

#### 解决了什么
聊天与群聊的重复消息判定，统一走 Redis 优先策略，实际生效在核心业务入口。

---

## 3. 代码如何解决“重启丢失”

### 之前（仅内存）
服务一重启，`_dedup` 清空，短时间内的重发消息会被当作新消息。

### 现在（Redis优先）
去重状态写在 Redis key：
- key 示例：`chat:dedup:42_1711382400123_7`
- TTL 120 秒
- 在 TTL 内，即便服务重启，重复消息仍可被识别（只要 Redis 键还在）

> 注：如果你希望“机器重启后也尽量保留”，需在 Redis 开启 AOF/RDB 持久化（部署配置层面，不是代码层面）。

---

## 4. 风险与边界

1. Redis 宕机时：自动降级到本地 `_dedup`，不会阻断服务。
2. 多实例场景：Redis 正常时可共享去重状态，避免跨节点重复。
3. TTL 过后：相同 `message_id` 允许再次处理（这是预期行为，防止键无限增长）。

---

## 5. 本次修改文件清单

1. `include/server/redis/redis.hpp`
2. `src/server/redis/redis.cpp`
3. `include/server/chatservice.hpp`
4. `src/server/chatservice.cpp`

---

## 6. 关键代码索引（便于你快速跳转）

- `include/server/redis/redis.hpp:27`
- `src/server/redis/redis.cpp:67`
- `src/server/redis/redis.cpp:76`
- `include/server/chatservice.hpp:33`
- `include/server/chatservice.hpp:97`
- `src/server/chatservice.cpp:79`
- `src/server/chatservice.cpp:89`
- `src/server/chatservice.cpp:411`
- `src/server/chatservice.cpp:501`

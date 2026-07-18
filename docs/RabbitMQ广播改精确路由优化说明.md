# RabbitMQ 广播改精确路由优化说明

## 背景
现网跨节点转发原方案采用 RabbitMQ `fanout` 广播：
- 每条跨节点消息会被所有服务节点消费一次。
- 实际只有目标用户所在节点需要处理，其他节点均为无效消费。
- 节点数越多，网络/CPU/队列吞吐浪费越明显。

本次改造将其升级为 `direct` 精确路由：
- 仅投递到目标用户所在节点。
- 保留离线兜底，保证不丢消息。

---

## 改造目标
1. 消除 fanout 广播带来的全节点无效消费。
2. 实现 `userId -> serverId` 在线路由。
3. 保留消息可靠性：路由缺失或发布失败时降级离线。
4. 兼容当前业务处理链路（ACK、去重、离线消息）不变。

---

## 核心设计

### 1) RabbitMQ 交换机改为 direct
- 每个实例启动时携带自己的 `_serverId`。
- 消费队列绑定 key = `_serverId`。
- 发送时 `routing_key = toServerId`，仅目标节点可收到。

### 2) Redis 维护在线路由
新增 Redis 路由键：
- key: `chat:user:server:<userId>`
- value: `<serverId>`
- TTL: 120 秒

路由维护策略：
- 登录成功：写入 `userId -> serverId`
- 心跳：刷新 TTL（长连接保活）
- 注销/异常断线：删除路由键

### 3) 投递路径（不变但更精确）
1. 本节点在线：直接推送。
2. 其他节点在线：查 Redis 获取 `toServerId`，RabbitMQ direct 定向发布。
3. 路由缺失或发布失败：离线库兜底。
4. 消费端本地连接不存在（竞态下线）：离线库兜底。

---

## 代码改动清单

### A. RabbitMQ 总线
- `include/server/mq/rabbitmq_bus.hpp`
  - `connect(...)` 增加 `serverId` 参数。
  - `publish(...)` 改为 `publish(toServerId, userid, payload)`。
  - 注释从 fanout 广播改为 direct 精确路由。
- `src/server/mq/rabbitmq_bus.cpp`
  - exchange 类型从 `fanout` 改为 `direct`。
  - 队列绑定 routing_key 从空串改为 `_serverId`。
  - 发布时 routing_key 使用 `toServerId`。

### B. Redis 路由接口
- `include/server/redis/redis.hpp`
  - 新增 `setUserServer(...)`
  - 新增 `getUserServer(...)`
  - 新增 `removeUserServer(...)`
- `src/server/redis/redis.cpp`
  - 实现上述 3 个接口。

### C. ChatService 路由编排
- `include/server/chatservice.hpp`
  - 新增 `_serverId` 成员。
  - `deliverMsg(...)` 注释更新为按 `serverId` 精确路由。
- `src/server/chatservice.cpp`
  - 构造函数中 `_rabbitMqBus.connect(..., _serverId)`。
  - `deliverMsg(...)` 在 online 场景先 `_redis.getUserServer(...)`，再 `_rabbitMqBus.publish(toServerId, ...)`。
  - `login(...)` 成功后 `_redis.setUserServer(...)`。
  - `heartbeat(...)` 里刷新路由 TTL。
  - `loginout(...)` 和 `clientCloseException(...)` 删除在线路由。
  - `handleRabbitMqBusMessage(...)` 本地连接不存在时写离线兜底（不再 ignore）。

---

## 效果评估

### 性能收益
- 改造前：跨节点消息处理复杂度约 `O(N)`（N 节点都要消费）。
- 改造后：约 `O(1)`（仅目标节点消费）。

### 可靠性变化
- 比广播过滤更稳定：目标节点唯一消费。
- 新增路由缺失兜底与消费端兜底，降低竞态丢消息风险。

### 运维注意
- 当前 `_serverId` 默认值为 `server-1`，多实例部署时需确保每个实例唯一。
- 若未来有配置中心，建议将 `_serverId` 放到配置文件/启动参数。

---

## 回归验证建议
1. 双节点部署，分别配置不同 `_serverId`。
2. A 节点用户给 B 节点用户发送消息，确认仅 B 节点消费。
3. 人为删除目标路由键，验证消息进入离线兜底。
4. 目标用户发送中断线，验证消费端兜底离线生效。
5. 检查 ACK/去重链路行为无回归。
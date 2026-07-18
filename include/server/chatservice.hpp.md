# `chatservice.hpp` 讲解

## 作用概览

该文件定义核心业务单例 `ChatService`：用户认证、好友/群组、可靠消息、在线连接、Redis 路由与去重、RabbitMQ 跨节点转发以及 MySQL 离线兜底都在此协调。各 Model 负责具体 SQL，本类负责业务编排。

## 按学习顺序讲解

### 生命周期与分发

- `instance()`：返回进程内唯一实例。
- `ChatService()`：注册 `msgid -> handler`，连接 Redis/RabbitMQ 并设置消费回调。
- `getHandler(msgid)`：返回对应处理器；未知类型返回记录错误的兜底函数。
- `reset()`：服务退出时把数据库中的在线状态重置为离线。

### 认证与错误

- `authenticatedUserId(conn)`：通过连接名 O(1) 查询已绑定用户。
- `requireAuthenticatedUser(conn, claimedUserId, action)`：拒绝未登录连接或消息体冒用其他用户 ID。
- `sendError(conn, code, message)`：发送统一 `ERROR_MSG` 帧。
- `login/reg/loginout/clientCloseException`：分别完成登录、注册、主动注销和异常断线清理。登录还负责原子抢占 Redis 路由、拉取离线消息及好友/群资料。
- `heartbeat`：校验身份、续期 Redis 在线路由并返回心跳 ACK。

### 可靠投递

- `isDuplicateWithFallback(msgId)`：优先用 Redis `SET NX EX` 做跨节点去重，Redis 故障时退回进程内 LRU。
- `forgetMessageMark(msgId)`：投递及离线落库都失败时撤销去重标记，允许同 ID 重试。
- `sendAck(conn, msgId, state)`：返回处理结果，空 ID 兼容旧客户端。
- `deliverMsg(toUserId, msg, route)`：按“本节点连接 → Redis 路由 + RabbitMQ → MySQL 离线消息”选择路径。

### 业务处理器

- `oneChat`：认证、校验 ID、去重、统一投递并 ACK。
- `addFriend`：认证后保存好友关系。
- `createGroup`：创建群并把创建者加入群。
- `addGroup`：把当前用户加入指定群。
- `groupChat`：认证和去重后查询其他成员，逐一调用统一投递；整体失败时撤销标记并返回失败 ACK。
- `handleRabbitMqBusMessage`：消费目标节点消息，本地连接存在则发送，否则写离线库。

## 函数详细说明

### 单例、初始化与分发

#### `static ChatService *instance()`

返回函数内静态实例地址。C++11 起静态局部变量初始化具备线程安全保证；返回裸指针不表示调用方拥有对象，不能 delete。

#### `ChatService()`

私有构造保证外部只能走单例。它建立消息编号到成员函数的绑定，初始化本地去重器，连接 Redis，注册 RabbitMQ 消费回调并连接 broker。初始化失败不会阻止进程启动：Redis 有本地降级，RabbitMQ 失败后跨节点投递会落离线库。

#### `MsgHandler getHandler(int msgid)`

在 `_msgHandlerMap` 中查找并按值返回 `std::function`。未知编号不抛异常，而是返回一个记录错误日志的 lambda，使网络线程不会因 `map::at` 失败退出。

#### `void reset()`

委托 `UserModel::resetState` 批量清理在线状态。它只处理数据库视图，不清理本进程 map；通常用于进程即将退出的路径。

### 连接身份与错误响应

#### `authenticatedUserId(conn)`

校验连接指针后，在 `_connMutex` 保护下用连接名查询 `_connUserMap`。找到返回正用户 ID，未绑定返回 -1；反向表让每条消息无需 O(n) 扫描 `_userConnMap`。

#### `requireAuthenticatedUser(conn, claimedUserId, action)`

把连接真实绑定 ID 与 JSON 声明 ID 比较。匹配且为正数才返回 true；否则记录 action、声明 ID、真实 ID，调用 `sendError(401,...)` 并返回 false。所有能代表用户修改状态或发消息的 handler 都应先调用它。

#### `sendError(conn, code, message)`

连接为空或已断开时直接返回；否则生成 `ERROR_MSG` JSON，经 `FrameCodec` 发送。它统一错误格式，但不会关闭连接，客户端可以修正请求继续使用。

### 去重、确认与投递

#### `isDuplicateWithFallback(msgId)`

空 ID 为兼容旧客户端直接视为新消息。非空 ID 先调用 Redis 原子写入：1 表示第一次，0 表示已存在；只有 -1 故障时才进入本地 LRU。返回 true 的调用方必须跳过所有副作用并回 `ACK_DEDUP`。

#### `forgetMessageMark(msgId)`

同时尝试删除 Redis key 和本地 LRU 条目。用于“所有投递与离线持久化均失败”的补偿，否则发送方重试相同 ID 时会被错误去重。

#### `sendAck(conn,msgId,ackState)`

只对有效连接和非空 ID发送。ACK 携带原 `message_id` 和处理状态，使并发 pending 消息能准确匹配；它确认服务端处理结果，不代表接收人已读。

#### `deliverMsg(toUserId,msg,route)`

1. 锁内查本节点连接并复制智能指针，锁外发送；成功 route=`local`。
2. 本地不在线时查用户数据库状态；若为 online，再从 Redis 取节点 ID 并 RabbitMQ 精确发布，成功 route=`rabbitmq`。
3. Redis 路由缺失、MQ 发布失败或用户离线时，写 `offlinemessage`，route 为 `offline` 或 `offline_fallback`。
4. 返回值只在某条路径成功时为 true；业务 handler 据此决定 ACK_OK 或 ACK_FAIL。

### 账户函数

#### `login(conn,js,time)`

读取 ID/密码并校验；禁止同一连接切换为另一个账号；查询用户并根据哈希前缀走 bcrypt 或旧明文校验。认证通过后用 Redis `SET NX` 抢占路由，更新数据库 online 和双向连接 map；任一步失败会回滚已抢占路由。成功响应还组装离线消息、好友和群组；旧明文密码在本次登录后升级为 bcrypt。

#### `reg(conn,js,time)`

检查昵称非空且不超过 50、密码 6～72 字节；生成 cost 12 bcrypt 哈希，构造 `User` 并插入。成功返回自增 ID，哈希或数据库失败返回注册错误，不把明文落库。

#### `loginout(conn,js,time)`

先验证连接身份；锁内删除用户→连接和连接→用户两张映射；条件删除只属于本节点的 Redis 路由。只有路由未被其他会话接管时才把数据库改为 offline，避免旧连接覆盖新登录。

#### `clientCloseException(conn)`

不信任客户端 JSON，而是从反向连接表找用户；清理本地 map、RabbitMQ 兼容订阅与 Redis 路由。和主动注销一样使用“仅持有者删除”，处理网络断开与跨节点重登竞态。

### 业务函数

#### `oneChat(conn,js,time)`

提取发送者、接收者、消息 ID；认证后做跨节点去重。重复时立即 ACK_DEDUP；新消息序列化原 JSON 并调用 `deliverMsg`。成功 ACK_OK，失败先撤销去重标记再 ACK_FAIL，使同 ID 可重试。

#### `addFriend(conn,js,time)`

验证当前用户后，把 `userid/friendid` 交给 `FriendModel::insert`。当前 Model 无返回值，因此请求没有明确成功响应，这也是可改进点。

#### `createGroup(conn,js,time)`

认证发送者，读取群名/描述，创建 `Group`。数据库生成 ID 后再以 `creator` 角色加入 `groupuser`；两步目前不是事务，第二步失败可能留下无创建者群。

#### `addGroup(conn,js,time)`

认证后把用户以 `normal` 角色加入群。联合主键会阻止重复入群，但当前 handler 未把数据库错误细分给客户端。

#### `groupChat(conn,js,time)`

认证与去重后查询群内其他用户 ID，对每人调用 `deliverMsg`。全部成功才 ACK_OK；存在失败则撤销整个 message ID 并 ACK_FAIL。重试可能让此前成功成员再次经过投递，因此依赖接收端 message ID 去重；更严格实现应记录成员级投递状态。

#### `heartbeat(conn,js,time)`

验证用户身份后，只在 Redis 路由仍等于当前 `serverId` 时续 TTL。续期成功返回心跳 ACK；不匹配说明当前连接已失去会话所有权，应拒绝而不是延长旧路由。

#### `handleRabbitMqBusMessage(userid,msg)`

消费线程收到目标用户消息后，锁内查本地连接、锁外发送；若连接刚断开或不存在则写离线库。它补上“消息已到目标节点但用户状态发生切换”的最后竞态窗口。


## 面试重点

重要性最高。常见问题：如何防重复、如何避免消息丢失、跨节点如何找人、重复登录如何解决、为什么 ACK_FAIL 要撤销去重标记、连接身份绑定如何阻止伪造。还应主动说明边界：当前是“至少一次重试 + 服务端去重”，离线消息拉取尚不是严格事务型消费，RabbitMQ 使用自动 ACK 也存在极端故障窗口。

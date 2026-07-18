# `redis.hpp` 讲解

## 作用概览

该类用一个同步 hiredis 连接实现两类共享状态：消息去重键和“用户 ID → 服务节点”在线路由。所有命令用互斥锁串行化。

## 按学习顺序讲解

- `Redis()/~Redis()`：初始化和释放 hiredis 上下文。
- 拷贝构造与赋值被删除：避免两个对象重复释放同一连接。
- `connect()`：读取环境配置，使用 1.5 秒超时连接 Redis。
- `markMessageIfFirst(key,ttl)`：执行 `SET key 1 EX ttl NX`；1=首次、0=已存在、-1=异常。
- `removeMessageMark(key)`：删除失败业务的去重键。
- `claimUserServer(userid,serverId,ttl)`：原子抢占在线路由，解决跨节点同时登录竞态。
- `refreshUserServerIfMatches(...)`：Lua 比较持有者后续期，旧连接不能续期新会话。
- `getUserServer(userid)`：查询跨节点投递目标。
- `removeUserServerIfMatches(...)`：Lua 条件删除；区分已删、不存在、其他节点持有和异常。

## 函数详细说明

### 生命周期与所有权

- `Redis()` 把 `_context` 设为 nullptr，表示尚未连接。
- `~Redis()` 在互斥锁内释放 hiredis context；连接使用期间对象不能提前析构。
- 拷贝构造与拷贝赋值显式删除，因为 hiredis context 是独占资源，浅复制会造成并发未定义行为和重复释放。

### `bool connect()`

从环境变量读取地址和端口，以有限超时建立同步连接。成功后后续函数共用同一个 context；失败返回 false。当前没有自动重连，连接运行中断开后命令会持续返回故障状态。

### `int markMessageIfFirst(key,ttlSeconds)`

执行 `SET key 1 EX ttl NX`，把“判断不存在”和“写入带过期键”合并为一个 Redis 原子命令。返回 1=首次写入、0=键已存在、-1=参数/连接/命令异常。调用者不能把 -1 当作重复，否则 Redis 故障会丢消息。

### `bool removeMessageMark(key)`

执行 `DEL` 撤销失败消息的共享去重标记。函数只判断回复类型是否为整数，没有要求删除数量必须为 1，因此键已经不存在也可视作命令成功。

### `int claimUserServer(userid,serverId,ttlSeconds)`

对 `chat:user:server:<userid>` 执行 `SET ... NX EX`。多个节点并发登录时只有一个收到 OK；TTL 防止持有节点崩溃后路由永久残留。返回语义与去重写入一致。

### `int refreshUserServerIfMatches(userid,serverId,ttlSeconds)`

通过 Lua 在服务器端原子执行“GET 值等于本节点 → EXPIRE”。若拆成客户端 GET 再 EXPIRE，期间可能发生新节点接管，旧连接会错误续期新会话。返回 1 成功、0 不存在/不匹配、-1 异常。

### `string getUserServer(userid)`

读取路由键并返回 serverId；不存在、非法参数或 Redis 错误都返回空串。业务层将空串视为无法精确路由并降级离线存储，因此空串同时表示“确实无路由”和“查询失败”。

### `int removeUserServerIfMatches(userid,serverId)`

Lua 先读取当前值：不存在返回 2，匹配则 DEL 并返回 1，不匹配返回 0，异常返回 -1。区分这些状态让注销代码知道旧连接是否还有权把数据库状态改成 offline。

### 并发说明

所有公开命令都用同一 `_mutex`，因为同步 hiredis context 不能被多个 Muduo I/O 线程同时读写。正确性简单，但所有 Redis 请求串行，压力较大时需连接池或按线程分片。


## 面试重点

重要性最高。常见问题：为什么“先 GET 再 DEL/EXPIRE”不安全？两条命令间会发生会话切换，Lua 保证原子比较并修改；为什么键有 TTL？进程崩溃未清理时能自动恢复；单同步连接和全局锁的瓶颈？高并发下应使用连接池或按 I/O 线程分连接。

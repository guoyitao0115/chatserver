# `redis.cpp` 讲解

## 作用概览

该文件实现 Redis 同步命令，将跨节点共享去重和会话路由压缩为少量原子操作。

## 按学习顺序讲解

- `Redis()`：把上下文初始化为空。
- `~Redis()`：持锁释放上下文，防止析构与命令并发。
- `connect()`：读取地址和端口，以超时连接；失败时清理半初始化上下文。
- `markMessageIfFirst(key,ttl)`：使用二进制安全的 `%b` 和 `SET NX EX`；OK、NIL、异常分别映射为 1、0、-1。
- `removeMessageMark(key)`：执行 DEL，供失败重试撤销幂等记录。
- `claimUserServer(userid,serverId,ttl)`：原子创建路由，只有首个并发登录成功。
- `refreshUserServerIfMatches(...)`：Lua 中比较 value 后 EXPIRE，防止旧连接续期新节点会话。
- `getUserServer(userid)`：GET 并按 Redis 返回长度构造节点 ID。
- `removeUserServerIfMatches(...)`：Lua 区分不存在、匹配删除和不匹配，防止旧连接删掉新会话。

## 函数详细说明

### `Redis::Redis()`

构造函数只把 `_context` 初始化为 `nullptr`，表示还没有建立 Redis 连接。这样析构和各个命令函数都能通过空指针判断当前是否可用。

它不在构造阶段自动连接，是为了让调用方显式决定连接时机，并能在连接失败时继续以降级模式运行。例如消息去重可以在 Redis 不可用时退回本地内存去重。

### `Redis::~Redis()`

析构函数先加 `_mutex`，再检查 `_context` 并调用 `redisFree`。加锁是为了避免其他线程正在使用 hiredis 同步连接时同时释放底层上下文。

释放后把 `_context` 置空，避免悬空指针。当前类持有的是单个同步连接，所以所有命令都通过同一把锁串行化，简单可靠，但高并发下会成为吞吐瓶颈。

### `connect()`

该函数从环境变量读取 Redis 地址和端口，默认连接 `127.0.0.1:6379`，并设置 1.5 秒超时。随后调用 `redisConnectWithTimeout` 建立同步连接。

如果连接失败，函数会打印原因，释放半初始化的 `_context`，并返回 false。这样上层可以继续启动服务，只是跨节点路由或 Redis 去重能力会受影响。

连接成功后返回 true。当前没有实现自动重连，因此运行过程中 Redis 断开时，后续命令可能返回异常语义，业务层需要按返回值降级或失败处理。

### `markMessageIfFirst(key, ttlSeconds)`

这个函数用于跨节点消息去重。`key` 通常由 `message_id` 派生，`ttlSeconds` 是去重记录保留时间。

实现使用 Redis 的 `SET key 1 EX ttl NX`，这是一个原子命令：只有 key 不存在时才写入并设置过期时间。返回值约定为 1 表示首次出现，0 表示重复消息，-1 表示 Redis 不可用或参数非法。

命令中使用 `%b`，是 hiredis 的二进制安全格式，能正确处理包含特殊字符的 key。业务层拿到 0 会返回 `ACK_DEDUP`，拿到 -1 则退回本地 `MsgDedup`，避免 Redis 故障时系统完全不可用。

### `removeMessageMark(key)`

这个函数删除消息去重标记，通常在“消息最终没有投递也没有落库”时调用。这样客户端用同一个 `message_id` 重试时，不会被错误地当成重复消息拒绝。

实现调用 `DEL key`。返回 true 只表示 Redis 返回了整数类型结果，不严格区分删除了 1 个 key 还是 key 原本不存在。这个语义对撤销标记足够，因为目标是尽力清理。

### `claimUserServer(userid, serverId, ttlSeconds)`

这个函数用于登录时抢占用户路由。Redis key 形如 `chat:user:server:<userid>`，value 是当前服务节点 ID，TTL 由心跳续期。

实现同样使用 `SET key serverId EX ttl NX`，保证并发登录时只有第一个连接能成功写入。返回 1 表示抢占成功，0 表示该用户已经在其他连接或节点在线，-1 表示 Redis 出错或参数非法。

这是项目避免同一用户多端/多节点同时登录的核心逻辑。需要注意，它依赖 Redis 的原子性，但如果 Redis 自身发生主从切换或网络分区，仍不能声称覆盖所有分布式一致性故障模型。

### `refreshUserServerIfMatches(userid, serverId, ttlSeconds)`

这个函数用于心跳续期，但只允许当前连接仍持有路由时续期。它通过 Lua 脚本完成“读取 value、比较 serverId、续期 EXPIRE”三个动作，避免中间被其他连接抢占后旧连接误续期。

返回值来自 Redis 整数结果：通常 1 表示匹配并续期成功，0 表示 key 不存在或 value 不匹配，-1 表示命令失败。业务层在心跳中如果发现不是当前 server 持有，应拒绝旧连接继续工作。

这个设计解决的是“旧连接不能续命新会话”的问题，是面试里解释 Lua 原子性的好例子。

### `getUserServer(userid)`

这个函数查询某个用户当前在哪个服务节点。单聊或群聊投递时，如果本地连接表找不到目标用户，就会调用它判断目标是否在其他节点。

实现用 `GET key`，如果 Redis 返回字符串，就按 `reply->len` 构造 `serverId`；如果 key 不存在、连接失败或参数非法，就返回空字符串。

返回空字符串并不一定表示用户永久离线，也可能是 Redis 短暂故障。因此业务层通常把它作为“无法跨节点直达”的信号，然后尝试离线落库兜底。

### `removeUserServerIfMatches(userid, serverId)`

这个函数用于退出或断线时释放用户路由，但只在 Redis 中的 value 仍等于当前 serverId 时删除。它也使用 Lua 脚本，避免旧连接删掉新登录连接刚写入的路由。

返回值语义是：1 表示匹配并删除，0 表示 value 不匹配，2 表示 key 已不存在，-1 表示执行失败。业务层可以根据“是否仍由当前节点持有”决定是否更新数据库状态为离线。

这个函数和 `refreshUserServerIfMatches` 是一组：一个保护续期，一个保护删除，共同解决跨节点登录和断线竞态。

## 面试重点

重要性最高。重点准备 Redis 原子性、Lua 的必要性、TTL 心跳续期、故障降级和同步连接锁的吞吐瓶颈。可能追问：Redis 主从切换是否绝对避免双登录？不能保证所有故障模型下线性一致，严格需求需更强一致性存储或 fencing token。

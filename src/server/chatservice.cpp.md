# `chatservice.cpp` 讲解

## 作用概览

这是项目的业务编排核心。它把网络请求连接到 MySQL、Redis、RabbitMQ 与本地在线连接表，完成认证、单聊/群聊、消息 ACK、跨节点去重与离线兜底。

## 按学习顺序讲解

### 初始化与辅助函数

- `isBcryptHash(hash)`：识别 `$2a$/$2b$/$2y$` 前缀，用于兼容旧明文密码并渐进升级。
- `instance()`：函数内静态对象实现线程安全单例初始化。
- `ChatService()`：注册九类请求 handler，读取节点 ID，连接 Redis，先设置 RabbitMQ 回调再建立连接，防止消费线程启动时回调为空。
- `reset()`：进程退出时重置数据库在线状态。
- `getHandler(msgid)`：查分发表；未知 ID 返回记录错误的空处理器。

### 连接身份

- `authenticatedUserId(conn)`：锁内通过 `conn->name()` 查反向映射。
- `sendError(conn,code,message)`：只向有效连接发送统一错误帧。
- `requireAuthenticatedUser(conn,claimedUserId,action)`：要求连接绑定 ID 与请求声明 ID 完全相等，否则返回 401，阻止伪造他人发消息、建群或注销。

### 去重、ACK 与统一投递

- `isDuplicateWithFallback(msgId)`：空 ID 兼容旧端；正常用 Redis 原子标记，Redis 异常时才使用本地 `MsgDedup`。
- `forgetMessageMark(msgId)`：删除 Redis 与本地标记，确保彻底投递失败后同 ID 可重试。
- `sendAck(conn,msgId,ackState)`：构造 `MSG_ACK`；旧客户端未带 ID 时不发送。
- `deliverMsg(toUserId,msg,route)`：先复制本地连接并锁外发送；否则查询用户状态和 Redis 节点路由，经 RabbitMQ direct 转发；路由缺失或发布失败时写 MySQL 离线表。只有发送或落库成功才返回 true。

### 账户生命周期

- `login(conn,js,...)`：校验参数与连接是否已绑定其他账号；查询用户并校验 bcrypt/旧明文；通过 Redis `SET NX` 原子抢占跨节点路由；更新数据库和双向连接表；旧密码升级 bcrypt；查询并返回离线消息、好友和群组。重复登录只有一个节点能成功。
- `reg(conn,js,...)`：校验昵称和密码长度，用 cost 12 生成 bcrypt，再插入用户并返回自增 ID。
- `loginout(conn,js,...)`：认证后删除本地双向映射，条件释放 Redis 路由；只有当前节点仍持有路由时才把数据库状态改为离线。
- `clientCloseException(conn)`：从连接反向表定位用户，删除本地映射并条件释放路由；同样避免旧连接覆盖其他节点的新会话。

### 业务处理器

- `oneChat(conn,js,...)`：检查必要字段、认证发送者和 `message_id` 去重；调用 `deliverMsg`。成功返回 `ACK_OK`，重复返回 `ACK_DEDUP`，完全失败撤销标记并返回 `ACK_FAIL`。
- `addFriend(conn,js,...)`：认证后写好友关系。
- `createGroup(conn,js,...)`：认证并校验名称，创建群成功后以 `creator` 身份入群。
- `addGroup(conn,js,...)`：认证后以 `normal` 身份加入群。
- `groupChat(conn,js,...)`：认证和去重后查询除自己外的成员，逐个统一投递；任一成员投递失败会使本次 ACK 失败并撤销去重标记。
- `heartbeat(conn,js,...)`：认证后用 Lua 条件续期 Redis 路由，返回 `HEARTBEAT_MSG_ACK`；路由已不属于当前实例时拒绝旧连接。
- `handleRabbitMqBusMessage(userid,msg)`：目标节点消费消息后查本地连接；在线则发帧，否则写离线表，覆盖节点切换或断线竞态。

## 函数详细说明

### `isBcryptHash(const string &hash)`

这是文件内的兼容性辅助函数，用密码串前缀判断数据库内容是否已经是 bcrypt 哈希。识别 `$2a$`、`$2b$` 和 `$2y$`，是为了兼容常见 bcrypt 版本。登录时只有识别为哈希才调用 bcrypt 校验；旧项目遗留的明文密码则暂时按明文比较，并在验证成功后立即升级为哈希。它是迁移策略，不是完整的哈希合法性验证器。

### `ChatService::instance()`

函数返回进程内唯一的 `ChatService`。对象使用函数局部静态变量创建，C++11 起初始化过程由语言保证线程安全，可以避免多个 I/O 线程首次访问时重复构造。单例集中持有 handler 表、连接表和模型对象，但也意味着测试隔离需要显式清理其内部状态。

### `ChatService::ChatService()`

构造函数建立业务分发表，把登录、注册、单聊、好友、群组、注销和心跳等 `msgid` 映射到成员函数。网络层只需根据消息号查 handler，无需堆叠大型 `switch`。

随后它读取当前服务节点 ID，配置 Redis 和 RabbitMQ。Redis 用于在线路由、跨节点登录互斥和消息去重；RabbitMQ 用于把消息定向发送到另一个聊天节点。RabbitMQ 消费回调在连接前设置，避免消费线程已经收到消息但回调尚未初始化的窗口。外部组件连接失败时会记录降级状态，后续投递仍可尝试离线表兜底。

### `reset()`

该函数调用用户模型把数据库中的在线状态复位，主要用于服务启动或退出时修正上次异常终止留下的脏状态。数据库 `state` 只是展示和兜底字段，实时跨节点归属仍以带过期时间的 Redis 路由为准，否则多节点各自重置数据库可能破坏真实会话。

### `getHandler(int msgid)`

函数在 `_msgHandlerMap` 中查找业务处理器。存在时返回已绑定的可调用对象；不存在时返回一个安全的兜底 lambda，记录未知消息号，而不是返回空函数后由调用者误调用。新增协议消息时需要同时维护协议枚举、这里的注册以及客户端处理逻辑。

### `authenticatedUserId(const TcpConnectionPtr &conn)`

该函数以连接名为键查询 `_connUserMap`，返回这条 TCP 连接当前绑定的用户 ID。查询过程受 `_connUserMapMutex` 保护，因为多个 Muduo I/O 线程可能并发登录、发消息和断线。未登录或映射已被清理时返回空值，调用方不能把请求 JSON 中的 `id` 当成可信身份。

### `sendError(const TcpConnectionPtr &conn, int code, const string &message)`

函数统一构造 `{msgid: ERROR_MSG, code, message}`，再通过 `FrameCodec` 编码发送。发送前检查连接仍有效，避免异常处理过程中再次对关闭连接写数据。集中封装可以让各种认证失败、参数错误和服务异常保持相同响应结构。

### `requireAuthenticatedUser(const TcpConnectionPtr &conn, int claimedUserId, const string &action)`

这是所有敏感业务操作的前置鉴权。它先取连接真实绑定的用户 ID，再与请求中声明的 ID 比较；未登录或二者不一致时返回 401，并在错误信息中指出当前动作。验证成功才返回 true。

这一步修复了典型的“只相信 JSON 里的发送者 ID”漏洞：攻击者即使修改 `id` 字段，也不能冒充其他用户发消息、加好友、建群或注销。连接身份应在登录成功后绑定，并在断线或注销时可靠清除。

### `isDuplicateWithFallback(const string &msgId)`

函数用于判定业务消息是否已经处理。新客户端提供 `message_id` 时，优先调用 Redis 原子写入并设置 TTL：第一次写入成功表示可以处理，键已存在表示重复。Redis 不可用或操作异常时才退化到进程内 `MsgDedup`，保持单节点内的幂等性。

空 ID 被视为兼容旧客户端而不参与去重。Redis 方案能覆盖多节点重试，本地方案无法在重启和跨节点后保留记录，因此这是可用性优先的降级，不应描述成所有故障下的 exactly-once。

### `forgetMessageMark(const string &msgId)`

当消息既未送达在线连接，也未成功发布或写入离线库时，函数删除 Redis 和本地去重标记。这样客户端使用相同 ID 重试仍有机会重新执行。如果业务已经产生部分不可逆副作用就不能随意撤销标记，因此调用位置必须和实际投递语义保持一致。

### `sendAck(const TcpConnectionPtr &conn, const string &msgId, int ackState)`

函数为带 `message_id` 的请求构造 `MSG_ACK`，返回成功、重复已处理或失败状态。旧客户端没有 ID 时跳过 ACK，避免发送其无法理解的响应。客户端收到成功或去重 ACK 后删除 pending；失败则可以保留或重试。

ACK 表示服务端已完成当前实现定义的投递动作，例如已发给在线连接、成功交给 MQ，或成功落入离线表，并不证明最终用户已经阅读消息。面试时应明确这一语义边界。

### `deliverMsg(int toUserId, const json &msg, const string &route)`

统一投递函数按本地连接、跨节点总线、离线数据库的顺序尝试。它先在互斥锁内查找目标用户的本地连接并复制智能指针，随后释放锁再发送，避免慢 I/O 占用全局连接表锁。目标不在本节点时查询 Redis 路由；如果路由指向其他节点，则通过 RabbitMQ direct 路由发布。

用户离线、路由不存在、路由已失效或 MQ 发布失败时，函数把序列化消息写入 MySQL 离线表。只有在线发送、MQ 接收发布或离线持久化至少一个成功时才返回 true。这个集中入口让单聊和群聊复用相同可靠性规则，也便于以后增加持久化消息表、投递状态机或监控指标。

### `login(const TcpConnectionPtr &conn, json &js, Timestamp time)`

登录先检查 `id`、`password` 等参数，并确认当前连接没有绑定另一个账号。之后查询用户：bcrypt 记录用安全校验函数比较，遗留明文记录作兼容比较。凭据通过后，调用 Redis 原子 `SET NX EX` 抢占“用户 → 节点”路由；若键已由其他有效节点持有则拒绝重复登录，从而把跨节点竞态收敛到一个原子操作。

抢占成功后更新数据库在线状态，并在互斥锁保护下同时写入“用户 → 连接”和“连接 → 用户”两张表。若旧密码是明文，会在本次成功登录后写回 bcrypt。随后查询离线消息、好友和群组，组装登录成功响应；离线消息被纳入响应后再从离线表删除。

该流程的关键是失败回滚：如果路由已抢占而后续关键步骤失败，应释放仅属于当前节点的路由，避免用户被幽灵在线状态阻塞。离线查询与删除不是事务化消费，极端崩溃窗口仍可能导致重复或遗漏，需要在更高等级方案中用消息状态表和确认机制完善。

### `reg(const TcpConnectionPtr &conn, json &js, Timestamp time)`

注册处理器提取昵称和密码，检查类型、非空和长度，再使用 bcrypt cost 12 生成不可逆哈希。构造用户模型并插入数据库后，成功响应返回数据库自增用户 ID，失败响应给出明确状态。

哈希必须在写库前完成，数据库永远不应新增明文密码。cost 越高抗暴力破解越强，但也会消耗 CPU；生产环境需要结合机器性能、注册/登录限流和压测结果选取参数。

### `loginout(const TcpConnectionPtr &conn, json &js, Timestamp time)`

主动注销先通过连接身份校验，随后删除两张本地映射。Redis 路由使用“值等于当前 serverId 才删除”的条件操作，避免旧连接迟到的注销请求误删用户在新节点建立的会话。只有确认当前节点仍拥有或刚刚释放该路由时，才把数据库状态更新为离线。

这类条件删除通常由 Lua 脚本保证比较和删除原子完成。若简单地先 `GET` 再 `DEL`，两个命令之间用户可能已经重新登录到其他节点，产生竞态。

### `clientCloseException(const TcpConnectionPtr &conn)`

异常断线没有可信的请求 JSON，因此函数从连接反向映射查出用户，再删除正向和反向映射。随后执行与主动注销相同的 Redis 条件释放和数据库状态修正。

函数需要容忍重复调用：网络层关闭事件、错误路径和进程清理可能先后触发，找不到映射时应安全返回。使用连接反向表也让清理复杂度保持近似 O(1)。

### `oneChat(const TcpConnectionPtr &conn, json &js, Timestamp time)`

单聊处理器先校验发送者、接收者、消息内容和消息 ID，再用连接绑定身份验证发送者。去重检查返回重复时，不会再次投递，而是返回 `ACK_DEDUP`，让因 ACK 丢失而重试的客户端安全结束 pending。

首次消息调用 `deliverMsg`。投递或持久化成功后返回 `ACK_OK`；所有路径均失败则撤销去重标记并返回 `ACK_FAIL`，允许客户端稍后使用相同 ID 重试。消息中的客户端序列号会原样传递，接收端可按会话排序，但服务端不会以发送时间重新排列全局消息。

### `addFriend(const TcpConnectionPtr &conn, json &js, Timestamp time)`

函数验证当前连接与请求用户一致，检查好友 ID 后调用好友模型写入关系。当前模型通常以两条有向记录表达双向好友，具体原子性依赖 SQL 实现。更完整的产品还需要好友申请、同意、拒绝、重复关系检查和不存在用户校验。

### `createGroup(const TcpConnectionPtr &conn, json &js, Timestamp time)`

建群先认证创建者并校验群名称、描述等字段，再插入群基本信息。数据库生成群 ID 后，立即把创建者以 `creator` 角色写入群成员表。两个写操作在逻辑上应当是一个事务；如果当前模型未包事务，第二步失败可能留下无创建者群，是后续优化重点。

### `addGroup(const TcpConnectionPtr &conn, json &js, Timestamp time)`

加入群处理器认证用户后，把用户以 `normal` 角色写入指定群。当前接口偏教学演示，通常还需要检查群是否存在、是否已经加入、是否需要审批，以及用唯一索引处理并发重复加入。

### `groupChat(const TcpConnectionPtr &conn, json &js, Timestamp time)`

群聊先完成字段校验、连接身份认证和 `message_id` 去重，再查询群内除发送者外的成员列表。对每个成员调用 `deliverMsg`，因此本机在线、其他节点在线和离线用户共享同一套投递策略。

如果所有目标均成功，发送者收到 `ACK_OK`；检测到重复则收到 `ACK_DEDUP`。只要有成员完全投递失败，当前实现会撤销整条消息的去重标记并返回失败，这会让重试时已成功的成员再次经过投递，依赖接收侧去重才能避免重复展示。更严谨的实现应记录“消息—成员”级状态，只重试失败成员。

### `heartbeat(const TcpConnectionPtr &conn, json &js, Timestamp time)`

心跳先确认连接绑定身份，再通过 Redis 条件续期脚本刷新在线路由 TTL。只有路由值仍等于当前 serverId 才能续期，防止旧节点上的僵尸连接覆盖用户已经迁移到新节点的归属。

续期成功返回 `HEARTBEAT_MSG_ACK`；路由丢失或已归属其他节点时返回错误，并由客户端/服务端后续关闭陈旧会话。心跳同时解决 NAT/连接存活检测和 Redis 临时路由租约续期，但不能代替业务消息 ACK。

### `handleRabbitMqBusMessage(int userId, const string &msg)`

RabbitMQ 消费线程收到定向到本节点的消息后，函数查询本地用户连接。用户仍在线则编码后发送；连接已经离开本节点则写入离线表，以覆盖“发布时路由指向本节点、消费时用户刚好断线或迁移”的竞态窗口。

该回调可能运行在 MQ 自己的线程中，因此访问连接表必须加锁，发送时仍应先复制连接再释放锁。RabbitMQ 的确认模式决定进程崩溃时消息是否会重投；若使用自动确认，就不能宣称消费端崩溃场景绝不丢失，生产方案应采用手动 ACK、持久化交换机/队列和发布确认。

## 面试重点

重要性最高。建议重点准备：跨节点登录竞态为何要 `SET NX`；“本地→MQ→离线库”如何降低丢失概率；ACK_OK 的准确语义；Redis 故障时一致性如何降级；为什么身份必须绑定连接；离线查询后删除与 RabbitMQ 自动 ACK 的极端窗口；群聊部分成功如何设计更精确的成员级状态。回答时不要声称绝对 exactly-once，本实现更接近“至少一次发送 + 幂等去重 + 持久化兜底”。

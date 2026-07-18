# `main.cpp`（客户端）讲解

## 作用概览

这是命令行聊天客户端的完整实现。它建立 TCP 长连接，使用长度帧协议收发 JSON，提供注册/登录/好友/群聊命令，并用 `message_id + ACK + 超时重试 + 客户端序列号` 改善消息可靠性与展示顺序。

## 按学习顺序讲解

### 1. 帧收发

- `sendFrame(fd,payload)`：调用公共编码器，并在全局发送锁内循环 `send` 直到整帧发完；处理 `EINTR`，避免多个后台线程把字节交叉写入同一 socket。
- `recvAll(fd,buf,n)`：精确读取 n 字节，解决一次 `recv` 不保证返回全部数据的问题。
- `recvFrame(fd)`：先读 4 字节头、校验长度，再读完整 payload；失败返回空串。

### 2. 唯一 ID 与会话序列

- `SnowflakeIdGenerator(workerId)`：将 worker 限制为 10 位。
- `setWorkerId(workerId)`：登录后用用户 ID 低 10 位更新 worker，持锁保证并发安全。
- `nextId()`：组合 41 位相对时间、10 位 worker 和 12 位毫秒序列；时钟回拨时保持不倒退，序列耗尽时等待下一毫秒。
- `nowMs()`、`waitNextMs(lastTs)`：分别读取毫秒时间和等待时间越过上一毫秒。
- `nextClientSeqForPeer(toid)`、`nextClientSeqForGroup(groupid)`：分别按接收者、群组维护单调序列，避免不同会话互相影响。
- `generateMsgId()`：把雪花 ID 转成字符串作为 `message_id`。

### 3. ACK 与重试

- `addPending(msgId,payload)`：同时记录待确认负载、发送时刻和重试次数。
- `removePending(msgId)`：收到成功/去重 ACK 或本地发送失败后清理三张表。
- `retryTaskHandler()`：每秒扫描一次；超过 5 秒未确认则最多重发 3 次。锁内只整理任务，锁外执行网络发送，减少阻塞。
- `heartbeatTaskHandler(clientfd)`：登录后每 10 秒发心跳；上一轮未确认时告警，本轮发送前重置 ACK 状态。

### 4. 主线程与响应同步

- `responseVersion()`：读取当前响应版本。
- `waitForResponse(previousVersion)`：条件变量等待版本增长，避免登录/注册用忙等轮询。
- `notifyResponse()`：接收线程处理登录或注册响应后增加版本并唤醒主线程。
- `main(argc,argv)`：解析地址、连接 socket，启动接收/重试/排序/心跳线程，再运行登录注册菜单。

### 5. 收包、离线数据和顺序显示

- `buildSessionKey(js)`：单聊按发送者，群聊按“群 ID + 发送者”隔离顺序空间。
- `formatIncomingLine(js)`：把消息格式化成命令行文本。
- `printOrderedIncoming(js)`：无序号时直接显示；有序号时去掉旧消息，把新消息放入有序 map，并连续输出 `last+1`。
- `orderFlushTaskHandler()`：序号缺口超过 2 秒或缓存达到 100 条时跳过缺口，避免永久队头阻塞。
- `doRegResponse(response)`：打印注册成功 ID 或失败。
- `doLoginResponse(response)`：设置当前用户和雪花 worker，解析嵌套好友/群/离线消息，最后标记登录成功。
- `readTaskHandler(clientfd)`：持续收完整帧并分派；ACK_OK/DEDUP 清 pending，聊天消息进入排序器，心跳 ACK 更新连接状态，错误统一打印，登录/注册响应通知主线程。
- `showCurrentUserData()`：打印当前用户、好友和群成员。
- `getCurrentTime()`：用线程安全的 `localtime_r` 生成消息显示时间。

### 6. 命令系统

- `mainMenu(clientfd)`：解析第一个冒号前的命令，并通过函数表分派。
- `help(...)`：列出命令和格式。
- `addfriend(clientfd,str)`：发送加好友请求。
- `chat(clientfd,str)`：解析目标与内容，附加时间、单聊序列和消息 ID，先入 pending 再发送。
- `creategroup(clientfd,str)`：解析群名称和描述并发送。
- `addgroup(clientfd,str)`：发送加入群请求。
- `groupchat(clientfd,str)`：附加群序列和消息 ID，走 pending/ACK 流程。
- `loginout(clientfd,...)`：发送注销，退出主菜单并停止登录态心跳。

## 函数详细说明

### `sendFrame(int fd, const string &payload)`

函数先调用公共 `encodeFrame` 把 JSON 正文编码为“4 字节网络序长度 + payload”，再持有全局发送互斥锁循环调用 `send`，直到整个帧写完。一次 `send` 可能只接受部分字节，遇到 `EINTR` 也应继续，因此不能把返回值大于零就当作整帧成功。

发送锁覆盖完整帧而不是单次系统调用，可防止接收线程之外的重试线程、心跳线程和主菜单线程同时写 socket 时把两个帧的字节交叉。函数失败时返回错误，调用方据此保留 pending 或提示断线。

### `recvAll(int fd, void *buffer, size_t length)`

这是精确读取辅助函数。它维护已读取偏移，重复调用 `recv` 直到获得指定字节数；返回 0 表示对端正常关闭，负值且不是 `EINTR` 表示错误，二者都返回 false。该函数适合固定长度头和已知长度正文，但会阻塞当前接收线程直到数据齐全或连接结束。

### `recvFrame(int fd)`

函数先用 `recvAll` 读取 4 字节头，通过公共协议函数解码网络序长度并检查最大帧限制，再精确读取正文。它把 TCP 字节流恢复为一个完整 JSON 消息，天然处理拆包，也不会把粘在后面的下一帧误当成本帧正文。返回空串代表关闭或协议错误，因此当前协议若允许零长度业务帧，需要另设状态区分。

### `SnowflakeIdGenerator::SnowflakeIdGenerator(uint64_t workerId)`

构造函数设置节点位，并把输入限制在雪花算法预留的 10 位范围内。客户端稍后会用登录用户 ID 的低位更新 worker，使同一时刻不同用户生成相同 ID 的概率进一步降低。

### `SnowflakeIdGenerator::setWorkerId(uint64_t workerId)`

登录成功后调用该函数更新 workerId。修改过程持有生成器内部锁，避免重试或其他线程正生成 ID 时读到中间状态。截断到 10 位意味着用户数超过 1024 后 worker 会重复，真正的唯一性还依赖时间戳和序列；严格分布式部署应由中心或配置系统分配节点号。

### `SnowflakeIdGenerator::nextId()`

函数在锁内读取当前毫秒时间。如果本机时钟小幅回拨，就使用上一时间戳，保证生成结果不倒退；同一毫秒内递增 12 位序列，序列达到上限后等待下一毫秒。最后把相对 epoch 时间、workerId 和序列移位组合成 64 位整数。

这种实现保证单进程生成器内单调且不重复，但无法完全抵御进程重启、worker 冲突和长时间时钟回拨。项目把结果转成字符串传输，可避免 JavaScript 对大于 `2^53-1` 整数的精度丢失。

### `SnowflakeIdGenerator::nowMs()` 与 `waitNextMs(uint64_t lastTimestamp)`

`nowMs` 从系统时钟取得毫秒值；`waitNextMs` 在序列用尽时循环读取，直到时间超过上一毫秒。后者确保序列回零前时间位已经变化，但忙等会短暂占用 CPU，高吞吐客户端可改为更精细的等待或批量分配。

### `nextClientSeqForPeer(int toId)` 与 `nextClientSeqForGroup(int groupId)`

两个函数分别在互斥锁保护下维护单聊目标和群组的递增计数。序列按会话划分，而不是全客户端共用，因此一个繁忙群聊不会让其他会话出现大量无关缺口。接收端结合发送者与会话 ID 建立相同顺序域。

### `generateMsgId()`

调用雪花生成器取得 64 位 ID，再转换为十进制字符串。字符串形式既适合做 Redis 去重键，也能让 C++、Node.js 和浏览器之间无损传递。

### `addPending(const string &msgId, const string &payload)`

首次发送可靠消息前，函数在锁内同时保存原始 payload、最近发送时间和重试次数。先登记再发送能避免服务端极快返回 ACK，而接收线程查不到 pending 的竞态。若首次系统调用立即失败，调用方需要根据策略删除或保留记录。

### `removePending(const string &msgId)`

函数从 payload、时间和次数三张 pending 表同步删除同一 ID。成功 ACK 和 `ACK_DEDUP` 都表示服务端已经接受过这条业务消息，可以清除；保持三张表同步可避免重试线程访问到残缺状态。工程上可把三个字段封装成单个结构和一张 map，减少一致性负担。

### `retryTaskHandler()`

后台任务周期扫描 pending。消息超过 ACK 等待阈值且未达到最大次数时，函数更新重试次数和时间，并复制 payload 到待发送列表；超过上限则报告失败并清理状态。真正的 `sendFrame` 在释放 pending 锁后执行，避免网络阻塞阻塞 ACK 接收和新消息登记。

重试仍使用原 `message_id`，这是服务端能够识别重复投递的前提。若每次重试生成新 ID，ACK 丢失会造成对方重复收到消息。

### `heartbeatTaskHandler(int clientfd)`

登录态后台线程按固定周期发送包含当前用户 ID 的心跳。发送前检查上一轮心跳 ACK，未确认会输出连接异常提示；随后重置状态并发送新一轮。服务端用心跳续期 Redis 在线路由，客户端也借此发现“TCP 表面存在但业务链路已失效”的情况。

### `responseVersion()`、`waitForResponse(uint64_t previousVersion)` 与 `notifyResponse()`

这三个函数共同完成主线程和接收线程的响应同步。主线程在发送登录/注册请求前记录版本，随后在条件变量上等待版本增长；接收线程处理对应响应后递增版本并通知。条件谓词可处理虚假唤醒，也不会因为响应在 `wait` 前到达而永久错过。

版本表示“又收到一个需要唤醒主线程的响应”，如果未来允许多个并发登录类请求，还应使用 request ID 将响应与具体请求对应。

### `buildSessionKey(const json &js)`

函数为收到的聊天消息生成排序域。单聊通常以发送者 ID 为关键部分；群聊同时包含群 ID 和发送者 ID，因为不同发送者各自维护客户端序列。会话键设计必须与发送端序列生成规则一致，否则本来无关的消息会互相等待缺失序号。

### `formatIncomingLine(const json &js)`

把 JSON 中的发送者、时间、群 ID 和正文转换为用户可读的一行文本。格式化与排序分离后，排序缓存可以保存结构化消息，在真正输出时统一展示。读取可选字段时应使用默认值，避免旧客户端消息缺字段导致接收线程异常退出。

### `printOrderedIncoming(const json &js)`

没有 `client_seq` 的旧消息直接显示。带序列的消息先根据会话键取得状态：序号小于等于已输出值时视为迟到或重复并丢弃；更大的序号放入有序 map。随后从 `lastPrinted + 1` 开始连续取出并输出，直到遇到缺口。

该函数解决的是客户端观察到的会话内乱序。它不能改变服务端和网络真实到达顺序，也不能凭空恢复永久丢失的消息，所以还需要超时放行策略。

### `orderFlushTaskHandler()`

后台任务检查各会话排序缓存。若最小待输出序号前存在缺口，并且等待超过阈值或缓存达到容量上限，函数会把最小现有序号作为新的连续起点继续输出，避免一条永久缺失消息造成后续全部阻塞。输出动作和状态修改需要正确控制锁范围，防止接收线程同时插入导致迭代器失效。

这一策略在“严格不乱序”和“界面不永久卡住”之间选择了有界等待。生产 IM 通常还会向服务端请求缺失区间，而不是直接跳洞。

### `doRegResponse(const json &response)`

解析注册响应状态。成功时打印服务端分配的用户 ID，提醒用户保存；失败时展示错误码或错误信息。处理结束调用响应通知，使正在等待的菜单线程继续。

### `doLoginResponse(const json &response)`

登录失败时输出原因并保持未登录状态。成功时保存当前用户对象，将用户 ID 低位设置为雪花 worker，并解析服务端返回的好友、群组和离线消息列表。原项目把部分列表元素以 JSON 字符串嵌套在数组中，函数需要逐项再次解析。

离线聊天消息同样进入排序显示流程，而不是绕过实时消息规则。全部状态准备完成后再标记登录成功并唤醒菜单线程，避免主线程提前进入命令界面却读到半初始化数据。

### `readTaskHandler(int clientfd)`

这是唯一持续读 socket 的线程。它循环调用 `recvFrame`，解析 JSON 后按 `msgid` 分派：登录/注册响应交给专用函数；聊天消息进入会话排序器；`MSG_ACK` 根据状态清除或保留 pending；心跳 ACK 更新存活标记；统一错误消息负责展示。

由单一线程读取可以避免多个线程争抢字节流边界。解析和字段访问必须捕获异常，任何未处理异常都可能让接收线程退出，继而表现为客户端再也收不到 ACK。连接关闭后函数还应通知其他后台任务停止，并唤醒正在等待响应的主线程。

### `showCurrentUserData()`

打印登录用户基本资料、好友在线状态以及群组和成员信息，帮助用户在进入命令循环前了解可用对象。它只展示登录响应时获得的快照；好友上下线或群成员变化若没有增量推送，数据不会自动实时更新。

### `getCurrentTime()`

读取系统时间并用 `localtime_r` 转换为本地时间，再格式化为消息时间字符串。`localtime_r` 使用调用方提供的结构体，适合多线程客户端；普通 `localtime` 返回共享静态存储，可能被其他线程覆盖。

### `main(int argc, char **argv)`

程序入口解析服务器地址和端口，创建 TCP socket 并建立连接。连接成功后启动接收、重试、排序刷新等后台任务，然后进入注册/登录菜单；登录成功后再启动或激活心跳与业务命令流程。

它还负责进程级错误处理和退出。当前代码使用分离线程和全局状态，退出时往往依赖关闭 socket 或直接结束进程；更工程化的实现应使用可连接线程、原子停止标志和 RAII socket，在退出前按顺序停止生产任务、关闭连接并 join 所有线程。

### `mainMenu(int clientfd)`

函数读取用户输入，以第一个冒号前的文本作为命令名，剩余部分作为参数，再从命令函数表找到对应处理器。表驱动分派让新增命令只需注册名称和函数，避免长 `if/else`。未识别命令会提示帮助，输入退出或注销后离开循环。

### `help(int clientfd, const string &args)`

输出所有命令及参数格式，不访问网络。虽然签名与其他命令一致，`clientfd` 和参数可能未使用，这是为了能放入统一函数表。

### `addfriend(int clientfd, const string &args)`

解析好友用户 ID，构造包含当前登录用户 ID 和目标 ID 的加好友请求并发送。客户端输入校验只改善体验，服务端仍必须验证身份、目标合法性和重复关系，因为客户端数据不可信。

### `chat(int clientfd, const string &args)`

解析“目标用户 ID:消息正文”，生成会话内序列、时间和唯一 `message_id`，构造单聊 JSON。函数先调用 `addPending` 再发送，以防 ACK 先于登记到达；首次发送失败时输出错误，后续由策略决定重试。正文可能包含冒号，因此解析时应只切分第一个分隔位置。

### `creategroup(int clientfd, const string &args)`

从输入中解析群名称和描述，构造建群请求。名称/描述的长度与空值可在本地预检，但权限和数据库唯一性必须由服务端最终判断。

### `addgroup(int clientfd, const string &args)`

解析群 ID 并发送入群请求。当前命令通常没有等待独立响应刷新本地群列表，用户可能需要重新登录才能看到最新快照，是可以继续完善的交互点。

### `groupchat(int clientfd, const string &args)`

解析“群 ID:消息正文”，生成按群维护的客户端序列和消息 ID，再登记 pending 并发送。服务端会验证发送者身份，但若未验证群成员资格，非成员伪造群 ID 仍可能发送，因此权限检查必须落在服务端模型查询中。

### `loginout(int clientfd, const string &args)`

构造注销请求并发送，随后结束登录态命令循环、停止心跳或清理本地用户状态。请求中的用户 ID只是声明，服务端会与连接绑定身份核对；即使注销响应丢失，TCP 关闭后的异常清理也应释放当前节点路由。

## 面试重点

重要性高。常见问题：一次 `send/recv` 为什么不够、多个线程写同一 socket 如何防止帧交叉、ACK 丢失为什么不会重复投递、雪花 ID 如何处理时钟回拨、排序为何按会话而不是全局、序号洞为何要超时放行。要准确说明：这套机制提高到“至少一次重试 + 接收端去重”，不等于数学上的绝对不丢；客户端线程均 detach 且用 `exit` 退出，工程化版本还应增加统一生命周期和重连恢复 pending。

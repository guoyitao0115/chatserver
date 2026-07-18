# `app.js` 讲解

## 作用概览

这是无框架浏览器客户端，负责 WebSocket 连接、登录注册、会话与消息渲染、ACK 重试、心跳和好友/群组操作。状态集中在 `state` 对象，业务编号与 C++ `public.hpp` 对齐。

## 按学习顺序讲解

### 连接与协议

- `$(selector)`：`querySelector` 简写。
- `showToast(message)`：显示 2.4 秒提示，并重置上一次定时器。
- `setConnection(online,text)`：同步连接点颜色和文字。
- `socketUrl()`：按当前页面协议选择 `ws/wss`，固定路径 `/ws`。
- `connect()`：避免重复连接，绑定 open/message/close/error；收到消息解析 JSON，断线停止后台任务并延时重连。
- `send(payload)`：仅在 OPEN 状态序列化发送，否则提示并返回 false。
- `parseEmbeddedList(values)`：兼容服务端数组元素是 JSON 字符串或对象，并跳过损坏项。
- `handleMessage(message)`：按 `msgid` 分派登录、注册、聊天、ACK、心跳和错误。
- `handleLoginAck(message)`：建立用户/好友/群状态、切换视图、载入离线消息并启动后台任务。

### 会话与渲染

- `conversationKey(message)`：群聊按群 ID，单聊自动判断对端 ID。
- `activeKey()`：生成当前会话键。
- `storeIncoming(message,notify)`：以 `message_id` 做界面层去重，再保存、刷新或提示。
- `markAcknowledged(message)`：ACK_FAIL 保留 pending 并立即允许重试；成功/去重则移除 pending，更新气泡状态。
- `switchAuthTab(tab)`：切换登录/注册 UI 与 ARIA。
- `switchList(mode)`：切换好友/群组列表。
- `renderConversationList()`：用 DOM API安全创建列表；没有使用 `innerHTML`，昵称不会被当作脚本执行。
- `selectConversation(type,item)`：设置当前会话、解锁输入框并刷新。
- `renderMessages()`：创建空态或消息气泡；消息正文写入 `textContent`，防止存储型 XSS，并滚动到底部。
- `nextSequence(key)`：每个会话独立递增发送序号。
- `currentTime()`：格式化中文时分秒。

### 后台任务与事件回调

- 发送表单回调：构建单聊/群聊 payload，生成 UUID、序号，发送成功后加入本地消息和 pending。
- `startBackgroundTasks()`：每 10 秒心跳；每秒扫描 pending，5 秒超时、最多重试 3 次，最终标记失败。
- `stopBackgroundTasks()`：清理两个定时器并置空。
- 登录/注册表单回调：阻止默认刷新并发送请求。
- 退出回调：发送注销，清理用户、会话、消息和 pending 后回认证页。
- 新会话/dialog 回调：按当前列表模式展示字段，提交加好友、加群或建群请求。
- 文件末尾 `connect()`：页面加载后立即连接网关。

## 函数详细说明

### `$(selector)`

这是 `document.querySelector` 的简写，返回首个匹配元素。它只减少重复代码，不处理元素不存在的情况；本页面脚本依赖固定 HTML 结构，因此对应 ID 或 class 改名时要同步修改选择器。

### `showToast(message)`

函数把提示文字写入 toast 节点并切换可见样式。每次调用先清除上一次隐藏定时器，再启动新的 2.4 秒计时，避免短时间连续提示时旧定时器提前把新提示关掉。文字应通过 `textContent` 写入，不能把服务端错误直接塞进 `innerHTML`。

### `setConnection(online, text)`

根据 WebSocket 状态更新连接指示点、状态 class 和说明文字。它只负责视图，不改变 socket 本身；把状态渲染集中在一个函数中，可避免 open、close、error 回调显示不一致。

### `socketUrl()`

函数根据当前页面是 HTTP 还是 HTTPS 选择 `ws:` 或 `wss:`，复用当前 host 并固定 `/ws` 路径。这样开发环境、反向代理和不同端口部署无需在前端硬编码地址。HTTPS 页面必须使用 `wss`，否则浏览器会阻止混合内容。

### `connect()`

连接函数先检查已有 socket 是否处于 CONNECTING 或 OPEN，防止重连定时器和用户操作创建重复连接。随后创建 WebSocket 并注册 `open`、`message`、`close`、`error` 处理器。

open 时更新 UI；message 时捕获 JSON 解析错误并交给 `handleMessage`；close 时停止心跳和重试任务、清空当前 socket，并延时再次连接。error 主要更新提示，最终资源清理由 close 完成。当前重连只恢复传输连接，不自动恢复登录身份，用户仍需重新登录。

### `send(payload)`

发送前确认 socket 存在且 `readyState === WebSocket.OPEN`，再 `JSON.stringify` 后发送。成功返回 true，未连接时提示并返回 false，让表单回调决定是否加入 pending。浏览器 WebSocket 自己处理传输帧，前端不需要实现 mask 或长度编码。

### `parseEmbeddedList(values = [])`

服务端登录响应中的好友、群组和离线消息可能是 JSON 字符串数组，也可能已经是对象数组。函数逐项判断，字符串就再次 `JSON.parse`，对象直接保留；损坏项被捕获并跳过，避免一条脏历史记录阻止整个登录页面初始化。

这是对旧协议数据形态的兼容层。更干净的协议应直接返回结构化 JSON 数组，省去二次序列化和异常分支。

### `handleMessage(message)`

这是前端协议分发中心。它根据 `msgid` 区分登录响应、注册响应、单聊、群聊、消息 ACK、心跳 ACK 和统一错误。各分支只调用专用状态函数或展示提示，未知消息不会破坏连接。

分发编号必须与 C++ `public.hpp` 一致。字段来自网络，处理时要提供默认值并避免未经验证地调用数组方法；message 事件外层的异常捕获可阻止单个坏包终止后续收包逻辑。

### `handleLoginAck(message)`

登录失败时函数显示原因并停留在认证页。成功时保存当前用户，使用 `parseEmbeddedList` 恢复好友和群组，清理旧会话状态，切换到聊天视图并渲染用户资料与列表。离线消息逐条走 `storeIncoming`，从而复用实时消息的去重和会话归类逻辑。

全部基础状态建立后才启动心跳与 pending 扫描，避免后台任务读取半初始化的 user。若重复登录响应迟到，还应结合请求 ID 或当前视图防止旧响应覆盖新会话。

### `conversationKey(message)`

群消息使用群 ID 生成 key；单聊则比较消息发送者与当前用户，选择真正的对端 ID。发送和接收同一联系人因此都会落入同一个会话数组。生成规则必须稳定，否则同一个人的消息会被拆成两个列表。

### `activeKey()`

根据当前选择的会话类型和对象 ID 返回 key；没有选中会话时返回空值。渲染函数和发送表单都通过它访问状态，减少各处手写 key 造成的不一致。

### `storeIncoming(message, notify = true)`

函数先用 `message_id` 查询 `seenMessageIds`，已经出现过就直接返回，避免 ACK 丢失导致服务端重投时界面重复。新消息加入去重集合和对应会话数组；若该会话正在打开则立即重新渲染，否则可显示 toast 提醒。

没有 message ID 的旧消息只能按普通消息保存，无法可靠判断内容完全相同是重复还是用户真的发送两次。去重集合当前只在内存中且不淘汰，长时间运行可增加 TTL/LRU 或持久化策略。

### `markAcknowledged(message)`

函数用 ACK 中的 `message_id` 找到 pending 和本地气泡。`ACK_OK` 或 `ACK_DEDUP` 都代表服务端已接受过这条消息，因此删除 pending，并把 UI 状态改为已发送；`ACK_FAIL` 不把消息当成功，会调整最后发送时间或状态以允许重试，并展示失败提示。

`ACK_DEDUP` 常见于服务端已处理、原 ACK 丢失后的重发，它不是业务失败。正确区分这三个状态是“重试但不重复显示”的核心。

### `switchAuthTab(tab)`

在登录和注册面板之间切换 active class、隐藏属性与 ARIA 选中状态，同时清空上一次错误提示。函数让鼠标点击和可访问性状态保持同步，而不只是视觉换页。

### `switchList(mode)`

切换左侧展示好友或群组，保存当前模式并更新按钮状态，最后调用 `renderConversationList`。如果当前选中的对象不属于新列表，可保留右侧会话或按产品需求清空，当前行为应在 UI 上保持一致。

### `renderConversationList()`

函数根据 listMode 选择好友或群组数据，清空旧 DOM 后逐项创建按钮、头像、名称和状态。所有外部文本都写入 `textContent`，昵称即使包含 HTML 标签也只会显示为文字，避免 XSS。

每一项点击时调用 `selectConversation`。空列表会创建说明节点，而不是留下空白。频繁全量重绘在当前小数据量足够简单，数据规模扩大后可做 keyed diff 或虚拟列表。

### `selectConversation(type, item)`

保存当前会话类型与对象，更新标题和描述，启用输入框、发送按钮，并重绘列表选中态与消息区。状态先更新再渲染，确保 `activeKey` 能得到新 key。函数也可在此清除未读数。

### `renderMessages()`

函数取得当前会话数组并清空消息区。未选择会话或没有消息时显示空态；有消息时逐条创建发送方/接收方气泡、时间和 ACK 状态。正文和昵称使用 `textContent`，不执行用户输入中的标签或脚本。

渲染完成把容器滚动到末尾，保证最新消息可见。当前是每次全量重建 DOM，消息很多时会变慢；可以只追加新气泡、分页加载历史或使用虚拟滚动。

### `nextSequence(key)`

从 `state.sequences` 读取指定会话最后序号并加一后保存。每个会话独立计数，让服务端重投或网络乱序时接收端有可比较的顺序信息。页面刷新后计数归零，因此长期严格顺序还需要服务端序号或本地持久化会话游标。

### `currentTime()`

使用 `Intl.DateTimeFormat` 或本地时间方法生成易读时分秒，作为消息展示字段发送。客户端时间可能不准或被修改，不能用它做服务端安全判断或全局严格排序；可靠顺序依赖序列号和服务端接收/存储元数据。

### 发送表单 `submit` 回调

回调阻止浏览器默认刷新，确认已经登录、选中会话且正文非空。它生成 UUID 字符串作为 `message_id`，按会话取得 `client_seq`，构造单聊或群聊 payload。只有 `send` 成功进入浏览器 WebSocket 缓冲后，才把消息显示到本地并登记 pending，然后清空输入框。

本地先显示属于乐观 UI，最终状态由 ACK 更新。重试必须复用完全相同的 payload 和 ID；若重建消息或生成新 UUID，接收方可能看到重复内容。

### `startBackgroundTasks()`

启动前先调用停止函数，确保不会叠加多个 timer。心跳定时器在登录态按周期发送用户 ID，维持服务端 Redis 路由租约。重试定时器每秒扫描 pending：超过 5 秒且次数未满就原样重发并更新时间；达到上限后把气泡标记失败并移出自动重试集合。

遍历和修改 Map 时要保证逻辑不会漏项。断线期间不应无意义增加重试次数；当前 `send` 返回 false 时可以保留记录，等连接和登录恢复后再由更完善的会话恢复机制处理。

### `stopBackgroundTasks()`

分别 `clearInterval` 心跳和重试句柄并置为 null。函数可重复调用，供断线、注销和重新登录前统一清理。只清 timer 不等于清 pending，是否保留未确认消息取决于产品对重连恢复的设计。

### 登录表单 `submit` 回调

阻止默认提交，读取并校验用户 ID 与密码，构造 `LOGIN_MSG` 后调用 `send`。它不直接切换聊天页，而是等待 `handleLoginAck`，避免网络失败时前端出现伪登录状态。

### 注册表单 `submit` 回调

读取昵称和密码并发送 `REG_MSG`。前端可校验长度与确认密码以快速反馈，但 bcrypt、唯一性和数据合法性必须由后端再次执行。注册成功只展示新 ID，仍需用户登录。

### 退出按钮回调

登录状态下先发送 `LOGINOUT_MSG`，随后停止后台任务并清除用户、当前会话、消息、pending 和视图状态，返回认证页。即使请求发送失败，本地也退出；服务端会在 WebSocket/TCP 断开后执行异常连接清理。

### 新会话与 dialog 回调

打开对话框时根据当前好友/群组模式展示对应字段；提交时区分加好友、加入群和创建群，解析输入并发送相应 `msgid`。对话框只负责发起请求，服务端负责身份和业务合法性。成功后若协议没有返回增量对象，前端列表不会立即更新，可通过专用响应或重新拉取改进。

### 文件末尾 `connect()`

脚本加载并完成 DOM 查询、事件绑定后立即连接网关。由于脚本应放在页面底部或使用 `defer`，此时元素已经存在；若改成在 head 中同步加载，需要等待 `DOMContentLoaded`。

## 面试重点

重要性高。常见问题：前端重试为何不会造成重复消息、为什么 ACK_FAIL 不删除 pending、如何防 XSS、为什么会话序号不能全局共用、断线重连后状态如何处理。当前重连要求重新登录，pending 不做持久化；需要更强体验时可加 token 会话恢复与 IndexedDB。

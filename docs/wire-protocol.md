# RBP/3.0：按键与原生压缩音频协议

状态：0.5.0 固件及当前客户端已实现本协议；自动回归通过，真机验证待进行。旧协议仅在 archive 留作历史资料，不参与构建、协商或运行。

机器可读契约：[schema](../protocol/v3/schema.json)；字节向量与独立契约校验见 [protocol/v3](../protocol/v3/README.md)。管理、配对和按键保留原业务模型，音频改为编码单元与 decoder epoch。模型是一个活动蓝牙按键设备、零或一路压缩音频，不与 ATVV 绑定。

## 1. 面向客户端的功能

客户端只需理解：

1. 接收器信息、连接/电池/输入与语音能力。
2. 查找设备、选择配对、确认或输入口令、取消、解除绑定。
3. 查询按钮目录、接收完整逻辑按钮状态。
4. 启用语音交付，接收格式声明、编码音频和结束。
5. 错误、心跳、断线恢复。

GATT、CCCD、MTU、原始 HID usage 和设备控制字节是板内细节。上位机接收原生压缩有效载荷及公共格式/同步元数据，不提供 raw GATT 操作。诊断不能改变公共 opcode 的语义。

## 2. USB帧

运行时采用USB CDC ACM有序字节流：

    COBS(header + payload + CRC32C) + 0x00

COBS用于找回消息边界，CRC用于验证完整帧。每次串口read可能是半帧、多帧或空；不能把read返回当消息。

CRC32C参数：反射多项式0x82F63B78，init/xorout均0xFFFFFFFF，覆盖header+payload；ASCII 123456789的结果为0xE3069283。CRC以u32LE附在尾部。

32B固定header，所有多字节标量小端：

| offset | bytes | 字段 | 含义 |
|---:|---:|---|---|
| 0 | 2 | magic | 字节52 42（RB） |
| 2 | 1 | major | 3 |
| 3 | 1 | minor | 0 |
| 4 | 1 | kind | 1请求、2响应、3事件、4音频数据 |
| 5 | 1 | flags | 当前0 |
| 6 | 2 | header_size | 32 |
| 8 | 4 | session_id | 当前USB会话，HELLO请求为0 |
| 12 | 4 | tx_seq | 每方向实际发送序号，从1递增 |
| 16 | 4 | request_id | 请求/响应关联；事件/音频为0 |
| 20 | 2 | opcode | 操作类型 |
| 22 | 2 | status | 响应状态；其他kind为0 |
| 24 | 2 | payload_size | 0..512 |
| 26 | 2 | reserved | 0 |
| 28 | 4 | connection_id | 当前无线连接代次；接收器级操作为0 |

最大decoded长度548B，COBS含delimiter按552B接收空间配置。管理消息无通用分片；目录/候选分页，音频编码单元按第 8 节分片。

空delimiter忽略。累计非零字节超过551或半帧1000ms未完成，丢弃至下一个delimiter；已有会话同时失效。USB包边界不等于RBP边界，IN写入要处理64B整包结束/ZLP，OUT处理不能阻塞EP0。

先检查长度、magic、版本、kind、保留字段和CRC，再执行请求。损坏/过长帧丢到下个delimiter。有效会话中出现损坏帧或tx_seq间隙，接收方将会话视为不可信并停止交付；客户端释放自身输入、终止录音并用新HELLO恢复，不能仅略过错误继续宣称数据完整。

tx_seq在排队后、实际串行发送前分配，因此主动丢弃尚未发送的音频不会产生传输序号间隙。音频丢失由VOICE_ENDED的原因表达。

session_id与connection_id均非0且不在一次板子启动中复用；临近u32耗尽需受控重启。tx_seq/request_id在会话中不回绕，临近耗尽重新HELLO。持久peer_id在擦绑定后不复用同一已用ID（存储计数或新随机ID），不把新设备误当旧绑定。

非0约束仅针对有效身份；无会话/无连接和接收器级消息使用0。input_seq接近耗尽时终止当前逻辑连接并重建，不回绕；stream_id/frame_seq亦不在有效生命周期回绕。board_time_us为启动后的单调时间，睡眠补偿，重启才清零。

## 3. 管理消息的TLV编码

请求、响应及非固定结构事件采用扁平TLV；KEYS_STATE、KEYS_SNAPSHOT响应与VOICE_DATA用下文固定布局。空payload长度0，不编码空对象。

    tag:u8 | type:u8 | length:u16LE | value:length bytes

| type | 含义 | 必须长度 |
|---:|---|---:|
| 1 | u8 | 1 |
| 2 | u16LE | 2 |
| 3 | u32LE | 4 |
| 4 | u64LE | 8 |
| 5 | bool（0或1） | 1 |
| 6 | UTF-8 text，无末尾NUL | ≤96，字段可更小 |
| 7 | bytes | ≤480 |

不允许嵌套TLV、重复tag、非最短固定标量或多余尾字节。tag升序发送；接收未知tag跳过，缺失必填tag/类型错误拒绝。未知opcode返回UNSUPPORTED。未知枚举不能猜语义执行动作。

未知tag仍校验length不越界且不重复，未知type只可在未知tag中跳过。所有text按UTF-8字节数计限，拒绝非法UTF-8及内嵌NUL；用于显示时转义控制字符。列表count必须与记录数/尾部长度一致。客户端收到未知状态/结束原因时按不支持或异常处理，不能当OK/normal。

下文“tag:name:type”表示必填，“?”表示可选。列表采用bytes中的明确小记录结构，不引入CBOR/JSON/Protobuf依赖。所有字段相加仍需≤512B。数值范围以各操作定义为准。

## 4. 会话、请求与异步操作

客户端打开串口后发送HELLO。HELLO前板子不发送产品事件。串口打开/关闭不会擦绑定；DTR不作为物理复位。

HELLO请求session=0、connection=0、tx_seq=1，携带新随机nonce16。板子废弃旧USB会话及待发送产品数据，关闭旧语音交付，保留健康蓝牙连接，创建新session。HELLO响应echo nonce、新session且板发seq=1；客户端后续seq从2开始。旧nonce/旧session响应丢弃。HELLO成功后客户端先查询设备与按钮快照，再启用事件。

只有完整合法且版本支持的HELLO才替换会话。恢复端先写一个0x00，再写HELLO；板端停止旧队列，旧帧若已部分写入USB，须完成它或输出delimiter终止，然后才发新HELLO响应，不拼接两代字节。客户端在等待HELLO时允许丢弃残留坏帧/旧事件，仅匹配成功响应中的nonce后建立严格序号检查。

头布局正确且CRC有效的未知版本HELLO，可回当前头格式、session=0、seq=1、request_id=1、status=VERSION_MISMATCH，错误TLV另附3:echo_nonce:bytes16；不创建或破坏已有会话。无法识别头布局时不响应。当前会话中非HELLO的旧session消息丢弃，不刷新心跳、不执行。

请求ID由客户端从1单调递增，HELLO使用1。普通请求最多4个pending，耗时操作最多1个。新请求的ID必须大于已经见过的高水位；重复/旧ID回复DUPLICATE且不执行，不缓存重放副作用。查询必须使用新request_id。

短命令在1000ms内给最终响应。PAIR_BEGIN、FORGET_PEER为异步命令，正常即时返回ACCEPTED，operation_id等于该请求ID；之后通过OPERATION事件和GET_OPERATION查看结果。ACCEPTED不等于配对/删除成功。

板子保留当前USB会话的异步操作及最近4个完成结果，按完成时间淘汰；未保留结果返回NOT_FOUND。换会话后不以旧request_id查询旧操作，使用GET_PEER/GET_DEVICE确认持久状态。命令超时不自动重发，当前会话内使用GET_OPERATION(request_id)和GET_DEVICE/GET_PEER确认状态；无法确认时结果标uncertain。

客户端每1000ms发PING，其他合法当前会话请求同样刷新存活。5000ms无有效请求、USB断开或协议损坏时废弃会话、关闭语音交付、取消扫描和未完成配对；绑定/健康蓝牙连接可保留。取消不能撤销已提交的Flash操作，后续通过GET_PEER确认持久结果。

客户端用本机单调时钟判超时：PING超过1000ms无响应先标疑似故障，距离最后成功PING达到5000ms即本地清理并恢复；其他事件不能代替PING成功。USB reset/deconfigure/suspend也废弃会话；resume后重新HELLO，不补发睡眠期间输入。HELLO更换会话同样取消扫描与未完成配对，已受理Flash删除必须收敛到可查询的持久结果。

GOODBYE先停止本会话交付并清待发事件，回复OK后关闭会话。它不等于FORGET_PEER，不改变自动重连配置。客户端主动退出不等待旧语音正常完成，应本地标为中断。

### 响应status

| 值 | 名称 | 含义 |
|---:|---|---|
| 0 | OK | 短操作完成 |
| 1 | ACCEPTED | 异步操作已受理 |
| 2 | INVALID_ARGUMENT | 参数或TLV不合法 |
| 3 | UNSUPPORTED | 命令/设备/功能不支持 |
| 4 | BAD_STATE | 当前状态不允许 |
| 5 | BUSY | 操作进行中/设备占用 |
| 6 | NOT_FOUND | 目标或操作结果不存在 |
| 7 | TIMEOUT | 截止时间内未完成 |
| 8 | PAIRING_FAILED | 配对/加密失败 |
| 9 | LINK_LOST | 连接失效 |
| 10 | STORAGE_FAILED | 绑定或配置持久化失败 |
| 11 | RESOURCE_LIMIT | 内存/容量不足 |
| 12 | CANCELLED | 用户或会话取消 |
| 13 | DUPLICATE | 请求ID重复或过旧，未执行 |
| 14 | SESSION_MISMATCH | USB会话不匹配 |
| 15 | DEVICE_ERROR | 适配器/设备错误 |
| 16 | VOICE_UNAVAILABLE | 无语音或语音未就绪 |
| 17 | VERSION_MISMATCH | 协议不兼容 |

错误响应TLV：1:message:text?（≤96B）、2:uncertain:bool。错误码是公共产品错误，不将ATT/SMP私有码作为客户端判断依据；支持人员可根据固件日志进一步定位。

## 5. 接收器与设备信息

这些命令header.connection_id=0；不要求调用方预先知道当前设备。

| opcode | 名称 | 请求TLV | 成功响应 |
|---|---|---|---|
| 0x0001 | HELLO | 1:nonce:bytes16 | 1:echo_nonce:bytes16、2:bridge_uid:bytes16、3:firmware:text≤32、4:max_keys:u8、5:max_payload:u16、6:features:u32 |
| 0x0002 | PING | 1:cookie:u32 | 1:cookie:u32、2:board_time_us:u64 |
| 0x0003 | GET_DEVICE | 空 | DeviceInfo（下述） |
| 0x0004 | GET_PEER | 空 | 1:peer_id:u32、2:name:text≤48、3:auto_reconnect:bool |
| 0x0005 | GET_OPERATION | 1:operation_id:u32 | Operation（下述） |
| 0x0006 | GET_STATS | 空 | 1:protocol_errors:u32、2:input_resets:u32、3:voice_overruns:u32、4:voice_errors:u32、5:reset_reason:u16、6:fault_sequence:u32、7:fault_records:bytes、8:fault_evictions:u32 |
| 0x0007 | GOODBYE | 空 | 空 |

HELLO max_payload本版固定512，max_keys≤64。features bit0=压缩音频交付能力、bit1=交互配对能力、bit2=绑定保存、bit3=自动重连；其他bit=0。bit0 表示此固件有压缩音频交付能力，不代表当前设备voice_ready。本版只支持 major3/minor0；后续minor向后兼容时，由客户端使用自己支持的minor发HELLO，板子支持则沿用该minor，否则VERSION_MISMATCH，不按旧版解析。

GET_PEER无绑定时仍OK，peer_id=0、name为空、auto_reconnect=false。GET_DEVICE初始无型号时model_id/name为空、catalog_revision/key_count=0、sample_rate=0、battery=255、charging=0。响应回显对应请求opcode/request_id/connection_id；HELLO成功响应session例外为新会话。事件的connection按其各自上下文填写。

GET_STATS的reset_reason为产品枚举：0未知、1上电、2软件重启、3watchdog、4掉电复位、5内部故障。计数器按板子启动以来累计，饱和于u32最大值；不是协议序列号，不能回绕后误读为“从未出错”。

DeviceInfo：

| tag | 字段 | type | 含义 |
|---:|---|---|---|
| 1 | connection_id | u32 | 无链路时0；事件仍有旧connection上下文 |
| 2 | peer_id | u32 | 无绑定时0 |
| 3 | state | u8 | 见状态表 |
| 4 | model_id | text≤40 | 型号适配稳定ID，例如xiaomi.rc003，无设备为空 |
| 5 | name | text≤48 | 可读名称 |
| 6 | catalog_revision | u32 | 当前键目录版本，未就绪0 |
| 7 | key_count | u8 | 0..64 |
| 8 | voice_state | u8 | 0不存在、1初始化、2可用、3不支持、4故障 |
| 9 | sample_rate | u32 | 当前已知解码输出采样率，否则0；实际格式以 START/FORMAT 为准 |
| 10 | battery | u8 | 0..100；255未知 |
| 11 | charging | u8 | 0未知、1未充电、2充电 |
| 12 | voice_enabled | bool | 当前USB会话是否允许交付下一完整录音 |
| 13 | delivering | bool | 当前USB会话是否存在活动语音交付 |
| 14 | stream_id | u32 | 无活动交付为0 |
| 15 | waiting_idle | bool | 已启用但需等设备结束现有录音才能接下一句 |
| 16 | reason | u16 | 当前状态的公共status，正常为OK |
| 17 | message | text≤96，可选 | 可读原因，不含私有协议字段或配对口令 |
| 18 | voice_interaction | u8 | 0不可用、1按键请求开始、2按一下开始需主动停止、3按住说话；产品语义，非ATVV枚举 |
| 19 | max_capture_ms | u32 | 固件每次录音上限，当前基线120000；无语音0 |

state：0 unbound、1 disconnected、2 connecting、3 pairing、4 initializing、5 ready、6 unsupported、7 error。ready以逻辑按钮初始化完成为准，语音看独立voice_state。

Operation TLV：1:operation_id:u32、2:state:u8（0 pending、1 completed）、3:result:u16（pending时ACCEPTED，完成后为OK或公共错误）、4:peer_id:u32、5:connection_id:u32、6:uncertain:bool。

## 6. 查找与配对

无线扫描、候选识别、连接安全参数和绑定流程由固件管理。客户端只选择产品候选并回答用户交互。一个已绑定peer满槽时PAIR_BEGIN返回BUSY，要求先明确FORGET_PEER，不自动覆盖。

| opcode | 名称 | 请求（connection=0） | 成功响应 |
|---|---|---|---|
| 0x0100 | FIND_START | 1:duration_ms:u16（1..30000） | 1:search_id:u32 |
| 0x0101 | FIND_LIST | 1:search_id:u32、2:cursor:u8 | 1:next_cursor:u8、2:entries:bytes |
| 0x0102 | FIND_STOP | 1:search_id:u32 | 空 |
| 0x0110 | PAIR_BEGIN | 1:search_id:u32、2:candidate_id:u32 | ACCEPTED + 1:operation_id:u32 |
| 0x0111 | PAIR_REPLY | 1:operation_id:u32、2:prompt_id:u32、3:accept:bool、4:passkey:u32? | 空 |
| 0x0112 | PAIR_CANCEL | 1:operation_id:u32 | 空 |
| 0x0113 | FORGET_PEER | 1:peer_id:u32 | ACCEPTED + 1:operation_id:u32 |
| 0x0114 | SET_RECONNECT | 1:peer_id:u32、2:enabled:bool | 空 |
| 0x0115 | CONNECT_PEER | 1:peer_id:u32 | 空 |
| 0x0116 | DISCONNECT | 空 | 空 |

FIND只可在无活动连接、无耗时操作时开始，避免打断已有设备录音。搜索最多保留8个候选，按candidate_id递增，插入后slot不移动；同设备刷新名称/信号质量，不重复占槽。FIND_START会替换旧搜索并使旧search_id/candidate_id失效。

FIND_LIST cursor为0..7索引，next_cursor=255表示当前已到末尾。扫描中列表仍可能增长，UI可再次从0拉取；FIND_DONE后目录冻结。候选只在该搜索结束后60秒内有效；连接时固件检查可达性，不能凭陈旧广播建立错误身份。

entries结构：count:u8，随后count个记录：
candidate_id:u32LE、support:u8（0可能支持、1明确型号匹配）、signal:u8（0未知、1弱、2中、3强）、name_len:u8、name UTF-8（≤48B）。每页最多8项且总payload≤512。只列出固件有匹配依据的候选；无依据设备不开放raw接入。

PAIR_BEGIN自动停止搜索，依次连接、安全配对/绑定、设备适配初始化，60秒总截止。PAIR_PROMPT可请求确认/输入；超时或拒绝中止本次操作。结果可能为“已绑定但适配失败”，此时Operation包含非0 peer_id和DEVICE_ERROR/UNSUPPORTED，UI允许解除绑定后换设备，不伪称完整成功。

PAIR_REPLY中的passkey仅对输入口令方法必填，范围0..999999（UI显示六位，允许前导零）。其他方法携带passkey为INVALID_ARGUMENT。prompt_id过期或不匹配拒绝，用户回答不等于最终配对成功。

PAIR_CANCEL受理后异步操作以CANCELLED结束；若Flash已提交，保留真实peer状态并通过Operation/GET_PEER报告，不能说“取消=从未绑定”。

FORGET_PEER关闭自动重连并断开目标设备，持久删除完成才发Operation OK。SET_RECONNECT相同值不重复写Flash；成功仅在持久化完成后返回。新绑定默认auto_reconnect=true，固件重启可自动连接该peer。

FORGET_PEER最多5000ms完成或给出失败/uncertain；已提交删除不能因USB断开回滚成成功绑定。存储层必须具备掉电恢复状态，操作结果与GET_PEER一致。加密失败时不无限清bond/重新配对；保留绑定并返回PAIRING_FAILED，让用户明确解除后重配。

CONNECT_PEER请求一次连接尝试并立即OK，结果通过DEVICE_STATE；总尝试20秒。已有连接返回BAD_STATE；若同peer已在重连则返回OK而不重复调度。DISCONNECT立即撤销当前连接尝试/本轮重连，断线结果走DEVICE_STATE；本次USB供电期间暂停自动重连，持久配置不变，CONNECT_PEER或重新启用SET_RECONNECT解除暂停。

自动重连退避1/2/5秒循环；成功后由固件重新完成适配初始化。无需客户端重新订阅GATT。

### 配对及产品状态事件

以下kind=EVENT、request_id=0，无需EVENTS_ENABLE也发送到有效USB会话：

| opcode | 名称 | payload |
|---|---|---|
| 0x0180 | FIND_DONE | TLV 1:search_id:u32、2:reason:u8（0到期、1用户停止、2配对接管） |
| 0x0181 | PAIR_PROMPT | TLV 1:operation_id:u32、2:prompt_id:u32、3:method:u8、4:number:u32?、5:remaining_ms:u32 |
| 0x0182 | OPERATION | Operation TLV |
| 0x0183 | DEVICE_STATE | DeviceInfo TLV |

PAIR_PROMPT method0 confirm_pair、1 enter_passkey、2 confirm_number、3 display_passkey。number在2/3必填，其他不发送；方法3仅通知显示，用户在远端输入，客户端不必PAIR_REPLY，取消走PAIR_CANCEL。方法0表达用户授权本次配对，不等同MITM认证。无法完成所需安全方式的固件返回UNSUPPORTED，不暗中降级安全要求。

DEVICE_STATE用于连接、初始化、断线、电池、语音能力或交付状态变化；变化合并应保留故障/断线边界，不能只留下“最终ready”。事件header.connection_id为其上下文；断线事件用旧connection_id，payload连接字段为0。GET_DEVICE响应header=0，payload包含当前值。

PAIR_BEGIN等响应必须先于其PAIR_PROMPT/OPERATION事件；PAIR_BEGIN成功完成的最终OPERATION在初始化/状态事件之后发出。查询GET_OPERATION可恢复丢失通知。

## 7. 按钮目录与状态

以下请求header.connection_id必须等于当前已就绪连接，否则LINK_LOST/BAD_STATE。按钮目录在同一connection中不可改变；profile变化需新connection_id。

| opcode | 名称 | 请求 | 成功响应 |
|---|---|---|---|
| 0x0200 | KEY_CATALOG | TLV 1:cursor:u8 | TLV 1:revision:u32、2:next_cursor:u8、3:entries:bytes |
| 0x0201 | KEYS_SNAPSHOT | 空 | 下述24B固定状态结构 |
| 0x0202 | EVENTS_ENABLE | TLV 1:enabled:bool | 空 |
| 0x0280 | KEYS_STATE（EVENT） | — | 下述24B固定状态结构 |

catalog entries为count:u8，随后count个记录：
slot:u8（0..63）、key_id:u16LE、name_len:u8、name UTF-8（1..32B）。按slot排序，slot连续0..key_count-1；cursor按slot开始，next_cursor=255结束。每页最多12项且≤512B。相同model_id的key_id语义永久稳定；slot可随revision调整。

公共key_id：

| ID | 语义 | ID | 语义 |
|---|---|---|---|
| 0x0001 | Power | 0x0002 | Voice |
| 0x0003 | Up | 0x0004 | Down |
| 0x0005 | Left | 0x0006 | Right |
| 0x0007 | OK | 0x0008 | Back |
| 0x0009 | Home | 0x000A | Menu |
| 0x000B | TV | 0x000C | VolumeUp |
| 0x000D | VolumeDown | 0x000E | Mute |
| 0x000F | PlayPause | 0x0010 | Next |
| 0x0011 | Previous | 0x0012 | Stop |

0无效，其他未分配公共ID保留，0x8000..0xFFFF为model_id范围的扩展按钮。不保留手柄映射需求。客户端不认识key_id时按名称显示，仍可自定义映射。同一目录key_id不能重复。Voice仅在有可靠完整按下/松开来源时列出；无法观察的键不造假，语音业务可绑定独立START/END事件。

24B状态结构：

| offset | bytes | 字段 |
|---:|---:|---|
| 0 | 4 | input_seq，本连接每次状态变化/重置递增，从1起 |
| 4 | 8 | captured_us，板端单调微秒时间 |
| 12 | 8 | pressed_bits，bit slot为1表示按下 |
| 20 | 1 | kind：0当前快照、1物理变化、2失同步重置 |
| 21 | 1 | reason：0无、1断线、2输入队列溢出、3适配器错误 |
| 22 | 2 | reserved=0 |

未使用slot的bit必须0。kind0不制造新物理变化，重复快照保留input_seq与原状态captured_us；kind2 bitmap必须0且序号递增，表示必须释放本来源输入。

初始化input_seq=1、pressed_bits=0、captured_us=本连接输入初始化时刻。溢出reset占一个新序号，恢复真实当前状态再占下一个序号并以kind0发出；避免客户端按序号去重误吞恢复快照。状态快照会标记当前实际持有键为“等待松开”，不触发动作。

EVENTS_ENABLE(true)先OK，再立即发kind0当前快照，然后传后续变化；允许重复true重新取快照。false先停交付并清待发KEYS_STATE，再OK。客户端关闭交付必须本地释放自身输入。连接断开自动禁用，重连后需重新获取目录/启用产品事件，不涉及蓝牙细节。

KEYS_SNAPSHOT响应与事件可能交错，客户端按input_seq忽略更旧状态。客户端刚订阅时已按下的键只作为当前状态展示，默认等待这些键松开后再响应新按下，避免应用启动时意外执行动作。应用可显式选择其他策略。

正常变化不能为了只保留最新状态而丢弃中间短按。输入独立保留8条状态队列；溢出时清该队列，先发kind2 reset，再发kind0真实当前状态。客户端不为缺失期间重放tap/hold；已持有键等待松开重置。

对单次KEYS_STATE的内部变化，先全部up再全部down，组合判断基于最终完整状态。重复无线报告不重复生成down；自动重复/长按阈值由客户端决定。

## 8. 原生压缩音频交付

本节完整规范在 [RBP/3.0 编码音频契约](audio-wire-v3.md)，为本文不可分割的规范性部分：能力协商、START/FORMAT 的 decoder epoch、DATA 编码单元分片、END 计数、丢失策略与 codec registry。不能沿用 RBP/2.0 的 PCM 固定布局。

## 9. 有界队列与调度

重连语义：VOICE_ENABLE 成功后的消费意图按 USB session 和已绑定 peer 保存。意外 BLE 断开结束旧流、令当前 voice_enabled=false；同一 peer 恢复且 codec 接受配置有效时自动恢复启用。false 可携带 connection=0 或当前连接，在未 READY 时撤销该意图；true 仍要求当前 READY 连接。主动 DISCONNECT、换绑和 session 结束清除意图，详见 audio-wire-v3.md。按钮 EVENTS_ENABLE 仍按原规则在重连后重新启用。

不开放channel/credit/window操作。客户端保持专用读取循环，不在读取线程做ASR/GUI/文件对话框。USB自身有流控，但无线音频源未必能被反压，固件必须有界缓冲并显式处理溢出。

建议数据资源：8条按键状态、有界压缩音频字节池、8条控制/生命周期元数据槽。具体内存见架构预算。队列使用引用或固定池，不能为每次按键/音频分配无界heap对象。

调度先控制响应/配对/故障，再按键，再媒体小块；**媒体组内START/DATA/END顺序不可打乱**。消费者禁用/断线属于明确丢弃边界，可清待发压缩数据再END；正常END必须等待已排完整编码单元发送。出现输入/媒体错误时要保留至少一个终止通知槽。

控制/故障槽也耗尽：会话失效、关闭语音交付，停止产品发送直到HELLO。客户端通过心跳响应失败恢复；不能静默继续健康状态。

客户端自身编码音频/解码处理队列溢出同样结束本地录音并尽快VOICE_ENABLE(false)，不静默丢样本继续保存为正常WAV。串口接收与业务处理分离，GUI冻结不得阻塞配对响应/心跳。

异常清队列时一并移除未发送FORMAT；已发送FORMAT允许对应零样本后END。单个RBP帧一旦开始发送只能完整完成，不能半途被高优先级消息插入。连续控制流不得无限饿死媒体：每轮最多4条普通控制后给输入/媒体各一次发送机会；生命周期结束优先但仍遵守组内顺序。资源不足以满足时显式失败，不阻塞无线回调。

## 10. 版本与恢复契约

同major的minor可以增加可选TLV tag/新opcode/公共key_id，不能改旧固定结构、必填字段、已登记 codec profile 或默认操作语义。更改这些内容必须升major。未协商的新功能不得发送。

重连流程：

    打开CDC → HELLO → GET_DEVICE/GET_PEER
      → 已就绪：KEY_CATALOG分页 → EVENTS_ENABLE(true) → VOICE_ENABLE(true)
      → 未绑定：FIND_START/LIST → PAIR_BEGIN → 回答PAIR_PROMPT → OPERATION完成
      → 重连中：等待DEVICE_STATE就绪，再获取目录并启用输入/语音

故障本地清理必须先于自动重试：释放本来源的合成按键、取消tap/hold计时器、将录音标记interrupted。只清本应用注入记录，不清真实键盘的所有全局键。

固件升级由板型对应物理BOOT/ISP流程完成，首版无公共远程刷写命令。开发日志/原始抓包使用单独调试构建或调试接口，不作为普通demo的raw inspector。

## 11. 冻结前验证

需要C与独立Python实现的golden vectors：HELLO、TLV未知tag、目录分页、64位按钮位图、24B输入结构、压缩单元、分片、decoder 配置、尾单元、各END原因、PAIR_PROMPT各方法与操作结果。

要验证：半包/黏包、COBS/CRC错误、过长payload、重复tag、请求重复、过期prompt、旧session/connection、启用时正在按键/录音、normal END尾音次序、音频溢出、按键溢出、Flash失败、USB拔插与输入释放。

这些是后续实施验收要求；本轮只完成协议设计审查，不把草案当成已验证协议。

## 12. RBP/2.0 迁移

帧 major=3，HELLO 只协商 3.0。旧 2.0 不自动回退、不猜测音频；双方必须拒绝不兼容会话。按键/配对 opcode 虽保持，仍不可跨 major 混用。迁移必须同步覆盖固件、客户端、demo、模拟器和向量；旧实现的编译成功/测试通过不证明支持新协议。见 [迁移与验收清单](validation-plan.md)。


## 主机主动开始录音（0.6.0）
VOICE_START opcode=0x0303，请求/OK 响应均为空，header 使用当前 connection_id。
需要当前 READY、VOICE_READY，且本 USB 会话已通过 VOICE_ENABLE(true) 协商接收格式。
已有源流、正在交付/结束旧流、等待旧录音结束或已有待启动请求时返回 BUSY；
未启用返回 BAD_STATE；链路不匹配返回 LINK_LOST；适配器未实现返回 UNSUPPORTED。
OK 仅表示启动请求已在板子排队，不能作为实际开始的证据。实际音频通过原 VOICE_STARTED/FORMAT/DATA/ENDED 链路交付。
远端拒绝/启动超时走设备故障状态；RC003 对结果不确定的开麦尝试终止 bearer 后恢复绑定连接，不自动重发该请求。
客户端请求超时不得自动重复 VOICE_START。没有 stream_id 前，取消用 VOICE_ENABLE(false)；开始后可用 VOICE_STOP(stream_id)。
会话结束或关闭接收取消尚未发送的启动；已发送的启动进入等待关闭路径，防止迟到 START 继续录音。
VOICE_START 不等于 VOICE_ENABLE，不改变绑定/自动重连，也不暴露 ATVV 命令。
分段由上位机安排：上一段正常 ENDED 后再申请，使用新的 stream_id、独立格式与初始编码状态，保留真实间隙，不宣称无缝。


### Release 原始错误记录（0.6.5）

GET_STATS 的 tag 6–8 是常规诊断字段，不依赖 DEBUG。tag 7 最多包含 4 条记录，按从旧到新排列，每条 24 字节、小端：

| 偏移 | 类型 | 内容 |
| --- | --- | --- |
| 0 | u32 | sequence，最后一次出现的序号 |
| 4 | u16 | domain，错误来源 |
| 6 | u16 | stage，API/协议阶段 |
| 8 | u32 | code，原始返回码，不截断为 u8 |
| 12 | u32 | context，参数 ID/事件类型等元数据 |
| 16 | u32 | count，连续相同记录的出现次数，饱和累计 |
| 20 | u32 | board_ms，最后一次出现时的板端毫秒时钟 |

来源：1 SDK、2 GAP、3 GATT API、4 GATT event、5 ATT、6 ATVV、7 storage、8 adapter。具体阶段见 release-errors-0.6.5.md。业务响应码仍表示产品级结果，不能代替这里的原始码。

tag 6 为当前序号，初始 0，递增回绕时跳过 0；判断变化使用不等比较。连续相同来源/阶段/码/上下文合并，更新序号、时间、次数。tag 8 是被覆盖的记录条数（不是发生次数），饱和于 u32 最大值。此历史不因读取、USB 会话重建而清除，板子重启后清零；必须持续采集才能保留长期历史。board_ms 可回绕，启动时主循环尚未更新时间的记录为 0；它不是墙上时钟，也不是中断级精确时间。

这些记录是诊断事实，不全部代表操作失败：例如断链原因或最终重试成功前的资源不足。主机不能仅凭记录存在就断开会话。此接口不上传密钥、原始 BLE 报文或音频内容。

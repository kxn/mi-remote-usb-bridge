# RBP/3.0 编码音频契约

本文件是 [公共协议](wire-protocol.md) 第 8 节的规范性组成部分。当前版本 0.6.5 已实现；验收范围见 validation-plan.md。

## 1. 职责

板子交付设备原生压缩音频，不解码、不转码、不重采样。适配器剥离设备封装，保留编码有效载荷，并把初始化、格式与同步信息转为本契约。GATT、ATVV 控制码、CCCD 不暴露给客户端。

编码标识与设备协议无关。首个适配器仍为 RC003/ATVV 1.0，首个编码为下文 IMA profile；未来其他设备可以复用编码或登记新的编码。此接口不意味着 CH582F 支持 Classic Bluetooth、任意手柄或耳机。原生 PCM 源不属于本版压缩交付能力，不在板上为它临时编码。

编码单元是 decoder 所需的输入边界。包式编码保留完整 codec 包边界；字节流编码可以按 profile 切成有界片段。它不是 USB 包或 GATT notification 的通用同义词。大单元可跨多个 RBP 帧，板子无需先缓存整个大包。

## 2. 能力与启用

| opcode | 请求 | 成功响应 |
|---|---|---|
| 0x0300 VOICE_ENABLE | 1:enabled:bool；true 还必须带 2:accepted_codecs:bytes、3:max_unit_bytes:u32 | 1:enabled:bool、2:waiting_idle:bool |
| 0x0301 VOICE_STOP | 1:stream_id:u32 | 空，最终结束看 ENDED |
| 0x0302 GET_VOICE_CAPS | 空 | 1:codecs:bytes、2:max_unit_bytes:u32、3:config_max_bytes:u16=256 |

以上请求使用当前 connection_id。codec 列表为 `(codec_id:u32LE, codec_revision:u16LE)` 连续数组，无 count 前缀；1..16 项，长度是 6 的倍数，按 id/revision 严格递增且禁止重复。GET_VOICE_CAPS 在无可用语音时允许空列表与 max_unit_bytes=0；不伪造初始化未完成的能力。

VOICE_ENABLE(true) 要求 voice_state=ready，接受列表与设备支持列表至少有一项相交，否则 VOICE_UNAVAILABLE 或 UNSUPPORTED，不改变原配置。max_unit_bytes=1..65536，是主机愿意接收/重组的单元上限，不是板上缓冲大小；已知设备要求超过该值则 UNSUPPORTED。这不是转码或采样率协商。

同参数重复 true 幂等。delivering 或 waiting_idle 期间变更配置返回 BUSY；先 disable 收口。false 不带 tag 2/3，清除配置；新 USB 会话也必须重新协商。已在录音时首次启用只等待下一次完整录音，不能从任意 ADPCM 中间开始。

同一 USB 会话、同一已绑定 peer 的意外 BLE 断链保留此前成功启用的消费意图及 codec 接受列表；断链时 `voice_enabled=false`，恢复语音能力且接受列表仍匹配后自动变为 true。当前音频流仍以 link_lost 结束，恢复时创建新流，不续接旧流。禁用、主动 DISCONNECT、换绑及 USB 会话结束撤销恢复意图。VOICE_ENABLE(false) 允许在未 READY 时撤销意图，connection_id 为 0 或当前连接；过期非零连接仍拒绝。true 仍必须指定当前 READY 连接。客户端应以 DEVICE_STATE 的 voice_enabled 同步当前状态。

列表、长度或范围非法返回 INVALID_ARGUMENT。GET_VOICE_CAPS 的 max_unit_bytes 是当前适配器可能交付的最大单元，主机必须能接收它；不是 USB 片段长度。能力随连接变化，重连须重新查询，不把旧设备接受集合套给新设备。

设备运行中出现未接受的 codec/revision、超过 256 B 的配置、超过协商值的单元，结束该 stream，reason=unsupported_format；不切回 PCM、不猜测 codec。按键会话继续。主机仍须校验配置，不能仅因 id 在接受列表就调用 decoder。

若还未发出公共 START 就发现上述问题，不发不存在的 stream 的 END；报告 DEVICE_STATE 的 voice_state=unsupported 及 reason=VOICE_UNAVAILABLE，并请求停止该次源录音。已有交付时先 END 再报告状态；final_epoch 仍是最后合法声明值。恢复能力须由适配器重新确认，不无限重试同一不支持格式。

## 3. START、FORMAT 与 decoder epoch

VOICE_STARTED=0x0380、VOICE_FORMAT=0x0383，kind=EVENT。两者使用相同扁平 TLV，全部必填：

| tag | 类型 | 字段 |
|---:|---|---|
| 1 | u32 | stream_id，非零且会话内不复用 |
| 2 | u32 | epoch，START=1，每次 FORMAT 加 1，不回绕 |
| 3 | u32 | codec_id |
| 4 | u16 | codec_revision，非零、精确匹配 |
| 5 | u32 | sample_rate，输出 sample frames/秒，1..384000 |
| 6 | u8 | channels，1..8 |
| 7 | bytes | codec_config，0..256 B，语义及合法长度由 profile 定义 |
| 8 | u32 | max_unit_bytes，1..主机协商上限 |
| 9 | u64 | first_sample_index，epoch 起点；START=0，未知为 UINT64_MAX |
| 10 | u64 | captured_us，板端识别起流/配置边界的单调微秒时间 |
| 11 | u32 | first_unit_seq，下一单元编号；START=1 |

sample frame 指每声道各一个样本，不是 interleaved 样本元素总数。captured_us 不保证对应麦克风采样硬件时刻，不能用 USB 到包间隔恢复采样时间轴。

如果公共 START 因等待配置而延迟，captured_us 仍记录实际识别源起流的时间，不能改成 USB 发出时间。单调时间与标识只在其板子启动/USB 会话上下文中解释。

公共 START 必须有可信的完整 decoder 初始化配置。设备 START 已到但 seed 未知时，板端 source 已活动，公共 START 延迟到可信 SYNC；等待受首音频 1000 ms 超时约束。配置未明却先来音频，禁止输出猜测数据，报告设备语音故障；尚无公共 START 时不制造无身份的 END。已有 START 则以 END 收口。

每次 FORMAT 是 decoder 重建边界，即使 rate/codec 不变也成立。它携带新的完整配置，不是增量补丁。前一单元必须完整结束，不能在分片中插入 FORMAT。stream_id、unit_seq、累计 sample index 不归零；重建的是解码状态。实际发生可信状态重置必须通知上端，不能只在采样率变化时发送。

同步信息表明源计数缺失时，首版结束 source_data_lost，不能用 FORMAT 掩盖；没有可信源序号时只能说未检测到缺口，不能证明空口无丢包。主机看到未知 codec/revision/config 时标记本地 unsupported_format，异步 disable，继续维护健康的按键/控制会话。

## 4. DATA 与媒体分片

VOICE_DATA=0x0381，kind=AUDIO(4)。固定头 40 B，之后 1..472 B 原生编码片段，所有整数小端：

| offset | bytes | 字段 |
|---:|---:|---|
| 0 | 4 | stream_id |
| 4 | 4 | frame_seq，每个 DATA 从 1 连续递增 |
| 8 | 4 | epoch，必须已声明 |
| 12 | 4 | unit_seq，每个完整单元从 1 连续递增 |
| 16 | 4 | unit_size，完整单元压缩字节数，1..max_unit_bytes |
| 20 | 4 | fragment_offset，同单元从 0 连续递增 |
| 24 | 8 | first_sample_index，单元输出起点；未知 UINT64_MAX |
| 32 | 4 | unit_sample_count，整个单元输出 sample frames；未知 UINT32_MAX |
| 36 | 4 | reserved=0 |
| 40 | 1..472 | 原始编码有效载荷片段 |

data_length=payload_size-40。offset+length 必须无整数溢出且 <=unit_size；到达 unit_size 才完成单元。禁止片段重叠、间隙、单元交错。一个单元所有片段的 epoch/unit_seq/size/index/count 必须相同。FORMAT 不重置 frame_seq/unit_seq；它们不允许回绕。

索引已知时等于之前完整单元累计输出帧数。允许 profile 明确规定的零输出单元使用 count=0；未知不是 0。一旦出现未知 count，后续累计索引及 END 总样本数保持 UNKNOWN，不能根据墙钟或编码字节数猜测。主机可独立统计真实解码样本数，不能将其冒充板端声明。

包式 decoder 等完整单元才解码；字节流 decoder 可按 profile 增量解码。所有重组必须有界，不因设备声明的 size 无限分配。管理消息依然不支持通用分片；媒体分片不改变 COBS/CRC/USB 帧规则。

主机单元重组采用 2 秒无进展超时和 10 秒总时限，任一超时标记本地 incomplete_unit，清理该次录音并 disable；USB 心跳不能延长音频重组期限。对于需要更长单元周期的未来编码，须另行协商扩展，不能取消上限。板端首音频/流进展超时继续分别为 1000/2000 ms；这些是产品限制，不是 BLE/ATVV 标准值。

## 5. END 与异常

VOICE_ENDED=0x0382，kind=EVENT，必填 TLV：

| tag | 类型 | 字段 |
|---:|---|---|
| 1 | u32 | stream_id |
| 2 | u8 | reason |
| 3 | u64 | delivered_encoded_bytes，已提交 DATA 的编码片段字节总数，不含头 |
| 4 | u64 | ended_us |
| 5 | u32 | delivered_units，已完整提交的单元总数 |
| 6 | u64 | delivered_samples，完整单元输出帧总数，未知 UINT64_MAX |
| 7 | u32 | final_epoch，最后已声明的 epoch |

reason：0 normal、1 consumer_disabled、2 link_lost、3 buffer_overrun、4 invalid_encoded_data、5 source_data_lost、6 device_error、7 requested_stop、8 capture_limit、9 unsupported_format。

旧 decode_error 改为 invalid_encoded_data，限板子可验证的封装/初始化错误。真正 decoder 失败是主机 local_decode_error，不能宣称板子验证过解码结果。

normal/requested_stop/capture_limit 排完尾部完整单元后 END。源结束却存在不完整单元时必须异常结束。其他异常清除未提交数据/FORMAT，已提交的 RBP 帧完整发送再 END；允许异常 END 前存在部分单元，主机丢弃该不完整单元并标记录音不完整。encoded_bytes 包含它已经提交的片段，units/samples 只计完整单元，禁止回退已发送计数。正常结束不得有部分单元。

START、DATA、FORMAT、END 的媒体顺序不可被控制优先调度打乱。异常发生在 START 待发时须保留 START 的完整配置，按 START→零数据 END 收口。快速重按先将旧句未排尾音明确以 buffer_overrun 收口，再开始新句；禁止交错或将丢包后的半句伪装成新录音。

VOICE_ENABLE(false) 先结束交付再回 OK，响应后无旧音频。VOICE_STOP 匹配 stream_id，先 OK 再请求停麦，2000 ms 未停以 device_error 恢复；重复 STOP 不重复下发副作用命令。禁用 USB 交付不是物理麦克风已关闭的证明。

BLE 断开先 END(link_lost)，再按键 reset 和设备断线；USB 已断则主机以 session_lost 收口，新会话不补发旧音频。主机队列满允许失败/丢弃本次录音，但必须报告 local_overrun 并 disable，不能拼接成正常完整 WAV。首版不做自动 PLC、插静音或丢数据后继续有状态解码。

## 6. Codec registry

0 无效；1 = IMA_ADPCM_CONTINUOUS_HI，revision=1。2..0x7fffffff 为后续全局登记；0x80000000..0xffffffff 保留，本版拒绝，不接受厂商自行碰撞编号。设备换成非 ATVV 协议不要求换 codec_id。

IMA_ADPCM_CONTINUOUS_HI/1 的完整定义：

- rate=8000/16000，channels=1，decoder 输出 signed16 PCM；每字节高 nibble 先、低 nibble 后。
- config 恰好 4 B：predictor:s16LE、step_index:u8(0..88)、reserved:u8=0。
- config 是下一个 nibble 之前的状态，predictor 本身不额外输出为样本。
- 使用标准 89 项 IMA step table 与 nibble index adjustment；每次饱和 predictor 到 -32768..32767、index 到 0..88。精确算法和独立参考验证见本地 client/c/ima_decoder.c、Telink 参考源码及 tests/test_reference_audio.py。
- 不带 ATVV 控制头、WAV IMA block header；每字节输出 2 frames，unit_sample_count=unit_size*2。状态跨单元保留，单元不是独立解码块；FORMAT 才替换 seed。
- RC003 已有参考/实机证据的起始 0/0 seed 由其适配器声明，其他设备不能盲用。可信 ATVV SYNC 转为 FORMAT，即使 rate 不变；源计数缺口优先 source_data_lost。

新增 Opus、其他 ADPCM 或厂商编码必须登记 profile：包边界、config/extradata、声道布局、输出格式、初始化/重置、样本数可知性、单元上限、decoder 延迟/预跳过/尾裁剪和错误策略，以及独立参考向量。只填编码名称不够。本版仅定义 IMA，不声称已支持其他 codec；通用包边界和 opaque config 已为其提供承载。

## 7. IMA profile 的精确运算与短向量

所有中间 predictor/diff 运算使用至少有符号 32 位。令 n 为 0..15 的 nibble：

```text
step = step_table[index]
diff = (step >> 3)
if n & 4: diff += step
if n & 2: diff += step >> 1
if n & 1: diff += step >> 2
predictor = clamp(predictor + (-diff if n & 8 else diff), -32768, 32767)
index = clamp(index + [-1,-1,-1,-1,2,4,6,8][n & 7], 0, 88)
emit predictor
```

每一步独立右移再相加，不替换为一个近似乘除表达式。step_table：

```text
7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,
73,80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,
449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,
2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,
7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,22385,
24623,27086,29794,32767
```

config=00 00 00 00，编码 17，输出 samples=[1,12]，PCM16LE=01 00 0c 00，最终 predictor=12/index=8。

FORMAT config=2e fb 2a 00（predictor=-1234,index=42），编码 17，输出 [-1081,-387]，PCM16LE=c7 fb 7d fe，最终 predictor=-387/index=49。第二例必须使用新 seed，不能继续上一 epoch 的 predictor。

配置 step_index>88 是协议非法，不能因为旧 C reset helper 会钳制它就静默接受。

主机可用 VOICE_START(0x0303) 请求独立新录音；详情见 wire-protocol.md。必须预先 VOICE_ENABLE；OK 不是音频开始。两次录音的压缩状态不得串接，分别按各自 START 配置解码。

## 8. ICO profile 2/1

新增 [iFLYTEK ICO 精确契约](ico-codec-v1.md)：16 kHz 单声道、40 字节/320 samples、空配置。主机需协商支持，应用 PCM 输出不变。

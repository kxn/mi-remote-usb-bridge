# 0.6.5 release 错误透传审计

审计确认原先存在以下信息丢失，现已修复：

- GATT API 的 SDK 返回值在映射到失败/重试前未在 release 保留：现在先记录原始值；异步事件状态和 ATT error 同样保留。
- GAP API 及连接、配对回调的原始码可能只留在 DEBUG：现在记录 API 阶段、参数 ID/事件类型、原始码。
- ATVV MIC_OPEN_ERROR 的 16 位远端状态曾只进调试日志：现在保留远端值；异常停止原因也记录。正常 BUSY 流程不改。
- EEPROM 读写擦除返回值被布尔失败合并：现在先保留 32 位值；校验不一致标记为本地错误，不能伪称 SDK 返回值。
- SDK staCB 在 release 未注册：恢复轻量回调，保留如 stage=0x86/code=5 的原始组合，未恢复射频探针。
- 适配器初始化失败及异常 HID 报告补充状态元数据。

## 阶段解释

GAP：1 SetParamValue，2 BondMgr GetParameter，3 BondMgr SetParameter，4 CancelDiscovery，5 EstablishLink，6 StartDevice，7 StartDiscovery，8 Role GetParameter，9 TerminateLink，10 ConfigDeviceAddr，11 PasscodeRsp，12 SNV read；32+pair_state 为配对回调；64 role event（context=opcode），65 termination reason，66 link update status。

GATT API：1 MTU，2 services，3 characteristics，4 descriptors，5 read，6 long read，7 write，0 write command，9 indication confirmation。GATT event 的 stage 为方法号或当前操作号（依记录位置），context 在方法状态记录中为当前操作。ATT 的 stage 为请求操作码或当前操作号。ATVV：stage 0x0c 是 MIC_OPEN_ERROR；stage 0 是异常 AUDIO_STOP；stage 1 是本地音频状态错误。Storage：1 read、2 write、3 erase、4 readback mismatch（本地 code=1）。Adapter：stage 为初始化状态；256 为异常 HID 报告，code=report ID，context=length。

## 语义与边界

不改变底层返回值、业务错误枚举、重试、配对或语音控制流程。PasscodeRsp 的 0xFE 特殊正常分支继续按已核实 SDK 行为处理；地址解析未匹配、正常 ATT 枚举结束不是新增错误。

固定 4 条、连续重复合并的 RAM 历史，经 GET_STATS 上送，包含覆盖计数。短临界区保存并恢复原中断屏蔽值；无动态分配、无中断内 USB 输出。SDK 启动失败若发生于 USB 服务启动之前，仍不能通过未运行的 USB 接口读取；重启也会清除 RAM 历史。不能声称任意频率的错误都能无损保留。

## 验证与交付

C 全套测试、参考流程测试、模拟器、GUI/客户端 12 项测试通过；错误记录和产品 RPC 的 ASan/UBSan 回归通过。另修复 GATT 测试 mock 的不正确结构体访问，未修改对应生产语义。

构建为 0.6.5，DEBUG=0、SDK_RX_PROBE=0、HOST_VOICE_START=0。产物 build/fw-065-release/ch582f.bin。已刷板读取 SDK/GAP 原始记录，用户确认 GUI 使用正常。0.6.4 归档本地保留。

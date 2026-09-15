# RC003 / ATVV 实现（0.6.5）

当前适配器为小米遥控器 2 Pro RC003，使用 BLE Central、HOGP 按键和 ATVV 语音；公共 RBP/3 不暴露任意 GATT 操作。
wch_central.c 管理 GAP 生命周期，wch_gatt.c 串行调度 ATT、处理忙/资源不足及内存归属；rc003_adapter.c 发现服务、Report Map/Reference、ATVV 属性，不硬编码 handle。RC003 差异限定在已匹配 profile 内。

rc003_atvv.c 处理能力、START/STOP/SYNC 和压缩数据。板上不解码 ADPCM，而是交付编码、采样率、同步状态和单元边界，上位机生成 PCM。见 [音频契约](audio-wire-v3.md)。

## 已确认行为

- 按住语音键开始，松开结束。约60秒遥控器自行终止，即使仍按住；主机重发请求没有获得有效续接音频，因此 release 不自动续录，HOST_VOICE_START 默认关闭。
- 断链依据 SDK 事件/监督超时处理，按持久设置自动重连；不能保证离线按键补发。
- PasscodeRsp 的 0xFE 根据固定 SDK 语义按非 Passkey 流程处理；不把该例外套用到其他 API。
- SDK 0x86/5 原样保留，不单凭该码断言音频损坏，也不抹掉它。

[来源索引](../references/README.md) 固定 SDK、ATVV/Telink 和其他接收器代码版本，参考代码不代替本板实测。SDK 二进制库未改写，见 [供应商修改](vendor-patches.md)。

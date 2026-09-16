# 0.6.6

支持小米 RC003/ATVV 与已测联通 HID/ICO；RBP/3 only，一次连接一个遥控器。

- 联通严格描述符、Report Reference、FD02 主动初始化检查，FB 控制、FC 音频重组和 5 秒录音刷新。
- 主机 Python/.NET/C ICO 解码及统一键码、来源与能力接口；安装原生依赖后音频继续输出 PCM16LE。
- 修复小米枚举超过 8 个 Report 被拒绝、联通广播无 HIDS UUID 被过滤的问题。
- 默认 ATT_MTU=23，避免本机联通在大 MTU 描述符读取时停滞；底层原因仍未证明。

## 固件与验收范围

`ch582f.bin` 为 DEBUG=0、SDK_RX_PROBE=0、HOST_VOICE_START=0、ATT_MTU=23、VERSION=0.6.6。SHA-256 与来源文件哈希见 manifest.json。

用户已确认 dev.1 的小米正常，以及 `0.6.6-unicom-mtu23` 对照固件正常。此发布二进制按同一 MTU 配置重新构建，将版本标识改为 0.6.6；它不是与已刷测试文件逐字节相同的文件，也未宣称重新刷板验收。长录音、休眠回连、逐键与双段语音还需本版 CH582F 专项验收；没有双遥控器同时连接功能。

C 产品、WCH GATT、独立小米/联通适配器回归通过；主机统一按键、ICO 抓包解码和模拟器验证详见设计文档。旧 releases/0.6.5 保留。

[刷写](../../docs/firmware-bringup.md) · [上位机使用指南](../../docs/host-sdk-demo.md) · [统一按键](../../docs/logical-input.md) · [ICO 依赖及许可证](../../client/c/ico/README.md)

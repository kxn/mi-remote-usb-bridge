# 调试固件

0.6.5 release 自带 GET_STATS 原始错误记录，先用 GUI Diagnostics 或 CLI 采集。构建开关见 [刷写说明](firmware-bringup.md)。DEBUG=1 才启用详细 trace；SDK_RX_PROBE=1 要求 DEBUG=1，仅用于开发诊断；HOST_VOICE_START 独立默认关闭。
不得在 RBP CDC 中混入 printf 文本。保存版本、构建开关、固件 hash、错误元数据和操作顺序。SDK 回调避免动态分配、阻塞输出，临界区恢复原中断屏蔽状态。

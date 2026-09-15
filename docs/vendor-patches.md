# WCH SDK 修改（0.6.5）

来源 openwch/ch583 revision bd508ad7ceed48377619837051412a651952857f。除 MCU.c 外，供应商构建文件与该快照对应文件逐字节一致；保留上游许可证和 WCH 芯片使用条件。

MCU.c 保持原编码，修改：Lib_Read_Flash 返回实际读取状态；Lib_Write_Flash 检查擦除并返回写入状态；注册 bridge_sdk_status，release 保存原始 SDK 状态，DEBUG/SDK_RX_PROBE 才启用额外开发诊断。

二进制库未修改。产品侧 wch_smp_identity.c 通过链接包装修正固定 SDK 的身份处理路径，升级库必须重新审核，不能机械套用。bridge.ld 从供应商链接脚本派生，预留 4096 B 栈。产品 EEPROM 0x7C00–0x7DFF 与 SDK SNV 0x7E00 起保留区分开。

[SDK lock](../references/sdk-lock.json) 固定二进制校验。升级时核对头文件、库、回调、身份分发、SNV、异步事件及缓冲所有权，运行自动和实机回归。

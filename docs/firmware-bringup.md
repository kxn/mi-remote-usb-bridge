# 0.6.6 固件构建与刷写

已测试 CH582F USB-C 小板和 RC003；需要外部 32 MHz/32 kHz 晶体及天线。CLK_OSC32K=0 使用外部 32 kHz 晶体，其他板型需核对。

## 构建

Windows、GNU Make（已验证 MSYS2）、MounRiver RISC-V GCC 8.2.0。SDK 子集已包含。工具链放 tools/toolchain/riscv-none-embed-gcc8，或 TOOLBIN 指定 bin 目录。[固定下载入口](../references/toolchain-links.json)。项目根目录 MSYS2 shell：

```sh
make -f firmware/Makefile DEBUG=0 SDK_RX_PROBE=0 HOST_VOICE_START=0 BUILD=build/fw-release
```

输出 build/fw-release/ch582f.bin、ELF、map、dump。版本 0.6.6。默认工具链文件是 Windows .exe，其他平台交叉构建未验证，可覆盖 CC/OBJCOPY/OBJDUMP/SIZE。SDK 静态库有 hash 校验。栈预留 4096 B，静态区到栈至少留 2048 B。

## 刷写

1. 关闭串口 GUI/CLI。
2. 按住 BOOT（PB22），复位或重新插入 USB，松开 BOOT，进入 USB ISP；按键以板子丝印为准。
3. WCHISPTool 选择 CH58x / CH582 / USB，搜索。
4. 选择 releases/0.6.6/ch582f.bin 下载。普通升级不清空 DataFlash，保留绑定。
5. 正常拔插，出现 CDC 串口后启动 GUI，HELLO 应显示 0.6.6。

解除绑定用 Forget binding。USB 是 CDC，不是键盘或系统麦克风；描述符属开发配置，不声明商业 VID/PID 分配。避免 Windows 直接连接遥控器后与板子抢连接。

## 调试

```sh
make -f firmware/Makefile DEBUG=1 SDK_RX_PROBE=0 HOST_VOICE_START=0 BUILD=build/fw-debug
```

SDK_RX_PROBE=1 要求 DEBUG=1；HOST_VOICE_START 独立默认关闭。正式版不自动续录。release 自带原始错误记录，通常先读 GET_STATS。

0.6.6 默认 `ATT_MTU=23`；可显式覆盖用于诊断，不建议将 247 作为当前联通配置。参见 [发布记录](../releases/0.6.6/README.md)。

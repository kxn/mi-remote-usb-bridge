# Mi Remote USB Bridge

把小米遥控器 2 Pro（RC003）的蓝牙按键与语音，经 CH582F 小板桥接到 USB。
Windows 不再直接连接遥控器，应用通过串口协议读取按键和录音，因此 Home、语音等键不会自动变成系统键盘输入。

**当前版本：0.6.5，协议 RBP/3.0。** 已在 CH582F + RC003 + Windows 上实测配对、按键、语音及 release 错误记录。此前版本已验证长录音和掉线重连；不是每个版本都重跑了全部射频场景。macOS 有串口及动作注入实现，尚未完成真机验收。

## 功能与边界

- 手动扫描、显示候选、选择配对、解除绑定；不会扫描到就自动配对。
- 保存绑定与自动重连设置；启用后板子上电自主恢复连接，无需上位机发起。
- USB CDC 承载逻辑按键、电量、连接/配对状态及压缩音频；不枚举成键盘或系统麦克风。
- 遥控器按住语音键录音，板子传原始压缩数据及解码元信息，上位机解码/保存 WAV。
- RC003 一次录音约 60 秒上限；已取消自动续录，release 不启用主机主动开麦实验。
- release 保留原始 SDK/GAP/GATT/ATVV/存储错误记录，GUI、CLI 自动读取。
- RBP/3 可登记其他音频编码；目前设备适配器只有 RC003，不能据此声称支持耳机、PS4 手柄或任意遥控器。

## 快速开始

1. 使用 [0.6.5 固件](releases/0.6.5/ch582f.bin)，按 [构建和刷写](docs/firmware-bringup.md) 操作。
2. Python 3.11+，在项目根目录执行（Windows PowerShell）：

```powershell
python -m venv tools/demo-venv
.\tools\demo-venv\Scripts\python.exe -m pip install -r demo/requirements.txt
.\tools\demo-venv\Scripts\python.exe demo/rbp_gui.py --port COM7
```

替换为实际端口。macOS 使用对应 Python 和 `/dev/cu.*` 端口；不要同时启动两个串口客户端。

3. 遥控器进入配对模式，GUI 点击 **Scan** → 选中候选 → **Pair selected**。已绑定设备无需重新配对。
4. 勾选 **Record next voice streams…** 选择目录，再按住语音键说话。按键注入需要显式启用 **Enable mapped OS actions**。
5. **Diagnostics (release)** 显示统计和最近 4 条原始记录；详细日志在 `build/logs/gui-session.log`。

CLI 录音和日志：

```powershell
.\tools\demo-venv\Scripts\python.exe demo/rbp_diag.py --port COM7 --record-dir build/recordings --log build/logs/diagnostic.jsonl --compact-log
```

`--scan` 只发现；`--pair` 需要手动输入候选序号。详见 [上位机接入](docs/host-sdk-demo.md)。

## 代码布局

| 目录 | 内容 |
| --- | --- |
| `firmware/product` | RBP 会话、设备模型、队列、release 诊断 |
| `firmware/adapters` | HOGP 与 RC003/ATVV 适配器 |
| `firmware/wch` | 板级、BLE/GATT、USB，以及固定 WCH SDK 子集 |
| `protocol` | RBP/3 C 帧/TLV、音频结构、schema、测试向量 |
| `client/python` | 独立 Python SDK，可安装；不依赖 Qt |
| `client/dotnet` | 新增 .NET 9 RBP/3 会话与音频 SDK，见该目录 README |
| `client/c` | 可选 C 音频解码库；不是完整串口会话 SDK |
| `demo` | CLI、GUI、WAV/映射/离线重放 |
| `tests`, `sim` | 自动回归与 C 模拟器 |
| `releases/0.6.5` | 当前固件、校验清单与验收范围 |

## 开发文档

[架构](docs/architecture.md) · [完整协议](docs/wire-protocol.md) · [压缩音频契约](docs/audio-wire-v3.md) · [ATVV 实现](docs/atvv-implementation.md) · [SDK/上位机](docs/host-sdk-demo.md) · [测试](docs/validation-plan.md) · [诊断](docs/release-errors-0.6.5.md) · [SDK 修改](docs/vendor-patches.md) · [参考资料](references/README.md)

自有代码采用 [MIT](LICENSE)，WCH SDK 和其他第三方组件遵循 [各自条款](THIRD_PARTY_NOTICES.md)。本项目与小米、WCH 无官方隶属关系。

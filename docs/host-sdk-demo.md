# 上位机 SDK 与 Demo（0.6.6）

## SDK

Python rbp 是完整 RBP/3 客户端：帧、会话、心跳、RPC、按键、压缩音频重组与解码。不依赖 Qt 或常驻 Rust/C 服务。BridgeClient 只允许一个 I/O 所有者；并发应用用 BridgeWorker 提交命令并取事件，不在 I/O 回调中阻塞 RPC、解码或写盘。

```sh
python -m pip install ./client/python
```

源码 Demo 自动加入 SDK 路径，但仍需安装依赖。client/c 是 C 音频解码库，protocol/src 是帧/TLV 工具，不是完整 C 会话 SDK。其他语言可直接实现公开协议。

## GUI

Python 3.11+：

```sh
python -m pip install -r demo/requirements.txt
python demo/rbp_gui.py --port COM7
```

省略 --port 可在界面选择后 Open。macOS 使用 /dev/cu.*。Scan 只发现，Pair selected 才配对；Forget binding 清绑定；Connect/Disconnect 控制连接；Automatic remote reconnect 保存重连设置。
Record next voice streams 选择目录后，按住语音键录音。Stop current voice 停止当前流。WAV 不等于系统麦克风。Load mapping 和 Enable mapped OS actions 显式启用映射，默认不注入系统按键。

Diagnostics (release) 每 2 秒刷新统计和最近 4 条原始记录，包含来源、阶段、原始码、上下文、次数、板端时间及覆盖条数。记录不全部表示操作失败。日志在 build/logs/gui-session.log。

## CLI

```sh
python demo/rbp_diag.py --port COM7 --record-dir build/recordings --log build/logs/diagnostic.jsonl --compact-log
python demo/rbp_diag.py --port COM7 --scan
python demo/rbp_diag.py --port COM7 --pair
python demo/replay_audio.py path/to/voice.encoded.jsonl build/replayed.wav
```

--pair 需要手动输入候选序号，即使只有一个；空输入取消。默认恢复会话并连接已有绑定；--no-connect 用于观察自主重连，--seconds N 限时退出，Ctrl+C 结束。
CLI 保存压缩 JSONL、元数据和 WAV；compact 只省略重复 hex。没有 raw-only 开关，其他应用可消费 SDK 事件。每 10 秒读 GET_STATS，变化时写 release_faults。release 不支持实验 --start-voice。

## 生命周期

每个 START 新建 AudioDecoder/AudioStream，保存 codec_config、seed、epoch、单元边界和完整性标记，不能仅存裸 ADPCM。未知 codec 拒绝解码；速率/声道改变时 WAV 分段。
会话丢失、溢出、序列错误结束当前流，不跳过损坏编码继续输出。USB 会话和蓝牙绑定独立，关闭 GUI 不解除配对。
make decoder 在 Windows 构建 build/host/rbp_decoder.dll。client/c 源码及 protocol/include 可编入其他宿主，调用方负责 ABI size/version、完整单元及输出容量。macOS 真机及应用打包需另验收。

## 小米与联通接入步骤（0.6.6）

1. 刷入 `releases/0.6.6/ch582f.bin`；关闭 GUI 串口再刷写，通常无需清 DataFlash。固件默认 ATT MTU=23，同一时间连接一个遥控器。
2. 安装 Python SDK/GUI 依赖。联通语音另运行 `python client/c/ico/build.py`，需要原生 GCC/Clang；开发目录自动查找 `build/ico/ico.dll`。安装后的应用通过 `RBP_ICO_LIBRARY` 指向该库，位数必须与宿主一致。没有原生库时不声明 ICO 能力，不能解码联通语音。依赖下载、许可证与 .NET/C 部署见 [ICO 安装](../client/c/ico/README.md)。
3. GUI Open 对应 CDC 串口；遥控器进入配对模式，再 Scan、选择设备、Pair selected。列表也可能显示蓝牙开关等未识别设备；出现候选不等于兼容，连接后仍严格检查描述符和协议行为。Forget binding 只清当前板端绑定，不会使遥控器停止广播。
4. Ready 后测试按键；选中 Record next voice streams 并指定目录，按住语音键、说话、松开，检查 WAV。小米和联通均向现有上层音频接口交付 PCM16LE，编码差异由主机库处理。
5. 程序接入优先使用 [统一按键接口](logical-input.md)：Python `enable_logical_keys()` / `on_key_event`；.NET `RemoteInputSession` / `RemoteKey`；C `rbp_input`。使用 model_id 识别方案，has_key 查询能力；不要将位图 slot 当作统一键码，不依赖广播名称确定型号。物理布局单独查询。

将 SDK 集成到独立应用时，Python wheel 不含 ICO 原生库；.NET 部署也需复制相应架构原生库或设置环境变量。示例 GUI 保留原始键目录显示接口，库的统一按键包装器可供新应用直接使用。

### 诊断与验证边界

- `init failed: S=08 ... M=0000 ... T=1` 表示读取 HID Map 阶段没有得到首段数据。此次联通在 MTU=247 下伴随密集 SDK `0x86/5`，MTU=23 对照版用户反馈正常；尚未证明 SDK 私有状态码的底层根因。
- 小米多于 8 个 Report 特征时保持旧版处理；联通仍严格检查实测拓扑。
- 两种遥控器的连接方案已在本次用户测试中得到反馈，但本次 0.6.6 还没有逐项完成 CH582F 长录音、休眠重连、双段语音与所有键位的完整硬件验收。研究仓库中 USB 蓝牙棒测试结果不能替代 CH582F 验收。

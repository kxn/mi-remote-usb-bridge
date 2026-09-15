# 上位机 SDK 与 Demo（0.6.5）

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

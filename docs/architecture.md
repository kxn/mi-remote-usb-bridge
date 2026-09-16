# RBP/3.0 架构：板上蓝牙适配，上位机音频解码

当前版本 0.6.5 / RBP/3.0，验收范围见 validation-plan.md。

开发中的小米/联通双配置见 [适配与识别设计](multi-remote-design.md)；代码已加入，联通 CH582F 实机验收尚未完成，0.6.5 发布包保持原样。

## 1. 职责

蓝牙设备 → 板上设备适配器 → 逻辑按键 + 原生压缩音频及格式描述 → USB RBP/3.0 → 上位机 decoder → PCM/录音/ASR。

板子负责连接、SMP 配对、绑定、GATT、设备控制、源录音生命周期、可靠可知的源连续性验证。上位机不发送 ATVV 开麦字节，不操作 CCCD，不需要理解设备厂商封装。上位机负责 codec 配置验证、解码、播放、WAV/ASR、按键动作和用户配对确认。

一个活动设备、最多 64 个逻辑键、零或一路输入音频。其他非 ATVV 设备可通过新板上适配器接入；同 codec 复用同 decoder。CH582F 的无线能力没有因通用 USB 协议而增加，Classic Bluetooth 手柄/耳机不在已支持清单。

## 2. 适配器边界

内部接口应输出 source_begin(format)、encoded_fragment(unit metadata, borrowed bytes)、format_reset(full format)、source_end(reason)，另有逻辑键、电池和配对事件。source 活动与当前 USB 是否接收严格分开。

format 包含 codec_id/revision、采样率、声道、完整 decoder config、max_unit_bytes。数据附编码单元总长度/偏移，以及源协议能够可靠提供的输出样本数；无法提供就 UNKNOWN，禁止解码全包只为计算 sample_count。适配器可增量转交大单元，不要求板上分配 64 KiB。

有状态 codec 的初始化必须在 START 前明确；后续可信状态重置必须输出 FORMAT，即使采样率不变。设备协议数据缺口不能被“重置后继续”掩盖。首个 RC003 适配器保留现有经验证的 BLE/GATT/ATVV 顺序，仅把解码工作移出板子。

## 3. 队列和所有权

编码字节池与有限元数据管理 START/FORMAT/单元分片/END；产品输出环与 USB DMA 保留各自异步所有权。一次 callback 中的借用数据必须在 SDK buffer 释放前复制到有界产品缓冲或完整提交输出，不可保存悬挂指针。

对配置同样如此：START 未发出时必须拥有其 config；异常清队列不能使 START 丢配置。队列计数必须区分编码字节、完整单元和样本帧。不能把旧 pcm_len 除以 2 的逻辑机械改名后留下。媒体顺序与控制公平性延续旧设计。

处理顺序是尽快消费并释放 SDK 通知、推进 USB 输出；不拿 BLE SDK 缓冲当排队仓库，不关中断等待主机，不引入每包定时器延迟。无法承受的突发显式终止本 stream，保留可用按键/控制。

## 4. 资源判断

16 kHz 单声道 IMA：8 KB/s 压缩、32 KB/s PCM。同等纯音频空间可存约 4 倍时间，但元数据/封包开销不会同步缩小。按每单元 120 B、RBP3 40 B 音频头、32 B 通用头、CRC/COBS估算，USB 约 13.2 KB/s；旧 PCM 通路约 35.9 KB/s，实际约降低至 37%，不能说整个 USB 流量精确变成 1/4。

主机声明 64 KiB 单元上限不要求板子拥有 64 KiB RAM。板子按 <=472 B 片段转交；主机为包式 codec 有界重组。Debug/Release 的最终静态 RAM、栈峰值、吞吐均须在迁移完成后从 ELF 与实机重新报告，不能复用 0.4.5 数字冒充 v3 测量。

弱信号恢复会有积压突发的可能；纯重传重复由链路层处理，恢复后的新音频仍需流水排出。正常主机读取时的突发溢出属于板上通路验收问题；主机停读则允许有界失败，但不得静默制造完整音频。

## 5. 上位机与错误边界

USB IO 线程只处理有界协议工作、心跳和事件投递；decoder、WAV、ASR 独立有界队列。协议库默认提供压缩音频 API；可选 decoder 库提供 PCM 便利接口，不创建 Rust Core/常驻服务要求。

不认识 codec 时拒绝音频而保留键功能；local_decode_error/local_overrun 与 session_lost 分开。断线/取消明确收口，未知样本计数不伪装成零。公共契约以 wire-protocol.md 与 audio-wire-v3.md 为准。

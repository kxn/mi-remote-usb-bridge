# 0.6.11：首次语音启用前的按下边沿

近距拔插实测0.6.9：15:48:43.867 INITIALIZING，44.364 READY，44.379主机完成PCM协商。
无0x3E，无录音开始事件。确认Flash缓存上电恢复有效；现有release日志不能确定F8是否到达。

明确代码缺陷：Unicom仅在!ready或voice_wanted时锁存按下，遗漏READY与首次VOICE_ENABLE
之间的窗口。增加连接范围内voice_armed状态：首次主机启用前保留实际按下，启用后开始；
松开/断链取消。初次启用之后的显式禁用期间，不排队等待下次启用。没有根据建链推断按键。
该缺陷已修复，不声称它已被证明就是这次实测的原因。

新增debug事件 hid20：F8 down/state/host_wanted/voice_armed；hid21：开始语音。
继承0.6.10对PHY未初始化hdr.status误报的修复。SDK/GATT其余流程不改。
Flash页缓冲改为调用栈临时分配，避免永久占用512字节导致debug版RAM余量不足；
实编栈帧board_cache_load=544、save=592，调用者sync_gatt_cache=400字节，
仍保留原链接脚本的栈与额外2KiB余量检查，没有放宽限制。

make test reference-test通过，含READY前后、启用延迟、提前松开、显式禁用、Flash掉电回滚。
候选：build/fw-0611-debug/ch582f.bin，版本0.6.11-debug。
text=214800 data=344 bss=20624。
待测试：CLI启用BLE/GATT/HID/会话跟踪，固定位置，直接语音键唤醒并保持，
确认F8是否到达、相对READY/VOICE_ENABLE时刻、FB提交完成以及FC到达。
不要让GUI与CLI争抢串口；不重新配对，不清除Data Flash。

# 0.6.7 重连暂停状态修复

## 原因

FORGET_PEER 在删除旧绑定时设置 reconnect_paused=true，防止清理期间重新连接。旧实现只在 SET_RECONNECT(true) 或 CONNECT_PEER 中清除此标志；新绑定保存成功时虽然将 auto_reconnect 设为 true，却没有解除上一绑定遗留的暂停。

因此同一次上电内执行「解除配对 → 重新配对 → 断链」后，GET_PEER 可报告自动重连开启，而 rbp_server_should_reconnect 仍返回 false。此问题位于公共产品状态机，不局限于联通；小米也受同一路径影响。普通首次上电配对不一定复现。

修复：仅在新绑定成功持久化后清除 reconnect_paused。配对失败、存储失败不改变原暂停意图；显式 DISCONNECT 仍暂停当前绑定，SET_RECONNECT(true) 仍可显式恢复。没有修改 BLE 扫描、安全策略、缓存、音频或协议格式。

## 证据与验证

2026-09-16，在现有 0.6.6-unicom-mtu23 实物上读取到 auto_reconnect=true、state=1（离线），并非应用未启用配置。只发送一次 SET_RECONNECT(true)，未清绑定、未刷写，状态即进入 state=2（重连）。用户短按方向键后在 14:53:39 进入 unicom.hid_ico.v1 READY，随后 1.5 秒语音完成识别。独占串口采样保存在本机忽略目录 build/logs/reconnect-20260916-*.json。

新增 C 回归先在旧实现上失败（重新绑定后 should_reconnect 为 false），再在修复后通过。覆盖解除绑定期间暂停、重新绑定后的断链、显式断开、失败配对、失败持久化、显式恢复和 USB 会话结束。make test 全部通过；交叉构建成功。

候选固件：build/fw-067/ch582f.bin，版本 0.6.7，release、MTU 23、无实验录音和 debug 开关。SHA256: 372559bbf11e12498d396e0db6fd593b60eeebbf809ba45a2740269ea3457749。

此候选尚未刷入实物，不能将旧版显式恢复后的实测当作新版完整配对/掉线回归。刷写无需清 DataFlash；新版最终验收应覆盖同一次上电内解除配对、重新配对、断链、唤醒与语音。

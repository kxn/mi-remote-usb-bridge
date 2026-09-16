# 2026-09-16 15:40 联通重连失败

运行版本 0.6.9，内部 GUI 日志 miremote.log。

- 15:40:35.823 INITIALIZING，35.971 DISCONNECTED。
- GAP stage 0x41 code 0x3E context 0x93：147ms，bit31=0（尚未启动GATT），bit30=0。
- 15:40:37.984 再次 CONNECTING，38.412 INITIALIZING，38.957 READY。
- 成功重连初始化 545ms，已明显不同于完整发现约4s；没有发生GATT初始化失败。
- 15:41:12.393 下一次语音开始，13.582 定稿“这个如何？”；失败到恢复之间没有语音开始事件。
- 15:37:15、15:38:42 的 0x08 / context 高两位=11 是此前已READY链接超时，不能混同这次初始0x3E。
- 扫描开始的时间不等于遥控器回到范围内的时间，不能把扫描等待计入建链耗时。

0x3E 仍待定位空口建立失败的具体诱因。此次确认发生于加密后的GATT启动之前，
不是MTU、缓存加载、Report Map发现或语音FB命令引发。已绑定重连扫描使用既有身份，
未看到鉴权失败/清绑定。保持当前参数，不用猜测性延时或跨连接伪造语音按下掩盖问题。

## 附带查实的诊断缺陷（0.6.10）

SDK gapSendPhyUpdateEvent 分配8字节消息，写offset 0、2、3、4..7；没有写offset 1
（hdr.status）。offset 3 才是专用 status。反汇编：build/sdk-phy-event.txt。
原 wc_event_cb 统一读取 hdr.status，造成 PHY 成功事件误报随机错误值，如EB/31/41。
这推翻了先前把这些数值当作有效PHY错误分析的前提，但不影响独立的断链reason=3E。

改为仅初始化、建链完成、扫描完成事件读取头部状态；连接参数更新和PHY更新读取
专用status，并校验当前连接handle。真正的错误值不翻译、不丢弃；PHY错误stage=67，
context低8位TX PHY、次8位RX PHY。原连接参数更新错误stage=66保留。

测试：污染hdr.status的PHY/参数更新成功事件不产生fault；真实status失败保留原码；
过期连接handle不污染当前记录。make pair-test通过。固件0.6.10已编译用于诊断修正，
不宣称其修复了底层0x3E，用户无需为了这项误报修正立即再刷一次。

# 0.6.13：首次建联失败的有界快速恢复

## 2026-09-16 实测证据

原始日志：build/logs/preroll-0612.jsonl。
16:16:25.296 收到匹配本机身份的定向广播，RSSI -40 dBm；SDK接受建联。
板端261791 ms LINK_ESTABLISHED，间隔23.75 ms、latency 0、监督超时3000 ms。
261943 ms以0x3E断开：152 ms，无加密完成事件，GATT未启动。
固件等待2010 ms才恢复扫描。第二次收到普通广播，264044 ms建联，
264317 ms加密完成并恢复缓存，264460 ms READY，共416 ms。
265646 ms只收到F8松开；这轮没有收到F8按下，没有启动录音。
不能把这轮称为预收测试通过，也不能从主机日志确定用户物理按键时刻。

## 规范与 SDK 核对

- Bluetooth Core 5.4 Vol 6 Part B 4.5.2：未建立的连接在6个连接事件后判定丢失；
  这与已建立连接的3秒监督超时不同。改MTU或增加音频RAM不能解决本次失败。
- 同文4.4.2.4.3：高占空比定向广播最长1.28秒。现有广播报告本身不证明
  遥控器使用高占空比模式，但失败后停扫2秒不利于抓住快速重连窗口。
- 来源：https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/Core-54/out/en/low-energy-controller/link-layer-specification.html
- 固定SDK反汇编build/sdk-supervision-069.txt：ll_connect_supervision_timeout读取
  connection+15的bit1，在未置位路径向LL_ConnectToStandby传0x3E，另一分支传0x08。
  build/sdk-connect-timer-069.txt显示该回调由TMOS定时任务调用。
- 本机公开身份与广播TargetA相符；没有证据支持删绑定、换地址或强行更换PHY。
- WCH原始Central示例同样使用CentralEstablishLink(FALSE,FALSE,...)。
  这里第一个参数是发起连接时扫描占空比，不是“是否允许定向广播”。
- 本次失败尚未启动适配器；缓存写入在READY后，无法解释此轮最初152ms失败。
  USB关键区仅屏蔽USB IRQ。上述核对不能排除SDK内部调度/射频问题。

## 修复范围

此前所有断开共用1/2/5秒退避，已连接掉线先消耗一次，再发生0x3E会额外停扫2秒。
现将已绑定恢复中的早期0x3E单独处理：

- 仅在匹配当前handle的终止事件，或SDK最终失败completion之后允许恢复。
- 当前连接必须不是主动取消/断开、不是配对，且适配器尚未启动。
- 最多两次不额外等待；由下一次poll重新扫描和解析最新地址，不重入SDK回调。
- 上层自动重连开关、安全阻断、存储恢复、radio故障仍由原路径约束。
- 两次用尽后继续原有退避，只有READY恢复次数预算；重复事件不消耗预算。
- 不伪造语音按下、不根据连接成功启动麦克风、不重放旧连接状态。
- 新增ble20诊断：early资格、已用快速恢复次数、等待毫秒、普通退避级别。
- 失败completion的原始SDK状态追加GAP stage68记录；已有stage65保持不变。

这是恢复策略修复，不是0x3E底层成因已解决的声明；不保证首次语音一定完整。
若快速恢复后仍只收到release，需要进一步确认遥控器是否支持可靠读取当前
按键状态，再设计状态同步。未经验证不能把一次Report读取当作当前按键事实。

## 验证与下一步

make test reference-test通过；新增覆盖0x08后连续0x3E、有界恢复、重复终止、
READY预算恢复、失败completion、自动重连关闭、取消、配对、认证错误、
GATT已启动及计时器回绕。0.6.13-debug编译通过，原RAM余量断言通过。
text216228/data344/bss20960。
固件build/fw-0613-debug/ch582f.bin
SHA256 57FE407530E042381FA8C24E298150E62BE973C6A9D4F4759FC256E7C9EF15AC

待实机验证：离线后语音键唤醒，记录0x3E、ble20、下一次广播、READY、F8、FB和首包。
若复现0x3E，需要BLE空口抓包核对CONNECT_IND地址、WinOffset/WinSize、
Interval、SCA、Channel Map及最初6个连接事件。仅此板的GAP日志无法证明
CONNECT_IND被遥控器接受，也不能确定接收失败来自哪一端。
不通过改SDK超时或跳过定向广播掩盖这个待定位问题。

## 0.6.13 实机结果（16:33）

本轮起始定向广播成功建立连接，F8 down发生于板端48745ms，host_wanted=0；
48750ms提交FB=1，48839ms首个FC通知。证明早期按下保留/物理采集提前生效，
不证明实际发声到首包的时差（没有物理按键/发声时刻）。
录音收到239单元/76480采样/4.78秒，53658ms以Unicom FC continuity失败，
原因5 SOURCE_DATA_LOST。此时无F8 release，系固件保护性主动断开。
后续首次重连以0x3E失败（54853ms），54858ms重新扫描，仅等待5ms；
再次定向广播成功建立，证明定向建联并非必然失败。

本轮原日志未记录FC期望/实际序号及分片，无法判定丢片、重复还是序号规则问题。
已补source8 stage0x91及hid24，仅异常时保存expected/actual序号和part，不记录音频，
不取消连续性检查。新增测试验证清理前保留这两个计数，主机C回归通过。
该诊断补充编译为build/fw-0614-debug/ch582f.bin；尚未刷入，不宣称修复分片异常。
原始证据build/logs/reconnect-0613-20260916-163238.jsonl。

## 预收 FIFO 自审（同轮用户反馈）

核对voice_enable请求16:33:00.501870，F8到达16:33:00.455，首FC约16:33:00.55。
开头关键debug序号141..162连续，无hid22预收入队事件；首个完整单元直接交付。
从首个匹配广播板端48259ms到首FC48839ms约580ms，其中F8之前486ms，
F8到FB提交5ms、F8到首FC94ms。不能把94ms说成从物理按键到收音的总延迟。
160ms预收只覆盖已收到的编码音频等待主机授权，不能回补BLE连接建立前尚未收到的声音。

补充24单元真实fixture逐字节回归：先缓存4单元，交错tick排队/实时收包，
覆盖环形队列多次绕回；松手后排完已授权缓存；下一次录音使用不同后续音频，
确认不继承上一段数据。make test reference-test通过。
本轮未发现FIFO漏头或旧缓存重放的证据。上游连续性校验位于预收队列之前，
该错误不能由队列出队顺序直接触发。

不能从SOURCE_DATA_LOST断言用户说话尾部必然可听缺失，也不能将缺片解释为正常松手。
只有F8 release或停止流程才能证明正常终止；现日志缺少异常FC计数，保留待定位结论。

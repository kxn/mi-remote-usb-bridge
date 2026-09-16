# 0.6.9：持久化 GATT 缓存与早期建链失败诊断

## 变化

0.6.8 的缓存只在 RAM。0.6.9 为已验证的小米 RC003 和联通 HID/ICO 描述信息
增加独立 Data Flash 缓存，绑定到单调分配的 peer_id。不是将运行时结构体直接写盘：
格式 GAC1 显式编码 Report Map、各句柄、Report Reference、属性及已验证的配置。
上电恢复后仍须当前绑定恢复加密，以及完成各协议需要的运行时初始化。

持久化区域 0x7800..0x7BFF：两个 512 字节槽，每槽两个 256 字节擦除页；
原绑定事务区域 0x7C00..0x7DFF、SDK SNV 0x7E00..0x7FFF 不变。
外层 GCS1 含版本、代数、peer_id、长度、CRC32C、最后提交字。
同内容不重复写；新记录写入失败保留上一条；缓存不能修改密钥或清除绑定。
零长度最新记录表示缓存失效。未知版本/模型、损坏或其他 peer 的缓存不使用。

首次发现完成、绑定已提交且无录音进行时保存一次；断链清理前同步失效记录，
避免 detach 清除 peer_id 后忘记使 Flash 缓存失效。内存/闪存缓存失效时继续完整发现。
旧版本没有可恢复的缓存，所以升级后第一次仍需发现并写入一次，之后拔插才能
验证持久化恢复。不要勾选烧录器清空 Data Flash，否则绑定和缓存都会丢失。

## 2026-09-16 实测日志分析（运行的是 0.6.8）

- 15:20:12.309 INITIALIZING，12.456 DISCONNECTED：约 147ms。
- 15:20:13.471 INITIALIZING，13.618 DISCONNECTED：约 147ms。
- 原始诊断 source=2 stage=0x41 code=0x3E count=2，来自
  GAP_LINK_TERMINATED_EVENT 的 linkTerminate.reason（SDK 头文件注释为 termination reason from LL）。
- 第三次 15:20:15.689 INITIALIZING，19.703 READY：约 4.014s。
- 另有 source=2 stage=0x40 code=0x41 context=0x11，是 GAP PHY Update 事件的
  hdr.status。它不是上述终止事件，也不能只根据相同十六进制值套用另一命名空间。

因此慢至少包含两个阶段：早期 LL 失败/重试，以及首次 GATT 全量发现。
Flash 缓存解决后者，不宣称解决前者。

## SDK 与规范证据

锁定 SDK：references/sdk-lock.json，LIBCH58xBLE.a SHA256
13bd910bc647ce7ba2bc9a36de2fa5bed987ef83b9dacf5045f653bb062c67ce。
反汇编 ll_connect_supervision_timeout（ll_connect.o）：
- lbu a5,15(a0)，测试 bit 1；
- 默认 a1=62 (0x3E)，该位非零时 a1=8 (0x08)；
- 调用 LL_ConnectToStandby。
ll_connect_set_connect_timeout 将该函数注册给 tmos_start_callback_task。
原始反汇编保存在 build/sdk-supervision-069.txt、build/sdk-connect-timer-069.txt。
结构成员的完整语义没有源码声明，仅据分支和规范推断为建立状态，不能声称知道全部内部细节。

Bluetooth Core 5.4 Vol 1 Part F §2.59 定义 0x3E：
https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/Core-54/out/en/architecture%2C-mixing%2C-and-conventions/controller-error-codes.html
Vol 6 Part B §4.5.2：未建立连接在 6 个连接事件内仍未建立即终止，区别于建立后
使用 connSupervisionTimeout。初始配置为 15–30ms 间隔，3s supervision timeout；
日志的约147ms与早期建立超时相符，不能用增加3s超时解释/修复。
https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/Core-54/out/en/low-energy-controller/link-layer-specification.html

排查范围：CONNECT_IND 接收失败/数据通道收发错过，SDK与遥控器的初始锚点时序
兼容性，射频/时钟/中断延迟等。现有日志不能确定是哪一种。应用不主动更新PHY；
GATT 仅在 LINK_ENCRYPTED 后启动；USB临界区只屏蔽USB中断，USB中断优先级低于radio。
没有证据支持直接调大 MTU、清绑定或任意延长协议超时。

新 release 断链诊断 context：低30位=从 LINK_ESTABLISHED 到断链的毫秒数，
bit31=该连接已启动GATT，bit30=事件当时adapter为READY。主动断开可能已清理READY；
这些位是事件当时状态，不是曾经READY的历史。保留SDK reason不改写。

## 验证和产物

make test reference-test：通过；包含两种遥控器序列化恢复、录音期间延迟写、
断链前失效落盘、相同内容不重写、不同peer拒绝、1024种写入中断位置的缓存回滚，
以及原有绑定事务、USB、GATT、按键、音频测试。sim 编译通过。
交叉编译：text 210576，data 336，bss 21160。
缓存写入栈使用：sync_gatt_cache 400字节 + board_cache_save 80字节（不含下层ROM调用）；
共用静态512字节页缓冲，避免多页缓冲堆叠在栈上。
固件：build/fw-069/ch582f.bin
SHA256：134D632AC8899B5049AEFF8140335110F41B4EF582752915350D88503CA0E7BD

待实机验证：升级后首次READY保存，拔插再次唤醒测冷启动恢复，再做离开返回首按语音。
0x3E 仍未确认根因；若复现，先依据新增context判断阶段，再与成功连接参数比较，
必要时用空口抓包区分请求未接收和连接窗口未对齐，不将猜测写成固件补丁。

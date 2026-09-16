# 0.6.8 联通遥控器重连修正（待硬件验证）

日期：2026-09-16。包含 0.6.7 的重新配对后解除 reconnect_paused 修复。

## 证据与原因

内部程序日志中，14:57:51.191 到 14:57:55.181，以及 14:57:57.773 到
14:58:01.596，初始化分别耗时 3.990 秒、3.823 秒。开始扫描的时间不是遥控器
唤醒时间，不能用扫描等待时间估算建链耗时。

联通 unicom_ready 原来强制 cache_valid=false，每次重连重新发现服务、读取
169 字节 Report Map、读取各 Report Reference、写各 CCCD、初始化语音。
同时 unicom_notify 在 ready 前丢弃 F8 语音按下事件。

MTU=23 时 Read/Read Blob 每响应最多 22 字节，169 字节至少需要 8 次响应；
它增加冷启动发现成本，但不是反复全量发现的理由。联通 FC 本身是 20 字节
通知，本次保持 MTU=23，不回到此前尚未排除兼容问题的大 MTU。

## 实现

- 仅复用同一 peer_id、已验证且 cache_safe 的数据库；配对持久化成功后将
  初次发现结果关联到正式 peer_id。缓存只在 RAM，不增加 flash 写入。
- 重连保留数据库句柄，清除 hello、按键、待录音、音频分片和写事务等连接状态。
- 已测拓扑的恢复路径为 MTU 交换、FD02 CCCD 写入及 hello；测试确认没有重新
  发现服务、读取 Report Map/Reference。存在 Service Changed 或 Protocol Mode
  时仍执行对应恢复步骤，不能将“两次请求”泛化到所有设备。
- 等待当前连接的合法 hello 才允许语音。缓存恢复期间实际收到的 F8 按下可等待
  READY；松开/断链取消，方向键唤醒不会推断成语音按下。
- Service Changed、Database Out Of Sync、缓存属性失败或协议结构不匹配仍使
  缓存失效。音频连续性错误和传输超时关闭当前连接，但不等同于数据库改变。
- 运行时错误上报保留具体 cause 和原始 ATT 错误；不再仅显示 init failed。
  日志已有的 S=1B 错误尚不能确定是哪一个运行时原因，不能声称已经全部解决。

规范依据：Bluetooth Core 5.4, Vol 3 Part G §2.5.2 Attribute caching、
§3.3.3.3 Client Characteristic Configuration：绑定客户端可跨连接缓存数据库，
绑定设备的 CCCD 应跨连接持久化，收到变更后需使相关缓存失效。
https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/Core-54/out/en/host/generic-attribute-profile--gatt-.html

## 验证

- make test sim：通过（已有的编译警告仍存在）。
- make reference-test：通过，小米缓存/早期语音/取消/失效回归。
- 新联通用例：缓存恢复请求数量、hello 早于 CCCD ACK、初始化按下/松开、
  host 尚未启用语音、真实录音分片、可选 Service Changed/Protocol Mode 顺序、
  配对提交、跨 peer 不复用、数据库变化失效、媒体错误保留缓存、错误原因上报。
- 交叉编译 release 0.6.8：text=207724，data=328，bss=20640。
- 固件：build/fw-068/ch582f.bin
- SHA256：119176167749740BC59D29142A31BD01DA1D38FA595C7EF935C9534A72788E71

## 实机步骤及边界

刷入后拔插，先在附近唤醒一次等待 READY，完成冷启动数据库发现。
随后拿远至离线，再拿回直接按住语音说话；检查建链后到 READY 的时间、第一按的
录音与具体错误。不要重新配对，也不要重启板子，否则不属于 RAM 缓存恢复测试。

尚未实测该版本的重连耗时及首按成功率。缓存无法追回无线连接建立前设备未发送
的音频；上电首次连接仍完整发现，不能保证首个音节无损。

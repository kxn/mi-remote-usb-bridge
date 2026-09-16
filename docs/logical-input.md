# 统一按键与设备信息

## 应用层约定

- `model_id` 标识已验证的兼容方案，例如 `xiaomi.rc003`、`unicom.hid_ico.v1`，不是蓝牙广播名称，也不是对硬件厂商的鉴定。
- 应用使用统一 `key_id` / `Key` / `RemoteKey`：两种遥控器的 UP、OK、BACK、音量等语义相同。位图中的 slot 只是该连接键目录的索引，不能直接当作键码。
- `has_key` / `HasKey` 查询当前连接实际支持的键；连接未就绪时返回 false。未知扩展键保留数值，不丢弃。
- 来源包含接收器标识、session、peer、connection、model 和 catalog revision。接收器标识由应用传入，未传则为客户端实例生成；需要跨重启识别时由应用维护稳定标识。
- 收到 `down` / `up` 才处理按键变化。`snapshot` 仅建立状态，不生成虚假的按下动作；`reset` 和带 `synthetic` 的松开用于断线/重连清理，不应触发点击动作。应用退出、事件通道失败或溢出时，也应释放该接收器的全部输入。
- 一个库实例当前对应一个接收器的一条连接。多个 USB 接收器可以分别建立实例并按来源合并事件；这不表示一块 CH582F 已支持同时连接多个遥控器。

音频接口不变：应用原来的音频解码路径继续输出 PCM。统一按键层不重新实现音频解码。

## Python

在完成 `hello()` 后启用，允许在遥控器尚未连接时调用：

```python
from rbp import Key

client.on_key_event = lambda event: pending.put(event)
client.enable_logical_keys()
while running:
    client.drain(0.05)  # 自动加载目录、订阅、读取初始快照
    # 在自己的事件处理代码中：
    # if event.kind == "down" and event.key_id == Key.UP: ...
```

`client.has_key(Key.CHANNEL_UP)` 查询能力；`client.input_info()` 返回来源、键目录、当前按键集合、可选型号/布局元数据。直接使用 BridgeClient 时，应由同一线程拥有 IO；回调只排队，不递归调用阻塞 RPC。

使用 `BridgeWorker(transport, receiver_id="board-a")` 时，通过 `worker.call("enable_logical_keys")` 启用，`next_event()` 的 `("key_event", (event,))` 即统一事件。能力与信息通过 `worker.call("has_key", Key.UP)`、`worker.call("input_info")` 查询。原始 `keys` 回调仍保留，应用不要重复处理两套按键事件。

## .NET

在 BridgeClient 完成打开/握手后创建可选事件包装器：

```csharp
await using var input = new RemoteInputSession(client, "board-a");
await foreach (var e in input.Events.ReadAllAsync(cancellationToken))
{
    if (e.Key is { Kind: "down" } key && key.Key == (ushort)RemoteKey.UP)
        HandleUp(key.Source);
    if (e.Raw is { } raw)
        HandleRawEvent(raw); // 原有语音/管理事件处理
}
```

`input.Keys.HasKey(RemoteKey.UP)`、`input.Keys.Source` 和 `input.Keys.Catalog` 提供能力和身份。`InputProfiles.Layout(model)` 提供可选布局。

包装器独占读取 `client.Events`，应用改读 `input.Events`，不能同时从两个 reader 抢事件。包装器转发原始事件，但不拥有底层客户端的生命周期。读取通道抛错或结束时释放接收器输入；目录错误、队列溢出会显式终止通道，不能静默丢按键。`Completion` 表示后台循环已退出，错误从事件通道读取。

## C

`client/c/rbp_input.h` / `.c` 提供无分配的状态转换器；与自己的 transport 一起编译。C 包没有 RPC 客户端，因此调用者负责取得 KEY_CATALOG，并将按 slot 排列的 key_id 和来源交给 `rbp_input_install`。之后将 KEYS_STATE / KEYS_SNAPSHOT 字段交给 `rbp_input_feed`，断线时调用 `rbp_input_reset`。

使用 `rbp_input_has_key` 查询能力；事件携带完整来源。snapshot 的当前集合由 reducer 的 `bits` 和 `key_ids` 给出。所有调用串行化，回调数据仅在回调期间有效，不可重入 reducer。

## 物理布局

共用元数据位于 `client/python/rbp/input_profiles.json`，Python 包携带该文件，.NET 构建嵌入同一份文件。布局是独立的展示数据，不参与协议识别。

目前只登记已拍照确认的联通布局：归一化坐标表示大致键中心，不是精确尺寸。灰色 TV 键在物理布局中存在，但当前 BLE 键目录不包含它；不能据此断定它一定是红外专用。小米布局尚未登记，返回空值，应用可退回按语义排列。不要用布局表代替 `has_key` 能力判断。

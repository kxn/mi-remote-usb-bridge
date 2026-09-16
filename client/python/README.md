# Python SDK

Install with `python -m pip install ./client/python` from the repository root. Import `rbp.client.BridgeClient` or `rbp.worker.BridgeWorker`. No Qt dependency. See [host guide](../../docs/host-sdk-demo.md).

## Optional Unicom ICO audio

Codec 2/revision 1 support and native dependency setup: [ICO decoder](../c/ico/README.md). Python/.NET PCM helpers preserve PCM16LE output; C consumers can use the ICO-enabled generic rbp_decoder build; the original IMA structure prefix remains compatible. Negotiate only codecs available in the installed host library.
# 统一按键接口

`enable_logical_keys()` 自动取得键目录，`on_key_event` 输出带来源的统一按下/松开事件；`has_key(Key.UP)` 查询当前能力。详见 [统一按键、型号与布局](../../docs/logical-input.md)。

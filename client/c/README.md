# C audio decoder

Caller-owned C ABI for complete compressed coding units. This is not a complete serial/session SDK. Include protocol/include and compile rbp_decoder.c plus ima_decoder.c; see rbp_decoder.h and the [host guide](../../docs/host-sdk-demo.md).

## Optional Unicom ICO audio

Codec 2/revision 1 support and native dependency setup: [ICO decoder](ico/README.md). Python/.NET PCM helpers preserve PCM16LE output; C consumers can use the ICO-enabled generic rbp_decoder build; the original IMA structure prefix remains compatible. Negotiate only codecs available in the installed host library.
# 统一按键接口

`rbp_input.h` / `.c` 将键目录和按键位图转换为带来源的逻辑事件，无动态分配。C 调用者负责 RPC 和目录加载。详见 [统一按键、型号与布局](../../docs/logical-input.md)。

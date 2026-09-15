# 当前依赖（0.6.5）

- 固件：C、WCH CH58x BLE SDK（头版本 V2.10）、TMOS、USB CDC。
- 交叉构建：MounRiver RISC-V GCC 8.2.0，rv32imac/ilp32；主机测试使用 GNU Make 和 C11 编译器。
- Python SDK：Python 3.11+、pySerial 3.5，无 Qt 依赖。
- GUI：PySide6 6.8.3；macOS PyObjC Quartz 11.0。
- 解码：自有 Python/C IMA ADPCM。

版本固定在 demo/requirements.txt、client/python/pyproject.toml、references/sdk-lock.json。SDK 构建子集随源码提供，完整工具链和研究资料使用固定 URL 下载。

[第三方声明](../THIRD_PARTY_NOTICES.md) · [SDK lock](../references/sdk-lock.json) · [参考索引](../references/README.md)

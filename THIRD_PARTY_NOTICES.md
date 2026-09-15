# Third-party components

Project-owned firmware, protocol, host libraries, demos and tests are MIT licensed.

## WCH CH58x SDK

`firmware/wch/vendor/` is a build subset of [openwch/ch583](https://github.com/openwch/ch583/tree/bd508ad7ceed48377619837051412a651952857f), revision `bd508ad7ceed48377619837051412a651952857f`.
The upstream Apache-2.0 license is preserved in `firmware/wch/vendor/LICENSE`.
File-level copyright and conditions remain in the original sources and headers.
In particular, the BLE library/header states that the software, modified or not,
and binary are for microcontrollers manufactured by Nanjing Qinheng Microelectronics.
The project MIT license does not replace these conditions or relicense vendor binaries.
`firmware/wch/bridge.ld` derives from the vendor linker script.
Changes are documented in [vendor-patches](docs/vendor-patches.md).

## Host dependencies

pySerial 3.5 uses its BSD license. PySide6/Qt 6.8.3 and optional macOS PyObjC
retain their upstream licenses. They are installed separately, not vendored into
this source distribution. A bundled application distribution must include the
licenses and satisfy the terms of the Qt modules it actually distributes.

## Research references

Bluetooth/USB/ATVV specifications, complete reference repositories, IDE installers,
and toolchains are not redistributed in this repository. Pinned links are in
`references/sources.json`. Research copies retain their own licenses; downloading
them does not relicense them under MIT. GPL reference projects were used for
comparison, not linked into the shipped firmware or host library.

This is an independent project, not an official Xiaomi or WCH product.

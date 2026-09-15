# 0.6.5

Release firmware: DEBUG=0, SDK_RX_PROBE=0, HOST_VOICE_START=0. RBP/3 only.

The published binary is byte-for-byte identical to the flashed 0.6.5 firmware.
Adds bounded original error records to GET_STATS without enabling debug probes.
GUI and CLI collect source, stage, raw status, context, count, board time and eviction count.
GUI provides manual scan/select/pair and a release Diagnostics tab.

Hardware: CH582F + RC003 on Windows; 0.6.5 flashed and diagnostics received, user confirmed normal use.
Pairing/buttons were verified on 0.6.4; longer recordings/reconnection on preceding versions.
Not a claim that every earlier RF scenario was repeated on 0.6.5. macOS hardware not yet verified.

See manifest.json for SHA-256, sizes and source hashes. Toolchains, recordings and research caches are excluded.

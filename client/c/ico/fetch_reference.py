#!/usr/bin/env python3
"""Fetch the ITU-T G.722.1 fixed-point reference decoder (as bundled by pjproject).

The ICO voice format of the Unicom remote is standard G.722.1 (7 kHz mode,
16 kbit/s) plus a small obfuscation wrapper, so the public reference decoder
is all we need. Files are fetched from the pjsip/pjproject GitHub repository
(third_party/g7221) and SHA-256 verified so builds are reproducible.

Usage:  python fetch_reference.py [--out g7221]
"""
import argparse
import hashlib
import sys
import urllib.request

BASE = "https://raw.githubusercontent.com/pjsip/pjproject/master/third_party/g7221/"

FILES = {
    "common/basic_op.c":  "38882c3d8118153aa691095482c14f10ee632ffae2529c11fd0dfb8e82f245a3",
    "common/basic_op.h":  "a404bc23a655942a8933fdcc477c348b99a375e5f52e0bf90dd2684ccdef8261",
    "common/basic_op_i.h": "bfa51571aba18a44e907b1252ee43a7579c16be3743b6cf9e2fd413194fd17c8",
    "common/common.c":    "695e41d4bfd580e2aaf88b4e90bcccdded696776548b94d9eaba6a6eae9c1075",
    "common/config.h":    "735afae05804a4e955651e7c4e1578c34f1538d7241a45ea719d14695bc1c6a7",
    "common/count.h":     "ae33c0c077ece3cf7d90fa51b299136463d4170fcef41bf048c47d84c5639b0b",
    "common/defs.h":      "a83191fbb4ad1050724d37a973db8b8273f7c89a8f82c440864c28fa90a633f8",
    "common/huff_def.h":  "c34ff1932bd8153dab7e6e8ae3ce42a4f0af7532ca32bf067ae2c9bdfd94cc43",
    "common/huff_tab.c":  "bd4b32e283a50e94b71540cfcbd68b0cb1eb76d984fededf7e9118e7337a4536",
    "common/huff_tab.h":  "fdbe0cd53001d2c6f558d141322f82fdde1f480e9f042a9d1375fa00b799c57b",
    "common/tables.c":    "5e6f7f4ee523c04107eba044a835669c60420b4d1b8d95dc72d22f86f0279f28",
    "common/tables.h":    "c4851ab902497399c4c941351d4cc60115b4442f0cbd888bd457f2ca73089585",
    "common/typedef.h":   "7589d83d5f10ac7e91cdab94d44daba2a4720fa43c9f8c00c349719e5642841d",
    "decode/coef2sam.c":  "6bed16aca28d29031c38cea0a07cf40421b144a3ace89cef5279f35dc46c3880",
    "decode/dct4_s.c":    "bab1d85be212a3733ee4f107dbf4b32b77cacf862165082121a7248f3b8188e9",
    "decode/dct4_s.h":    "1b1d50325e957576d8563c455fcf571bf21bb40cac184cd349d80332d912a65a",
    "decode/decoder.c":   "5b501d68a180c47e0d057ce00c2fdd5b7a7742b913e41aaa01e353f3bd3a35f5",
}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="g7221")
    args = ap.parse_args()

    import pathlib
    out = pathlib.Path(args.out)
    ok = True
    for rel, want in FILES.items():
        dst = out / rel
        if dst.exists():
            got = hashlib.sha256(dst.read_bytes()).hexdigest()
            if got == want:
                print(f"ok      {rel}")
                continue
            print(f"re-fetch {rel} (hash mismatch)")
        else:
            print(f"fetch   {rel}")
        url = BASE + rel
        try:
            data = urllib.request.urlopen(url, timeout=60).read()
        except Exception as exc:  # noqa: BLE001
            print(f"  FAILED: {exc}")
            ok = False
            continue
        got = hashlib.sha256(data).hexdigest()
        if got != want:
            print(f"  FAILED: sha256 {got} != {want}")
            ok = False
            continue
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_bytes(data)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

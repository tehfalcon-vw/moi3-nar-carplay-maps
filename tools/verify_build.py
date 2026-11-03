#!/usr/bin/env python3
import struct
import subprocess
import sys
from pathlib import Path

HOOKS = (
    b"AirPlayCopyServerInfo",
    b"AirPlayReceiverSessionSetup",
    b"AirPlayReceiverSessionTearDown",
    b"AES_CBCFrame_Init",
    b"ScreenStreamCreate",
    b"ScreenStreamStop",
    b"ScreenStreamSetWidthHeight",
)


def fail(message):
    raise SystemExit(f"verification failed: {message}")


path = Path(sys.argv[1])
data = path.read_bytes()
if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
    fail("not a 64-bit little-endian ELF")
if struct.unpack_from("<H", data, 18)[0] != 183:
    fail("ELF is not AArch64")
for symbol in HOOKS:
    if symbol + b"\0" not in data:
        fail(f"missing dynamic hook name {symbol.decode()}")
if b"libdl.so.2\0" not in data:
    fail("libdl.so.2 is not a dependency")
versions = sorted(set(part.split(b"\0", 1)[0] for part in data.split(b"GLIBC_")[1:]))
if any(version not in (b"2.17",) for version in versions):
    fail(f"unexpected glibc versions: {versions}")
print(f"verified {path}: AArch64, seven hooks, libdl.so.2, GLIBC_2.17")

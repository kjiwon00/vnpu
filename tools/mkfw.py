#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""NPU 펌웨어 이미지를 만든다 (DESIGN.md 7절).

장치는 헤더와 CRC만 검사하므로 payload는 아무 내용이나 된다.

    ./mkfw.py npu_fw.bin                  # 버전 1.0, payload 4KiB
    ./mkfw.py npu_fw.bin --version 1.2 --payload-size 65536
"""
import argparse
import struct
import zlib

FW_MAGIC = 0x5746504E       # "NPFW"
HDR_SIZE = 32
MAX_SIZE = 1 << 20


def make_image(fw_version=0x00010000, hw_major=1, payload=b""):
    """헤더(32바이트) + payload를 bytes로 돌려준다."""
    hdr = struct.pack("<5I12x", FW_MAGIC, fw_version, hw_major,
                      HDR_SIZE + len(payload), zlib.crc32(payload))
    assert len(hdr) == HDR_SIZE
    return hdr + payload


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("output")
    ap.add_argument("--version", default="1.0", help="major.minor (기본 1.0)")
    ap.add_argument("--hw-major", type=int, default=1)
    ap.add_argument("--payload-size", type=int, default=4096)
    args = ap.parse_args()

    major, minor = (int(x) for x in args.version.split("."))
    payload = bytes(i * 31 & 0xFF for i in range(args.payload_size))
    image = make_image(major << 16 | minor, args.hw_major, payload)
    if len(image) > MAX_SIZE:
        ap.error("image larger than 1MiB")
    with open(args.output, "wb") as f:
        f.write(image)
    print(f"{args.output}: {len(image)} bytes, version {major}.{minor}")


if __name__ == "__main__":
    main()

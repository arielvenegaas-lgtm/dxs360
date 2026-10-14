"""Verify a FreeChainXenon output is a genuine Xbox 360 XEX2 with an embedded ELF.

Usage: python3 ci/verify_xex.py <path-to.xex>
"""
import sys

if len(sys.argv) != 2:
    print("usage: verify_xex.py <path-to.xex>", file=sys.stderr)
    sys.exit(2)

data = open(sys.argv[1], "rb").read()

if data[:4] != b"XEX2":
    print("error: not a XEX2 file (magic=%r)" % data[:4], file=sys.stderr)
    sys.exit(1)

if len(data) <= 0x1000:
    print("error: XEX suspiciously small (%d bytes)" % len(data), file=sys.stderr)
    sys.exit(1)

if b"\x7fELF" not in data:
    print("error: embedded PPC ELF not found in XEX", file=sys.stderr)
    sys.exit(1)

print("XEX2 magic OK; embedded ELF OK; size=%d bytes" % len(data))

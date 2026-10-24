#!/usr/bin/env python3
"""data/skillhub.icns from the PNG masters in branding/png (run by branding/export.sh)."""
import os
import struct

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
# type, pixel size of the PNG: icp4/5/6 = 16/32/64, ic07/08/09 = 128/256/512, ic11/12/13 = the @2x of 16/32/128
chunks = [(b"icp4", 16), (b"icp5", 32), (b"icp6", 64), (b"ic07", 128), (b"ic08", 256), (b"ic09", 512),
          (b"ic11", 32), (b"ic12", 64), (b"ic13", 256)]
body = b""
for tag, px in chunks:
    with open(os.path.join(root, "branding/png/skillhub-%d.png" % px), "rb") as f:
        data = f.read()
    body += tag + struct.pack(">I", 8 + len(data)) + data
with open(os.path.join(root, "data/skillhub.icns"), "wb") as f:
    f.write(b"icns" + struct.pack(">I", 8 + len(body)) + body)

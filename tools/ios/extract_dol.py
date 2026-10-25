"""Copies main.dol out of your own GameCube disc image (plain ISO/GCM) for the iOS build.

    py tools/ios/extract_dol.py "<disc>.iso" [out_dir]

For RVZ/WIA/CISO images, extract main.dol with Dolphin instead (right-click the game > Properties >
Filesystem > right-click the disc > Extract System Data; main.dol is in sys/), or convert the image
to ISO first. The file is checked against the supported releases (USA GLME01 / Europe GLMP01 rev 0).
"""
import hashlib
import os
import struct
import sys


def dol_size(head: bytes) -> int:
    offs = struct.unpack(">18I", head[0x00:0x48])
    sizes = struct.unpack(">18I", head[0x90:0xD8])
    return max(o + s for o, s in zip(offs, sizes) if s)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    src = sys.argv[1]
    out_dir = sys.argv[2] if len(sys.argv) > 2 else "."
    with open(src, "rb") as f:
        header = f.read(0x440)
        game_id = header[0:6].decode("ascii", "replace")
        if header[0x1C:0x20] != b"\xc2\x33\x9f\x3d":
            print(f"{src}: not a plain GameCube ISO/GCM (no disc magic). Extract main.dol with Dolphin instead.")
            return 1
        dol_off = struct.unpack(">I", header[0x420:0x424])[0]
        f.seek(dol_off)
        head = f.read(0x100)
        size = dol_size(head)
        f.seek(dol_off)
        dol = f.read(size)
    sha = hashlib.sha1(dol).hexdigest()
    print(f"disc {game_id}, main.dol at 0x{dol_off:X}, {size} bytes, SHA-1 {sha}")
    if game_id not in ("GLME01", "GLMP01"):
        print("warning: not Luigi's Mansion USA (GLME01) or Europe (GLMP01); the build will refuse it")
    os.makedirs(out_dir, exist_ok=True)
    out = os.path.join(out_dir, "main.dol")
    with open(out, "wb") as f:
        f.write(dol)
    print(f"wrote {os.path.abspath(out)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

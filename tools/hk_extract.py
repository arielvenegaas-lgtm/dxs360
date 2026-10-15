#!/usr/bin/env python3
"""
hk_extract.py - extract a single sprite from a LOCAL Hollow Knight install into a
portable .spr image for the native runtime.

This tool contains NO game assets. It reads whatever path you point it at and
writes the converted result wherever you ask. Nothing is ever uploaded.

.spr format (little endian):
    char   magic[4]   = 'HKS1'
    u32    version    = 1
    u32    width
    u32    height
    u32    format     = 0  (RGBA8888, straight alpha, top-left origin, row-major)
    u8     pixels[width*height*4]

Usage:
    python3 hk_extract.py --data "<...>/hollow_knight_Data" --list
    python3 hk_extract.py --data "<...>/hollow_knight_Data" --grep knight --list
    python3 hk_extract.py --data "<...>/hollow_knight_Data" --sprite knight_icon \
                          --outdir "<...>/hk_port/assets"
"""
import argparse
import glob
import os
import struct
import sys

try:
    import UnityPy
except Exception as e:  # noqa
    sys.exit("UnityPy is required: pip install UnityPy  (%s)" % e)


def load_envs(data_dir, limit=None):
    files = [os.path.join(data_dir, "resources.assets")]
    files += sorted(glob.glob(os.path.join(data_dir, "sharedassets*.assets")))
    files += sorted(glob.glob(os.path.join(data_dir, "level*")), key=_level_key)
    if limit:
        files = files[:limit]
    for f in files:
        if os.path.isfile(f):
            yield f


def _level_key(path):
    base = os.path.basename(path)
    try:
        return (0, int(base[5:]))
    except ValueError:
        return (1, base)


def iter_sprites(data_dir, limit=None):
    for f in load_envs(data_dir, limit):
        try:
            env = UnityPy.load(f)
        except Exception:
            continue
        for o in env.objects:
            if o.type.name != "Sprite":
                continue
            try:
                yield os.path.basename(f), o.read()
            except Exception:
                continue


def cmd_list(data_dir, grep, limit):
    n = 0
    for src, d in iter_sprites(data_dir, limit):
        if grep and grep.lower() not in d.m_Name.lower():
            continue
        print("%-24s %s" % (src, d.m_Name))
        n += 1
    print("# %d sprites" % n)


def write_spr(path, img):
    img = img.convert("RGBA")
    w, h = img.size
    with open(path, "wb") as fp:
        fp.write(b"HKS1")
        fp.write(struct.pack("<IIII", 1, w, h, 0))
        fp.write(img.tobytes())
    return w, h


def cmd_extract(data_dir, name, outdir, limit):
    os.makedirs(outdir, exist_ok=True)
    for src, d in iter_sprites(data_dir, limit):
        if d.m_Name != name:
            continue
        img = d.image
        spr = os.path.join(outdir, name + ".spr")
        png = os.path.join(outdir, name + ".png")
        w, h = write_spr(spr, img)
        img.convert("RGBA").save(png)
        print("found in %s" % src)
        print("wrote %s (%dx%d, %d bytes)" % (spr, w, h, os.path.getsize(spr)))
        print("wrote %s" % png)
        return 0
    print("sprite %r not found" % name, file=sys.stderr)
    return 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", required=True, help="path to hollow_knight_Data")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--grep", default=None)
    ap.add_argument("--sprite", default=None)
    ap.add_argument("--outdir", default=".")
    ap.add_argument("--limit", type=int, default=None,
                    help="max number of container files to scan")
    a = ap.parse_args()
    if a.list:
        cmd_list(a.data, a.grep, a.limit)
        return 0
    if not a.sprite:
        ap.error("--sprite NAME or --list is required")
    return cmd_extract(a.data, a.sprite, a.outdir, a.limit)


if __name__ == "__main__":
    sys.exit(main())

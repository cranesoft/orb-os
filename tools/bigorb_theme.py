#!/usr/bin/env python3
"""Turn a 466 px Orb theme (.orb) into an 800 px Big Orb theme.

This is the stand-in until Big Orb Studio bakes themes at 800 natively. It does three jobs:

  1. Every PNG is resized by 800/466 (Lanczos). Full-screen art becomes 800x800; hands,
     blips and strips keep their proportion to the dial.
  2. Every pixel number in the *_style.json files is multiplied by the same factor:
     positions, radii, margins, widths, pivots, font sizes. Colours, opacities, angles,
     counts, timings and units are left alone (NON_PIXEL below).
  3. theme.json gets "canvas": 800, a new slug (so the 466 and 800 copies never share a
     folder) and a fresh assetsHash (so the firmware never reuses 466 art it cached).

Every PNG written is then decoded with the firmware's own PNGdec (tools/pngcheck, built on
first use with clang++) and compared with Pillow pixel for pixel. A mismatch stops the run:
lib/PNGdec/ORB_PATCH.md is the story of why that check exists.

Theme fonts (font_*.bin) are rebuilt at 800 sizes. A .bin records its pixel size and its
characters but not its typeface, so each one is identified by rebuilding every typeface the
theme's studio.json names, at the original size, and keeping the one that comes out byte for
byte identical. That typeface is then rebuilt at round(size * 800/466). Typefaces come from
Google Fonts' own repository (github.com/google/fonts) and are cached in ~/.cache/bigorb-fonts;
fonts are rasterised with lv_font_conv 1.5.3 through npx. A font that cannot be identified is
copied unchanged, with a warning.

    python3 tools/bigorb_theme.py ~/Downloads/Aviator.orb            # writes Aviator-800.orb
    python3 tools/bigorb_theme.py ~/Downloads/Aviator.orb --dir sim/sdcard/themes
"""
import argparse
import glob
import io
import json
import os
import struct
import subprocess
import sys
import tempfile
import zlib

from PIL import Image

MAGIC = b"ORBTHM01"
SRC_PX = 466
DST_PX = 800
K = DST_PX / SRC_PX

# Keys whose numbers are NOT screen pixels. Matched on the last path component, case-insensitive
# on these substrings, so "windTitleCol", "blipAltJet" and "sweepOpacity" are all caught.
NON_PIXEL_SUBSTR = (
    "color", "col", "opa", "opacity", "align", "deg", "angle", "count", "order", "secs",
    "turns", "minutes", "seconds", "alt", "rangekm", "maxaircraft", "speed", "steps",
    "defaultsel", "plate", "blend", "style", "onscreen", "scale", "fade", "dim", "place",
    "format", "bg", "show", "zoom", "rest",
)
# Exact names that would otherwise look like pixels.
NON_PIXEL_EXACT = {"v", "id"}


def is_pixel_key(key):
    k = key.lower()
    if k in NON_PIXEL_EXACT:
        return False
    return not any(s in k for s in NON_PIXEL_SUBSTR)


def scale_json(o, key=""):
    if isinstance(o, dict):
        return {k: scale_json(v, k) for k, v in o.items()}
    if isinstance(o, list):
        return [scale_json(v, key) for v in o]
    if isinstance(o, bool) or not isinstance(o, (int, float)):
        return o
    if not is_pixel_key(key):
        return o
    if isinstance(o, int):
        return int(round(o * K))
    return round(o * K, 3)


def unpack(data):
    if data[:8] != MAGIC:
        raise SystemExit(f"not an Orb theme file (magic is {data[:8]!r})")
    at = 8
    (n,) = struct.unpack_from("<H", data, at); at += 2
    slug = data[at:at + n].decode(); at += n
    (count,) = struct.unpack_from("<H", data, at); at += 2
    files = []
    for _ in range(count):
        (n,) = struct.unpack_from("<H", data, at); at += 2
        name = data[at:at + n].decode(); at += n
        (size,) = struct.unpack_from("<I", data, at); at += 4
        files.append((name, data[at:at + size])); at += size
    return slug, files


def pack(slug, files):
    out = bytearray(MAGIC)
    s = slug.encode()
    out += struct.pack("<H", len(s)) + s
    out += struct.pack("<H", len(files))
    for name, data in files:
        n = name.encode()
        out += struct.pack("<H", len(n)) + n + struct.pack("<I", len(data)) + data
    return bytes(out)


def scale_png(data):
    im = Image.open(io.BytesIO(data))
    im.load()
    if im.mode not in ("RGBA", "RGB", "LA", "L"):
        im = im.convert("RGBA")
    w, h = im.size
    nw, nh = max(1, round(w * K)), max(1, round(h * K))
    # A full-screen layer must land on the panel exactly, never 799 or 801.
    if w == SRC_PX:
        nw = DST_PX
    if h == SRC_PX:
        nh = DST_PX
    big = im.resize((nw, nh), Image.LANCZOS)
    buf = io.BytesIO()
    big.save(buf, "PNG", compress_level=6)
    return buf.getvalue()


REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PNGCHECK_SRC = os.path.join(REPO, "tools", "pngcheck", "pngcheck.cpp")
PNGDEC = os.path.join(REPO, "lib", "PNGdec", "src")


def pngcheck_tool():
    """Build tools/pngcheck against lib/PNGdec once, into the system temp dir."""
    out = os.path.join(tempfile.gettempdir(), "orb_pngcheck")
    newest = max(os.path.getmtime(f) for f in [PNGCHECK_SRC] + glob.glob(PNGDEC + "/*"))
    if os.path.exists(out) and os.path.getmtime(out) >= newest:
        return out
    flags = ["-O2", "-D__LINUX__", "-DPNG_MAX_BUFFERED_PIXELS=8192", "-I" + PNGDEC]
    objs = []
    for c in ("adler32", "crc32", "infback", "inffast", "inflate", "inftrees", "zutil"):
        o = os.path.join(tempfile.gettempdir(), f"orb_pngcheck_{c}.o")
        subprocess.run(["clang"] + flags + ["-c", os.path.join(PNGDEC, c + ".c"), "-o", o], check=True)
        objs.append(o)
    subprocess.run(["clang++", "-std=gnu++17"] + flags +
                   [PNGCHECK_SRC, os.path.join(PNGDEC, "PNGdec.cpp")] + objs + ["-o", out], check=True)
    return out


def png_matches_firmware(data):
    """True if PNGdec decodes these PNG bytes to exactly what Pillow sees."""
    with tempfile.TemporaryDirectory() as d:
        src, raw = os.path.join(d, "a.png"), os.path.join(d, "a.rgba")
        with open(src, "wb") as f:
            f.write(data)
        r = subprocess.run([pngcheck_tool(), src, raw], capture_output=True, text=True)
        if r.returncode:
            return False
        got = open(raw, "rb").read()
    ref = Image.open(io.BytesIO(data)).convert("RGBA").tobytes()
    if len(got) != len(ref):
        return False
    # Pixels the firmware skips (alpha under 8) may differ harmlessly.
    for i in range(0, len(ref), 4):
        if ref[i + 3] >= 8 and ref[i:i + 4] != got[i:i + 4]:
            return False
    return True


# ---- theme fonts -------------------------------------------------------------------------
FONT_CACHE = os.path.expanduser("~/.cache/bigorb-fonts")
LV_FONT_CONV = ["npx", "-y", "lv_font_conv@1.5.3"]
HEAD_FMT = "<IHHHhHhHhhHHBBBBBBBBBBhH"   # lv_font_conv bin 'head', after its 8-byte chunk header


def font_head(data):
    f = struct.unpack_from(HEAD_FMT, data, 8)
    return {"size": f[2], "asc": f[3], "desc": f[4], "bpp": f[15], "comp": f[19]}


def font_codepoints(data):
    """Every code point the .bin's cmap table carries, as lv_font_conv -r ranges."""
    at = struct.unpack_from("<I", data, 0)[0]            # cmap follows head
    (_, tag) = struct.unpack_from("<I4s", data, at)
    if tag != b"cmap":
        raise ValueError("no cmap table")
    (n,) = struct.unpack_from("<I", data, at + 8)
    cps = []
    for i in range(n):
        off, start, rlen, _gid, entries, typ = struct.unpack_from("<IIHHHB", data, at + 12 + i * 16)
        if typ in (0, 2):                                 # continuous range
            cps.extend(range(start, start + rlen))
        else:                                             # sparse: u16 deltas from start
            cps.extend(start + struct.unpack_from("<H", data, at + off + 2 * k)[0] for k in range(entries))
    cps = sorted(set(cps))
    ranges, lo = [], None
    for c in cps:
        if lo is None:
            lo = prev = c
        elif c == prev + 1:
            prev = c
        else:
            ranges.append((lo, prev)); lo = prev = c
    if lo is not None:
        ranges.append((lo, prev))
    return ",".join(f"0x{a:X}-0x{b:X}" if a != b else f"0x{a:X}" for a, b in ranges)


def _fetch(url):
    import urllib.request
    with urllib.request.urlopen(url, timeout=30) as r:
        return r.read()


def typeface_file(slug, weight):
    """Local TTF for a Google Fonts slug ("special-elite") at a weight, or None."""
    os.makedirs(FONT_CACHE, exist_ok=True)
    key = os.path.join(FONT_CACHE, f"{slug}-{weight}.ttf")
    if os.path.exists(key):
        return key
    folder = slug.replace("-", "")
    listing = None
    for lic in ("ofl", "apache", "ufl"):
        try:
            listing = json.loads(_fetch(f"https://api.github.com/repos/google/fonts/contents/{lic}/{folder}"))
            break
        except Exception:
            continue
    if not listing:
        return None
    ttfs = [e for e in listing if e["name"].endswith(".ttf") and "Italic" not in e["name"]]
    names = {300: "Light", 400: "Regular", 500: "Medium", 600: "SemiBold", 700: "Bold", 800: "ExtraBold"}
    static = [e for e in ttfs if e["name"].endswith(f"-{names.get(weight, 'Regular')}.ttf")]
    variable = [e for e in ttfs if "[" in e["name"]]
    pick = (static or variable or [None])[0]
    if not pick:
        return None
    raw = _fetch(pick["download_url"])
    if "[" in pick["name"]:
        # A variable font: pin the weight (and leave every other axis at its default).
        from fontTools.ttLib import TTFont
        from fontTools.varLib import instancer
        tt = TTFont(io.BytesIO(raw))
        axes = {a.axisTag: a.defaultValue for a in tt["fvar"].axes}
        if "wght" in axes:
            axes["wght"] = weight
        tt = instancer.instantiateVariableFont(tt, axes)
        tt.save(key)
    else:
        with open(key, "wb") as f:
            f.write(raw)
    return key


def lv_font_bin(ttf, size, ranges, bpp):
    with tempfile.TemporaryDirectory() as d:
        out = os.path.join(d, "f.bin")
        r = subprocess.run(LV_FONT_CONV + ["--bpp", str(bpp), "--no-compress", "--no-prefilter",
                                           "--size", str(size), "--font", ttf, "-r", ranges,
                                           "--format", "bin", "-o", out],
                           capture_output=True, text=True)
        if r.returncode or not os.path.exists(out):
            return None
        return open(out, "rb").read()


def theme_typefaces(files):
    """(slug, weight) pairs the theme's Studio project names, plus Inter, the house face."""
    studio = next((d for n, d in files if os.path.basename(n) == "studio.json"), None)
    # Inter and Montserrat are the faces Studio uses where a design picks none (the compiled
    # ladder is Montserrat), so they are always worth a try.
    found = {("inter", 400), ("montserrat", 400), ("montserrat", 500)}
    def walk(o):
        if isinstance(o, dict):
            if isinstance(o.get("font"), str) and o["font"]:
                found.add((o["font"], int(o.get("weight") or 400)))
            for v in o.values():
                walk(v)
        elif isinstance(o, list):
            for v in o:
                walk(v)
    if studio:
        walk(json.loads(studio))
    return sorted(found)


def rebuild_font(name, data, candidates):
    head = font_head(data)
    if head["comp"] != 0:
        print(f"  {name}: compressed font, copied unchanged")
        return data
    ranges = font_codepoints(data)
    new_size = int(round(head["size"] * K))
    # Exact first: the same typeface file Studio used rebuilds the .bin byte for byte. Failing
    # that, the closest: same ascent and descent, and the nearest byte count within 8%. That
    # catches a face Google has since revised (Inter 3 to 4, a static cut becoming a variable
    # font), where the shapes are the designer's choice and only the file has moved on.
    best = None
    for slug, weight in candidates:
        ttf = typeface_file(slug, weight)
        if not ttf:
            continue
        trial = lv_font_bin(ttf, head["size"], ranges, head["bpp"])
        if not trial:
            continue
        if trial == data:
            best = (0, slug, weight, ttf, "exact")
            break
        th = font_head(trial)
        if (th["asc"], th["desc"]) == (head["asc"], head["desc"]):
            drift = abs(len(trial) - len(data)) / len(data)
            if drift <= 0.08 and (best is None or drift < best[0]):
                best = (drift, slug, weight, ttf, f"close, {drift:.1%} apart")
    if best:
        big = lv_font_bin(best[3], new_size, ranges, head["bpp"])
        if big:
            print(f"  {name}: {best[1]} {best[2]} ({best[4]}), {head['size']} -> {new_size} px")
            return big
    print(f"  {name}: typeface not identified, copied unchanged (will read small)")
    return data


def convert(slug, files):
    new_slug = (slug[:-4] if slug.endswith("-466") else slug) + "-800"
    out = []
    candidates = None
    for name, data in files:
        base = os.path.basename(name)
        if base.endswith(".png"):
            data = scale_png(data)
            if not png_matches_firmware(data):
                raise SystemExit(f"{base}: PNGdec does not decode this PNG exactly. "
                                 "Is lib/PNGdec the patched copy? (lib/PNGdec/ORB_PATCH.md)")
        elif base.endswith("_style.json"):
            data = (json.dumps(scale_json(json.loads(data)), indent=2) + "\n").encode()
        elif base.startswith("font_") and base.endswith(".bin"):
            if candidates is None:
                candidates = theme_typefaces(files)
                print("fonts (candidates: " + ", ".join(f"{a} {b}" for a, b in candidates) + ")")
            data = rebuild_font(base, data, candidates)
        out.append((name, data))

    # theme.json last: it carries hashes of everything else.
    hashes = {}
    for name, data in out:
        hashes[os.path.basename(name)] = zlib.crc32(data) & 0xFFFFFFFF
    final = []
    for name, data in out:
        if os.path.basename(name) == "theme.json":
            j = json.loads(data)
            j["canvas"] = DST_PX
            j["name"] = j.get("name", slug)
            fh = j.get("fileHashes", {})
            for k in list(fh):
                if k in hashes:
                    fh[k] = hashes[k]
            j["fileHashes"] = fh
            j["assetsHash"] = zlib.crc32(
                b"".join(d for n, d in out if os.path.basename(n) != "theme.json")) & 0xFFFFFFFF
            data = (json.dumps(j, indent=2) + "\n").encode()
        final.append((name, data))
    return new_slug, final


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("orb", help="466 px .orb theme file")
    ap.add_argument("--out", help="output .orb (default: <name>-800.orb next to the input)")
    ap.add_argument("--dir", help="also unpack into <dir>/<slug>/ (e.g. sim/sdcard/themes)")
    a = ap.parse_args()

    src = os.path.expanduser(a.orb)
    slug, files = unpack(open(src, "rb").read())
    manifest = next((d for n, d in files if os.path.basename(n) == "theme.json"), None)
    if manifest and json.loads(manifest).get("canvas", SRC_PX) != SRC_PX:
        raise SystemExit(f"{src} is already a {json.loads(manifest)['canvas']} px theme")

    new_slug, out = convert(slug, files)
    dst = a.out or os.path.splitext(src)[0] + "-800.orb"
    with open(dst, "wb") as f:
        f.write(pack(new_slug, out))
    print(f"wrote {dst}  (slug {new_slug}, {len(out)} files)")

    if a.dir:
        d = os.path.join(a.dir, new_slug)
        os.makedirs(d, exist_ok=True)
        for name, data in out:
            with open(os.path.join(d, os.path.basename(name)), "wb") as f:
                f.write(data)
        print(f"unpacked into {d}")


if __name__ == "__main__":
    main()

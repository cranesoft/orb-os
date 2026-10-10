#!/bin/bash
# Regenerate src/bigorb_fonts/: LVGL's built-in Montserrat ladder, drawn at Big Orb sizes.
#
# Same recipe LVGL used for its own lv_font_montserrat_*.c (Montserrat-Medium + the
# FontAwesome symbols LV_SYMBOL_* needs, bpp 4, uncompressed), with each rung n drawn at
# ORB_PX(n) = round(n * 800 / 466) px. lv_conf.h links these under LVGL's own names on the
# 800 px build only. Needs node (npx) and a built env so .pio/libdeps holds lvgl.
#
#   tools/bigorb_fonts.sh
set -euo pipefail
cd "$(dirname "$0")/.."
FD=$(ls -d .pio/libdeps/*/lvgl/scripts/built_in_font | head -1)
SYMS=$(grep -o "FontAwesome5-Solid+Brands+Regular.woff -r [0-9,]*" "$FD/../../src/font/lv_font_montserrat_16.c" | awk '{print $3}')
mkdir -p src/bigorb_fonts
for n in 12 14 16 18 20 22 24 26 28 32 36 40 44 48; do
  px=$(python3 -c "print(int($n*800/466+0.5))")
  out=src/bigorb_fonts/lv_font_montserrat_$n.c
  npx -y lv_font_conv@1.5.3 --no-compress --no-prefilter --bpp 4 --size "$px" \
    --font "$FD/Montserrat-Medium.ttf" -r 0x20-0x7F,0xB0,0x2022 \
    --font "$FD/FontAwesome5-Solid+Brands+Regular.woff" -r "$SYMS" \
    --format lvgl --force-fast-kern-format --lv-include lvgl.h -o "$out" >/dev/null
  # Swap LVGL's own enable guard for the Big Orb one, and drop the build paths from the header.
  python3 -I - "$out" "$n" <<'PY'
import re, sys
p, n = sys.argv[1], sys.argv[2]
s = open(p).read()
s = re.sub(r' \* Opts: .*\n', " * Opts: lv_font_conv 1.5.3, LVGL's own built-in recipe (Montserrat-Medium + FontAwesome5\n *       symbols, bpp 4, no compress) at ORB_PX(" + n + ") px. Regenerate with\n *       tools/bigorb_fonts.sh.\n", s)
s = s.replace(f"#ifndef LV_FONT_MONTSERRAT_{n}\n#define LV_FONT_MONTSERRAT_{n} 1\n#endif\n\n#if LV_FONT_MONTSERRAT_{n}\n",
    "/* Big Orb only. LVGL's own lv_font_montserrat_" + n + " is switched off in lv_conf.h for the 800 px\n"
    " * build and this one, drawn about 1.7x larger, takes its name, so every screen that asks for\n"
    " * \"Montserrat " + n + "\" gets the same look at the bigger panel without a code change. */\n"
    "#if defined(ORB_SCREEN_PX) && ORB_SCREEN_PX == 800\n")
s = s.replace(f"#endif /*#if LV_FONT_MONTSERRAT_{n}*/", "#endif /* ORB_SCREEN_PX == 800 */")
open(p, "w").write(s)
PY
  echo "montserrat $n -> $px px"
done

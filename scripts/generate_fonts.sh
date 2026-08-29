#!/usr/bin/env bash
# Generate LVGL Chinese fonts for fish-tank-terminal (4bpp + FontAwesome symbols).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FONT_DIR="$ROOT/main/ui/fonts"
BUILD_DIR="$ROOT/build/fonts"
TTC="/System/Library/Fonts/STHeiti Medium.ttc"
TTF="$BUILD_DIR/STHeiti.ttf"
FA_WOFF="$ROOT/managed_components/lvgl__lvgl/scripts/built_in_font/FontAwesome5-Solid+Brands+Regular.woff"
OUT24="$FONT_DIR/fish_font_24.c"
OUT36="$FONT_DIR/fish_font_36.c"
SYMS="$BUILD_DIR/ui_symbols.txt"
TITLE_SYMS="$BUILD_DIR/title_symbols.txt"
GB2312="$BUILD_DIR/gb2312_level1.txt"

FA_RANGE="0xF008,0xF00B,0xF00C,0xF00D,0xF011,0xF013,0xF015,0xF019,0xF01C,0xF021,0xF026,0xF027,0xF028,0xF03E,0xF048,0xF04B,0xF04C,0xF04D,0xF051,0xF052,0xF053,0xF054,0xF067,0xF068,0xF06E,0xF070,0xF071,0xF073,0xF074,0xF076,0xF078,0xF079,0xF07B,0xF093,0xF095,0xF0C4,0xF0C5,0xF0C7,0xF0C9,0xF0E0,0xF0E7,0xF0EA,0xF0F3,0xF11C,0xF124,0xF158,0xF1EB,0xF240,0xF241,0xF242,0xF243,0xF244,0xF287,0xF293,0xF2ED,0xF304,0xF55A,0xF7C2,0xF8A2"

mkdir -p "$FONT_DIR" "$BUILD_DIR"

cd "$ROOT"

python3 - <<'PY'
from fontTools.ttLib import TTFont
from pathlib import Path
ttc = Path("/System/Library/Fonts/STHeiti Medium.ttc")
out = Path("build/fonts/STHeiti.ttf")
out.parent.mkdir(parents=True, exist_ok=True)
TTFont(str(ttc), fontNumber=0).save(str(out))
print("saved", out)
PY

python3 - <<'PY'
from pathlib import Path
chars = []
for row in range(0xB0, 0xF8):
    for col in range(0xA1, 0xFF):
        try:
            chars.append(bytes([row, col]).decode("gb2312"))
        except UnicodeDecodeError:
            pass
text = "".join(chars) + "°·…！"
Path("build/fonts/gb2312_level1.txt").write_text(text, encoding="utf-8")
print("gb2312 chars", len(chars))
PY

rg -oN '[\x{4e00}-\x{9fff}]' "$ROOT/main" --glob '!APP/*' --glob '!fonts/*' 2>/dev/null \
  | sed 's/^.*://' | sort -u | tr -d '\n' > "$SYMS" || true
echo 'PIN:°·…！!0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz' >> "$SYMS"

rg -oN '[\x{4e00}-\x{9fff}]' "$ROOT/main/ui/fish_ui.c" "$ROOT/main/ui/wifi_setup.c" 2>/dev/null \
  | sed 's/^.*://' | sort -u | tr -d '\n' > "$TITLE_SYMS" || true
echo 'PIN:°·…！!0123456789-.°C' >> "$TITLE_SYMS"

if [ ! -f "$FA_WOFF" ]; then
  echo "FontAwesome woff not found: $FA_WOFF" >&2
  exit 1
fi

LV_FONT_CONV=""
if command -v lv_font_conv >/dev/null 2>&1; then
  LV_FONT_CONV=lv_font_conv
elif command -v npx >/dev/null 2>&1; then
  LV_FONT_CONV="npx --yes lv_font_conv"
fi
if [ -z "$LV_FONT_CONV" ]; then
  echo "lv_font_conv not found" >&2
  exit 1
fi

gen_font() {
  local size=$1 out=$2 symbols=$3 bpp=$4
  $LV_FONT_CONV \
    --font "$TTF" --size "$size" --bpp "$bpp" --format lvgl --no-compress \
    --range 0x20-0x7F \
    --symbols "${symbols}" \
    --font "$FA_WOFF" --range "$FA_RANGE" \
    -o "$out"
}

gen_font 24 "$OUT24" "$(cat "$GB2312")$(cat "$SYMS")" 2
gen_font 36 "$OUT36" "$(cat "$TITLE_SYMS")" 2

# ESP-IDF component uses "lvgl.h" directly, not "lvgl/lvgl.h".
for f in "$OUT24" "$OUT36"; do
  python3 - "$f" <<'PY'
import sys
from pathlib import Path
p = Path(sys.argv[1])
t = p.read_text()
old = '#ifdef LV_LVGL_H_INCLUDE_SIMPLE\n#include "lvgl.h"\n#else\n#include "lvgl/lvgl.h"\n#endif'
t = t.replace(old, '#include "lvgl.h"')
p.write_text(t)
PY
done

echo "Generated $OUT24 and $OUT36"

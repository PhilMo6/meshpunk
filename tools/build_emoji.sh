#!/usr/bin/env bash
# build_emoji.sh — convert Noto emoji PNGs into the meshpunk .bin format.
#
# Usage: bash tools/build_emoji.sh <in_dir> <out_dir> [size]
#   in_dir:  folder of Noto PNGs (filenames like "emoji_u1f600.png")
#   out_dir: destination for "<hex>.bin" files (no emoji_u prefix)
#   size:    optional pixel size, default 16
#
# Output format (matches the runtime loader in src/emoji_font.c):
#   bytes  0- 3   magic 'MEMO' (ASCII M,E,M,O)
#   bytes  4- 5   width  (u16 LE)
#   bytes  6- 7   height (u16 LE)
#   byte   8      color format (LV_COLOR_FORMAT_ARGB8888 = 0x10)
#   bytes  9-10   stride (u16 LE) = width * 4
#   bytes 11-15   reserved (zero)
#   bytes 16-..   raw premultiplied BGRA pixel data (LVGL ARGB8888 memory order)

set -euo pipefail

if [[ $# -lt 2 ]]; then
  echo "usage: $0 <in_dir> <out_dir> [size]" >&2
  exit 1
fi

IN_DIR="$1"
OUT_DIR="$2"
SIZE="${3:-16}"

if [[ ! -d "$IN_DIR" ]]; then
  echo "error: input dir '$IN_DIR' not found" >&2
  exit 1
fi
mkdir -p "$OUT_DIR"

if ! command -v magick >/dev/null 2>&1; then
  echo "error: ImageMagick 'magick' not on PATH" >&2
  exit 1
fi

STRIDE=$((SIZE * 4))
EXPECTED_BYTES=$((16 + STRIDE * SIZE))

# Codepoints that should never ship — they combine, don't render standalone.
is_skipped_hex() {
  local h="$1"
  case "$h" in
    fe0f|fe0e|200d) return 0 ;;                    # VS-16, VS-15, ZWJ
    1f3fb|1f3fc|1f3fd|1f3fe|1f3ff) return 0 ;;     # skin tones
    1f1e[6-9a-f]|1f1f[0-9a-f]) return 0 ;;         # regional indicators
  esac
  return 1
}

# Pack one unsigned int as N little-endian bytes to stdout via printf %b.
emit_le() {
  local val="$1"
  local n="$2"
  local i byte out=""
  for ((i = 0; i < n; i++)); do
    byte=$(( (val >> (i * 8)) & 0xff ))
    out+=$(printf '\\x%02x' "$byte")
  done
  printf '%b' "$out"
}

write_header() {
  printf 'MEMO'            # magic
  emit_le "$SIZE"   2      # width
  emit_le "$SIZE"   2      # height
  printf '%b' '\x10'       # cf = ARGB8888
  emit_le "$STRIDE" 2      # stride
  printf '%b' '\x00\x00\x00\x00\x00'  # reserved
}

COUNT=0
SKIPPED=0

shopt -s nullglob
for src in "$IN_DIR"/emoji_u*.png; do
  base="$(basename "$src" .png)"
  stem="${base#emoji_u}"

  # Skip multi-codepoint ZWJ / flag sequences (filenames contain '_').
  if [[ "$stem" == *_* ]]; then
    SKIPPED=$((SKIPPED + 1))
    continue
  fi

  # Skip non-rendering combining codepoints.
  if is_skipped_hex "$stem"; then
    SKIPPED=$((SKIPPED + 1))
    continue
  fi

  out="$OUT_DIR/${stem}.bin"
  tmp="$(mktemp)"

  # ImageMagick: resize, premultiply alpha, and emit headerless raw BGRA bytes.
  # LVGL's LV_COLOR_FORMAT_ARGB8888 is byte-order B,G,R,A in memory (it's a
  # little-endian 0xAARRGGBB uint32), so we emit BGRA not RGBA. Writing to
  # stdout with "...:-" avoids the colon-in-path issue Windows ImageMagick
  # has with drive letters.
  magick "$src" \
    -resize "${SIZE}x${SIZE}" \
    -background none \
    -alpha on \
    -channel RGB -evaluate multiply 1 +channel \
    -depth 8 \
    "BGRA:-" > "$tmp"

  pixel_bytes=$(wc -c < "$tmp")
  want=$((STRIDE * SIZE))
  if [[ "$pixel_bytes" -ne "$want" ]]; then
    echo "warning: $src produced $pixel_bytes bytes, expected $want; skipping" >&2
    rm -f "$tmp"
    SKIPPED=$((SKIPPED + 1))
    continue
  fi

  { write_header; cat "$tmp"; } > "$out"
  rm -f "$tmp"

  final_bytes=$(wc -c < "$out")
  if [[ "$final_bytes" -ne "$EXPECTED_BYTES" ]]; then
    echo "warning: $out is $final_bytes bytes, expected $EXPECTED_BYTES" >&2
  fi

  COUNT=$((COUNT + 1))
done

echo "built $COUNT .bin files in $OUT_DIR (skipped $SKIPPED, expected ${EXPECTED_BYTES} bytes each)"

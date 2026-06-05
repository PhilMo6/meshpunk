#!/usr/bin/env bash
# build_emoji.sh — convert Noto emoji PNGs into a single meshpunk emoji blob.
#
# Usage: bash tools/build_emoji.sh <in_dir> <out_file> [size]
#   in_dir:   folder of Noto PNGs (filenames like "emoji_u1f600.png")
#   out_file: destination blob file (e.g., "data/emojis.bin")
#   size:     optional pixel size, default 16
#
# Requires ImageMagick ('magick' command).
#
# Output blob format (matches the runtime loader in src/emoji_font.cpp):
#   Header (16 bytes):
#     [0..3]   magic 'EMJB'
#     [4..5]   version  (u16 LE) = 1
#     [6..7]   pixel_size (u16 LE)
#     [8..11]  entry count (u32 LE)
#     [12..15] reserved (zero)
#   Index (count * 4 bytes):
#     sorted array of Unicode codepoints (u32 LE each)
#   Data (count * pixel_size^2 * 4 bytes):
#     raw premultiplied BGRA pixels, one block per codepoint (same order as index)

set -euo pipefail

if [[ $# -lt 2 ]]; then
  echo "usage: $0 <in_dir> <out_file> [size]" >&2
  exit 1
fi

IN_DIR="$1"
OUT_FILE="$2"
SIZE="${3:-16}"

if [[ ! -d "$IN_DIR" ]]; then
  echo "error: input dir '$IN_DIR' not found" >&2
  exit 1
fi
mkdir -p "$(dirname "$OUT_FILE")"

if ! command -v magick >/dev/null 2>&1; then
  echo "error: ImageMagick 'magick' not on PATH" >&2
  exit 1
fi

STRIDE=$((SIZE * 4))
PIXEL_BYTES=$((STRIDE * SIZE))

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

# Pack one unsigned int as N little-endian bytes to stdout.
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

TMPDIR_WORK="$(mktemp -d)"
trap 'rm -rf "$TMPDIR_WORK"' EXIT

# Manifest: one line per emoji, "decimal_codepoint hex_codepoint"
MANIFEST="$TMPDIR_WORK/manifest.txt"
: > "$MANIFEST"

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

  tmp="$TMPDIR_WORK/${stem}.raw"

  # ImageMagick: resize, premultiply alpha, and emit headerless raw BGRA bytes.
  # LVGL's LV_COLOR_FORMAT_ARGB8888 is byte-order B,G,R,A in memory (it's a
  # little-endian 0xAARRGGBB uint32), so we emit BGRA not RGBA.
  magick "$src" \
    -resize "${SIZE}x${SIZE}" \
    -background none \
    -alpha on \
    -channel RGB -evaluate multiply 1 +channel \
    -depth 8 \
    "BGRA:-" > "$tmp"

  pixel_bytes=$(wc -c < "$tmp")
  if [[ "$pixel_bytes" -ne "$PIXEL_BYTES" ]]; then
    echo "warning: $src produced $pixel_bytes bytes, expected $PIXEL_BYTES; skipping" >&2
    rm -f "$tmp"
    SKIPPED=$((SKIPPED + 1))
    continue
  fi

  dec=$((16#$stem))
  echo "$dec $stem" >> "$MANIFEST"
  COUNT=$((COUNT + 1))
done

echo "converted $COUNT emojis (skipped $SKIPPED)"

# Sort by codepoint (numeric)
SORTED="$TMPDIR_WORK/sorted.txt"
sort -n "$MANIFEST" > "$SORTED"

# Write the blob
{
  # Header (16 bytes)
  printf 'EMJB'                           # [0..3]  magic
  emit_le 1       2                       # [4..5]  version
  emit_le "$SIZE" 2                       # [6..7]  pixel_size
  emit_le "$COUNT" 4                      # [8..11] count
  printf '%b' '\x00\x00\x00\x00'         # [12..15] reserved

  # Index: sorted codepoints (u32 LE each)
  while IFS=' ' read -r dec hex; do
    emit_le "$dec" 4
  done < "$SORTED"

  # Data: pixel blocks in same order as index
  while IFS=' ' read -r dec hex; do
    cat "$TMPDIR_WORK/${hex}.raw"
  done < "$SORTED"
} > "$OUT_FILE"

total_bytes=$(wc -c < "$OUT_FILE")
echo "wrote $OUT_FILE ($total_bytes bytes, $COUNT emojis at ${SIZE}x${SIZE})"

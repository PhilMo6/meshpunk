#!/usr/bin/env bash
# build_emoji.sh — convert Noto emoji PNGs into a single meshpunk emoji blob.
#
# Usage: bash tools/build_emoji.sh <in_dir> <out_file> [size] [flags]
#   in_dir:   folder of Noto PNGs (filenames like "emoji_u1f600.png" for
#             singles, "emoji_u1f468_200d_1f469_200d_1f467.png" for sequences)
#   out_file: destination blob file (e.g., "data/emojis.bin")
#   size:     optional pixel size, default 16
#   flags:
#     --singles-only    skip all multi-codepoint sequences (v1-equivalent set)
#     --no-skin-tones   skip sequences containing a skin-tone modifier
#     --no-flags        skip flag sequences (regional-indicator pairs and
#                       1F3F4 tag-char subdivision flags)
#
# Requires ImageMagick ('magick' command).
#
# Output blob format v2 (matches the runtime loader in src/emoji_font.cpp):
#   Header (16 bytes):
#     [0..3]   magic 'EMJB'
#     [4..5]   version  (u16 LE) = 2
#     [6..7]   pixel_size (u16 LE)
#     [8..11]  glyph count (u32 LE)
#     [12..15] sequence count (u32 LE; 0 in v1 files — the field was reserved)
#   Index (count * 4 bytes):
#     sorted array of Unicode codepoints (u32 LE each). Multi-codepoint
#     sequences appear here under their assigned PUA codepoint (0xE000+i).
#   Data (count * pixel_size^2 * 4 bytes):
#     raw BGRA pixels, one block per codepoint (same order as index)
#   Sequence table (seq_count * 48 bytes), sorted by (first real codepoint
#   ascending, length descending) so the runtime can binary-search by lead
#   codepoint and greedy-match longest-first:
#     u32 pua        assigned PUA codepoint (also present in the glyph index)
#     u32 len        number of real codepoints in the sequence (2..10)
#     u32 cps[10]    the real codepoints, zero-padded to 10 entries
#
# PUA assignment: sequences are sorted by their codepoint arrays and numbered
# 0xE000+i in that order. The mapping ships inside the blob, so the firmware
# never hardcodes it — but note that rebuilding with different filter flags
# reassigns PUA values, which invalidates any PUA codepoints persisted on the
# device (e.g. a sequence emoji bound to a key in /emoji_keymap).

set -euo pipefail

MAX_SEQ_CPS=10
PUA_BASE=57344   # 0xE000
VERSION=2
MOGRIFY_CHUNK=400

if [[ $# -lt 2 ]]; then
  echo "usage: $0 <in_dir> <out_file> [size] [--singles-only] [--no-skin-tones] [--no-flags]" >&2
  exit 1
fi

IN_DIR="$1"
OUT_FILE="$2"
shift 2

SIZE=16
OPT_SINGLES_ONLY=0
OPT_NO_SKIN=0
OPT_NO_FLAGS=0
for arg in "$@"; do
  case "$arg" in
    --singles-only)  OPT_SINGLES_ONLY=1 ;;
    --no-skin-tones) OPT_NO_SKIN=1 ;;
    --no-flags)      OPT_NO_FLAGS=1 ;;
    [0-9]*)          SIZE="$arg" ;;
    *) echo "error: unknown argument '$arg'" >&2; exit 1 ;;
  esac
done

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

# ---- Codepoint classification (decimal) -------------------------------------

is_skin_tone()  { (( $1 >= 127995 && $1 <= 127999 )); }   # 1F3FB..1F3FF
is_regional()   { (( $1 >= 127462 && $1 <= 127487 )); }   # 1F1E6..1F1FF
is_tag_char()   { (( $1 >= 917536 && $1 <= 917631 )); }   # E0020..E007F

# Standalone codepoints that should never ship — they combine, don't render.
is_skipped_single() {
  local d="$1"
  (( d == 65039 || d == 65038 || d == 8205 )) && return 0   # FE0F, FE0E, ZWJ
  is_skin_tone "$d" && return 0
  is_regional "$d" && return 0
  return 1
}

# Pack one unsigned int as N little-endian bytes to stdout.
# printf -v only — a $(printf ...) command substitution forks a subshell,
# and this runs ~30k times per build (10+ ms per fork under msys).
emit_le() {
  local val="$1"
  local n="$2"
  local i byte hex out=""
  for ((i = 0; i < n; i++)); do
    byte=$(( (val >> (i * 8)) & 0xff ))
    printf -v hex '\\x%02x' "$byte"
    out+="$hex"
  done
  printf '%b' "$out"
}

TMPDIR_WORK="$(mktemp -d)"
trap 'rm -rf "$TMPDIR_WORK"' EXIT

# Native ImageMagick on Windows can't read msys "/c/..." paths from an @list
# file (shell path conversion only applies to command-line args), so feed it
# mixed-mode ("C:/...") paths. No-op on real POSIX systems.
to_native() {
  if command -v cygpath >/dev/null 2>&1; then cygpath -m "$1"; else printf '%s\n' "$1"; fi
}
IN_DIR_W="$(to_native "$IN_DIR")"
WORK_W="$(to_native "$TMPDIR_WORK")"

CONVLIST="$TMPDIR_WORK/convert.txt"     # png paths to convert
SINGLES="$TMPDIR_WORK/singles.txt"      # "dec stem"
SEQS="$TMPDIR_WORK/seqs.txt"            # "sortkey stem len dec1 dec2 ..."
: > "$CONVLIST"; : > "$SINGLES"; : > "$SEQS"

N_SINGLE=0
N_SEQ=0
N_FLAGS=0
N_SKIN=0
SKIPPED=0

# ---- Pass 1: enumerate + classify -------------------------------------------

shopt -s nullglob
for src in "$IN_DIR"/emoji_u*.png; do
  base="${src##*/}"          # parameter expansion, not basename — 3.7k
  stem="${base%.png}"        # subprocess spawns cost ~15 min under msys
  stem="${stem#emoji_u}"

  if [[ "$stem" != *_* ]]; then
    # Single codepoint
    dec=$((16#$stem))
    if is_skipped_single "$dec"; then
      SKIPPED=$((SKIPPED + 1))
      continue
    fi
    echo "$dec $stem" >> "$SINGLES"
    echo "$IN_DIR_W/$base" >> "$CONVLIST"
    N_SINGLE=$((N_SINGLE + 1))
    continue
  fi

  # Multi-codepoint sequence
  if (( OPT_SINGLES_ONLY )); then
    SKIPPED=$((SKIPPED + 1))
    continue
  fi

  IFS='_' read -ra parts <<< "$stem"
  len=${#parts[@]}
  if (( len > MAX_SEQ_CPS )); then
    echo "warning: $stem has $len codepoints (max $MAX_SEQ_CPS); skipping" >&2
    SKIPPED=$((SKIPPED + 1))
    continue
  fi

  decs=()
  sortkey=""
  has_skin=0
  for hx in "${parts[@]}"; do
    d=$((16#$hx))
    decs+=("$d")
    printf -v padded '%08x' "$d"   # printf -v: no subshell fork per cp
    sortkey+="$padded"
    if is_skin_tone "$d"; then has_skin=1; fi
  done

  is_flag=0
  if (( len == 2 )) && is_regional "${decs[0]}" && is_regional "${decs[1]}"; then
    is_flag=1
  elif (( decs[0] == 127988 )) && is_tag_char "${decs[1]}"; then   # 1F3F4 + tags
    is_flag=1
  fi

  if (( OPT_NO_SKIN && has_skin )); then SKIPPED=$((SKIPPED + 1)); continue; fi
  if (( OPT_NO_FLAGS && is_flag )); then SKIPPED=$((SKIPPED + 1)); continue; fi

  echo "$sortkey $stem $len ${decs[*]}" >> "$SEQS"
  echo "$IN_DIR_W/$base" >> "$CONVLIST"
  N_SEQ=$((N_SEQ + 1))
  if (( is_flag ));  then N_FLAGS=$((N_FLAGS + 1)); fi
  if (( has_skin )); then N_SKIN=$((N_SKIN + 1)); fi
done

echo "classified: $N_SINGLE singles, $N_SEQ sequences ($N_FLAGS flags, $N_SKIN skin-tone), $SKIPPED skipped"

# ---- Pass 2: batched conversion ----------------------------------------------
# One mogrify per chunk instead of one magick per file — orders of magnitude
# faster on Windows. Output: $TMPDIR_WORK/<stem>.bgra (raw BGRA bytes; LVGL's
# LV_COLOR_FORMAT_ARGB8888 is byte-order B,G,R,A in memory).

split -l "$MOGRIFY_CHUNK" "$CONVLIST" "$TMPDIR_WORK/chunk_"
for chunk in "$TMPDIR_WORK"/chunk_*; do
  # @list keeps us clear of the Windows 32KB command-line limit.
  magick mogrify -format bgra -path "$WORK_W" \
    -resize "${SIZE}x${SIZE}" -background none -alpha on -depth 8 \
    @"$(to_native "$chunk")"
done
echo "converted $((N_SINGLE + N_SEQ)) PNGs to ${SIZE}x${SIZE} BGRA"

# ---- Pass 3: verify conversion outputs exist, drop missing entries ----------
# Existence check only (builtin, no spawns) — per-file size would cost one wc
# per PNG. A wrong-size .bgra would misalign every later glyph, so the final
# whole-blob size check at the bottom fails hard if any file came out odd.

verify_kept() {  # $1 = manifest (stem is field 2 in both formats)
  local manifest="$1"
  local out="$manifest.kept"
  : > "$out"
  local line stem f1 rest
  while IFS= read -r line; do
    [[ -z "$line" ]] && continue
    read -r f1 stem rest <<< "$line"
    if [[ ! -f "$TMPDIR_WORK/emoji_u${stem}.bgra" ]]; then
      echo "warning: no conversion output for $stem; skipping" >&2
      continue
    fi
    echo "$line" >> "$out"
  done < "$manifest"
  mv "$out" "$manifest"
}

verify_kept "$SINGLES"
verify_kept "$SEQS"

# ---- Pass 4: assign PUA + build glyph manifest -------------------------------

sort "$SEQS" -o "$SEQS"    # by sortkey → deterministic PUA assignment

GLYPHS="$TMPDIR_WORK/glyphs.txt"        # "dec stem"
SEQTAB="$TMPDIR_WORK/seqtab.txt"        # "firstdec len pua dec1 dec2 ..."
cp "$SINGLES" "$GLYPHS"
: > "$SEQTAB"

pua=$PUA_BASE
while IFS=' ' read -r sortkey stem len rest; do
  [[ -z "$sortkey" ]] && continue
  echo "$pua $stem" >> "$GLYPHS"
  first="${rest%% *}"
  echo "$first $len $pua $rest" >> "$SEQTAB"
  pua=$((pua + 1))
done < "$SEQS"

SEQ_COUNT=$((pua - PUA_BASE))
if (( PUA_BASE + SEQ_COUNT > 63743 )); then   # 0xF8FF end of BMP PUA
  echo "error: $SEQ_COUNT sequences overflow the BMP PUA block" >&2
  exit 1
fi

sort -n "$GLYPHS" -o "$GLYPHS"
COUNT=$(wc -l < "$GLYPHS" | tr -d ' ')

# Sequence table order: first codepoint ascending, length descending.
sort -k1,1n -k2,2rn "$SEQTAB" -o "$SEQTAB"

# ---- Pass 5: write the blob ---------------------------------------------------

DATALIST="$TMPDIR_WORK/datalist.txt"
awk -v dir="$TMPDIR_WORK" '{ print dir "/emoji_u" $2 ".bgra" }' "$GLYPHS" > "$DATALIST"

{
  # Header (16 bytes)
  printf 'EMJB'                           # [0..3]  magic
  emit_le "$VERSION" 2                    # [4..5]  version
  emit_le "$SIZE" 2                       # [6..7]  pixel_size
  emit_le "$COUNT" 4                      # [8..11] glyph count
  emit_le "$SEQ_COUNT" 4                  # [12..15] sequence count

  # Index: sorted codepoints (u32 LE each)
  while IFS=' ' read -r dec stem; do
    emit_le "$dec" 4
  done < "$GLYPHS"

  # Data: pixel blocks in index order
  xargs cat < "$DATALIST"

  # Sequence table: u32 pua, u32 len, u32 cps[MAX_SEQ_CPS]
  while IFS=' ' read -r first len seqpua rest; do
    emit_le "$seqpua" 4
    emit_le "$len" 4
    i=0
    for d in $rest; do
      emit_le "$d" 4
      i=$((i + 1))
    done
    while (( i < MAX_SEQ_CPS )); do
      emit_le 0 4
      i=$((i + 1))
    done
  done < "$SEQTAB"
} > "$OUT_FILE"

total_bytes=$(wc -c < "$OUT_FILE")
expected_bytes=$((16 + COUNT * 4 + COUNT * PIXEL_BYTES + SEQ_COUNT * 48))
if [[ "$total_bytes" -ne "$expected_bytes" ]]; then
  echo "error: blob is $total_bytes bytes, expected $expected_bytes — a conversion produced wrong-size pixels" >&2
  exit 1
fi
echo "wrote $OUT_FILE ($total_bytes bytes: $COUNT glyphs at ${SIZE}x${SIZE}, $SEQ_COUNT sequences)"

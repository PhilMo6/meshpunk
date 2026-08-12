#!/bin/sh
# Regenerates the soundtrack: MIDI from make_music.py, rendered to PCM with
# the module's own OPL synthesiser so the device ships audio it never has to
# compute, then compressed. Run from the jet3d root:
#
#     sh tools/build_music.sh <outdir>
#
# Ships as IMA ADPCM at the FULL 22050 rate. Four bits a sample is half the
# size of the 11025 16-bit this replaced, so the bandwidth came back for free
# -- and at the mixer's own rate playback is 1:1, so nothing resamples, which
# is what the ADPCM decoder needs anyway.
#
# Only the .adp are left behind. The .mid and .raw are intermediates and are
# deleted: the SOURCE of truth is tools/make_music.py, and shipping a .mid
# would leave the game a fallback path that SYNTHESISES the music live, which
# is the CPU cost this whole pipeline exists to avoid. If an .adp is ever
# missing on a device, silence is a much better failure than an unexplained
# frame-rate collapse.
set -e
cd "$(dirname "$0")/.."
if [ -z "$1" ]; then
  echo "usage: sh tools/build_music.sh <outdir>" >&2
  echo "  e.g. the game's music dir in a firmware tree, or any scratch dir" >&2
  exit 2
fi
OUT="$1"
PY="$HOME/.platformio/penv/Scripts/python.exe"

echo "midi:"
"$PY" tools/make_music.py "$OUT"

echo "renderer:"
mkdir -p tools/obj
gcc -O2 -std=gnu99 -Iengine/music/compat -Iengine/music -Iengine \
    -o tools/obj/render_music.exe tools/render_music.c \
    engine/music/dbopl.c engine/music/i_oplmusic.c engine/music/jet_music.c \
    engine/music/memio.c engine/music/midifile.c engine/music/mus2mid.c \
    engine/music/opl_queue.c engine/music/opl_tdeck.c

echo "adpcm:"
"$PY" tools/make_music.py "$OUT" --seconds | while read -r name secs; do
  ./tools/obj/render_music.exe "$OUT/$name.mid" "$OUT/$name.raw" \
      "$secs" 1 > /dev/null
  "$PY" tools/adpcm.py encode "$OUT/$name.raw" "$OUT/$name.adp" 22050
  rm -f "$OUT/$name.raw" "$OUT/$name.mid"
done

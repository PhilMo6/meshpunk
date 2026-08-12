#!/bin/sh
# Renders the audition candidates to WAV through the game's own OPL bank, so
# they are judged on the instruments the device will actually use -- a .mid
# played on a PC uses the OS wavetable and sounds nothing like this.
# Full 22050 rate here: these are for listening, not for shipping.
set -e
cd "$(dirname "$0")/.."
OUT="${1:-music-candidates}"
PY="$HOME/.platformio/penv/Scripts/python.exe"
mkdir -p "$OUT" tools/obj

echo "midi:"
"$PY" tools/make_candidates.py "$OUT"

gcc -O2 -std=gnu99 -Iengine/music/compat -Iengine/music -Iengine \
    -o tools/obj/render_music.exe tools/render_music.c \
    engine/music/dbopl.c engine/music/i_oplmusic.c engine/music/jet_music.c \
    engine/music/memio.c engine/music/midifile.c engine/music/mus2mid.c \
    engine/music/opl_queue.c engine/music/opl_tdeck.c

echo "wav:"
"$PY" tools/make_candidates.py "$OUT" --seconds | while read -r name secs; do
  ./tools/obj/render_music.exe "$OUT/$name.mid" "$OUT/$name.raw" "$secs" 1 \
      > /dev/null
  "$PY" tools/raw2wav.py "$OUT/$name.raw" "$OUT/$name.wav" 22050
  rm -f "$OUT/$name.raw"
  ls -la "$OUT/$name.wav" | awk '{printf "  %-34s %6.1f KB\n", $9, $5/1024}'
done

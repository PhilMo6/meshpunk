SKYLOOPERS AUDIO PIPELINE
=========================

The shipped music is pre-rendered: the device plays compressed samples and
never runs the FM synthesiser, because emulating the chip costs a large slice
of a core every frame. These scripts are what turn notes into what ships, and
they are here so the audio can be regenerated from published source alone.

    sh tools/build_music.sh <outdir>

writes menu.adp, race1.adp, race2.adp, race3.adp and finish.adp into <outdir>.
Point it at a firmware tree's games/skyloopers/music directory. Requires
python3 and gcc (MinGW on Windows); the OPL sources it compiles are the same
ones the module ships, so the output is what the device would have synthesised.

WHAT EACH FILE IS
  midi.py             a small standard-MIDI writer, no dependencies
  make_candidates.py  THE MUSIC. Every track ever auditioned lives here, as
                      code. This is the source of truth for the notes.
  make_music.py       a SELECTOR: names which candidates ship, under which
                      filenames. Deliberately not a second copy of the notes.
  make_genmidi.py     the OPL instrument bank (GENMIDI format), authored here
                      rather than taken from a WAD -- Doom's is proprietary.
                      Writes engine/music/genmidi_bank.h.
  render_music.c      renders a MIDI to raw PCM through the module's own OPL
                      code, normalising the result to 0.92 of full scale.
  adpcm.py            IMA ADPCM codec and the "JADP" container writer.
  build_music.sh      the pipeline: notes -> MIDI -> PCM -> ADPCM.
  build_candidates.sh renders every candidate to a playable .wav for
                      auditioning, through the real instrument bank.
  raw2wav.py          wraps headerless PCM in a WAV header.

ONLY THE .adp ARE INSTALLED. The .mid and .raw are intermediates and the
pipeline deletes them: a shipped .mid would give the game a fallback path that
synthesises live, which is the cost this whole arrangement exists to avoid.

If you change make_genmidi.py you must rebuild the module, because the bank is
compiled into it. Changing make_candidates.py or make_music.py only needs
build_music.sh re-run.

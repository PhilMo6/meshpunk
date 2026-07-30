# Vendored tiny386 core

Source: https://github.com/hchunhui/tiny386
Commit: `2b555cae0c965d62666e7ceddb23900eb541f554` (master, fetched 2026-07-24)

## Licenses

- **tiny386 core (CPU + machine): BSD-3-Clause**, (c) 2024-2025 Chunhui He. See LICENSE.
- **Peripherals ported from QEMU / TinyEMU: MIT** (i8259 PIC, i8254 PIT, i8042 keyboard
  controller, ne2000, parts of vga/ide).
- **fmopl.c (Adlib OPL2 synth): LGPL.** Built in (`-DUSE_FMOPL`) because most DOS-era game
  music is OPL. Building with `USE_FMOPL` undefined drops it and leaves the module pure
  BSD/MIT, at the cost of FM music.
- **SeaBIOS + SeaVGABIOS (`bios.bin`, `vgabios.bin`): LGPL-3.** Not in this directory —
  they ship as data files in the store package (meshpunk-apps/apps/Dos/). Taken from the
  tiny386 "continuous" release build, which applies the project's own SeaBIOS patch
  (see below); they are NOT interchangeable with stock SeaBIOS images.

## Files

Vendored: the portable core only — `i386.c` + `i386ins.def` + `simd.inc` (CPU), `pc.c`
(machine), `vga.c`, `ide.c`, `i8042.c`, `i8254.c`, `i8257.c`, `i8259.c`, `sb16.c`,
`pcspk.c`, `adlib.c`, `fmopl.c`, `fpu.c`, `misc.c`, `pci.c`, `ini.c`, `ne2000.c` + headers.

NOT vendored: `amd64.*` / `kvm.*` (long mode, KVM), `main.c` / `win32.c` (desktop
frontends), and the `esp/ sdl/ rawdraw/ wasm/ osd/ scripts/ tools/ glibc-fix/ conf/
seabios/ linuxstart/` directories. Our frontend is `../main_tdeck.c` + `../dos_*.c`.

`ne2000.c` is vendored even though the T-Deck build has no networking: `pc.c` calls
`ne2000_*` unconditionally (ioport handlers, `ne2000_step`, `isa_ne2000_init`). Without
`USE_SLIRP` / `USE_TUNTAP` / `BUILD_ESP32` it compiles to a null backend.

## Build-level adaptation (not source edits)

`BUILD_ESP32` is deliberately NOT defined. That guard selects ESP-IDF specifics we must
avoid in an ELF module:
- `i386.c` would include `esp_attr.h` and mark 31 functions `IRAM_ATTR`; our module
  executes from PSRAM, so the attributes must stay no-ops.
- `ne2000.c` would include `esp_mac.h`.
- `pcmalloc()` (a bump allocator the ESP frontend supplies) reverts to plain `malloc`
  in `i386.c`, `fmopl.c` and `ne2000.c` — so the frontend does not implement it.

Also `-DBPP=16` (native RGB565 framebuffer; upstream's own ESP board headers do the same)
and `-Dfseeko=fseek -Dftello=ftell` (the firmware exports `fseek`/`ftell` but not the
64-bit `*o` variants; disk images are well under 2GB).

Platform HAL implemented by `../main_tdeck.c`: `get_uticks()`, `bigmalloc()`, `load_rom()`.

## Local modifications

List every in-file edit here as it is made. All are tagged `MESHPUNK` in the source.

- **DIAGNOSTIC, strippable** — `sb16.c`: one `DOSAUD_MIX()` call at the end of
  `sb16_audio_callback` recording the input ring's occupancy and whether the
  resampler consumed all of it. Testing whether `approx_frac()`'s 8-denominator
  cap makes the resampler outrun the DMA. `../dos_audit.h` is force-included via
  `-include`, so the file needs no `#include` edit and the macro compiles to
  nothing without `-DDOS_AUDIT=1`.
- `sb16.c`: **guest audio rate divider** `meshpunk_sb_rate_div` (launcher "Audio rate":
  FULL / 1/2 / 1/4 / OFF, passed as `-audio N`). `sb16_audio_callback` hands the
  resamplers `s->freq / N`, so they consume the guest's buffer N times slower; the DMA
  pointer therefore advances N times slower and `left_till_irq` fires N times less often.
  The point is CPU, not audio: the guest's SB interrupt handler is the largest single
  consumer of emulated cycles (~27% of wall for MS Pac-Man), and it only runs because we
  drain its buffer -- with `-audio 0` we never pump, the IRQ never fires, and the handler
  never executes at all. hw-confirmed: 1/2 and 1/4 both speed up the game, and DOS itself
  boots faster. Costs are inherent, not bugs: pitch and speed drop by N, and anything the
  guest synchronises on audio stretches by N (MS Pac-Man gates level start on its
  start-sound completing).
- `sb16.c`: **DMA concealment in `SB_read_DMA`/`write_audio`** (auto-init, 8-bit
  formats). Root cause, proven from WAV captures at the host_audio_push boundary: our
  batched pump advances the DMA pointer with the guest frozen, so it can lap into the
  half-region the not-yet-run refill ISR has not rewritten — capture 1 showed 207 clicks
  phase-locked to the 713.5-sample block period, 120 of them replaying audio from exactly
  one DMA region (2 blocks) earlier with correlation > 0.9. A v1 hard ack-gate (never
  advance past an un-acked boundary) killed the replays (120 → 15 in capture 2) but
  hw-stuttered: at this emulation speed the guest's ack lands 4-29ms after the raise,
  longer than the one-block window, so the mixer starved (116 flat plateaus + 129 gaps).
  v2 keeps the same bookkeeping but conceals instead of holding: valid data is delivered
  exactly up to the boundary (the seam lands on a call edge), the pump then reads on —
  stale bytes replay the previous region, the least-wrong filler for quasi-periodic game
  audio — and both seams get a crossfade in `write_audio` (`fade`/`fade_from`/
  `last_byte` in SB16State; entry armed at the un-acked boundary crossing in the raise
  block, exit armed at the rejoin). v3 (capture 3): exiting at the ACK alone still
  spliced — the guest acks early in its ISR and copies the refill in afterwards — so
  exit requires ack AND the region wrap (`dma_pos == 0`), where stored-B-end →
  stored-A-start is the guest's true stream succession; concealing reads are clamped at
  the wrap so the exit lands on a call edge. THIS (v3) IS THE CURRENT STATE.
  ~~v4 matched splice~~ BUILT AND REVERTED 2026-07-27/28: replay from a SAD-matched
  point in ring history instead of fades-toward-a-constant. It did kill the full-rate
  static (capture 8: perfect lattice, joins at rho 0.97-1.00), but a 128-byte grain
  floor sounded like "a 56k modem" (an episode outlasting its lookback loops an 8-28ms
  grain — granular chirping), and at a 512-byte floor the full-rate result was a
  temporal collage (fresh/0.2-0.35s-old interleave at region cadence) plus
  machine-gunned transient SFX (a miss near a chomp replays the chomp — capture 8
  showed exact output loops of chomp neighborhoods). Root limitation is the guest's
  ISR throughput (misses = never-rendered blocks), which no filler fixes. Phil called
  the trade: reverted to v2/v3 fades. Retained from that campaign: the stale-head
  re-read (separate entry above), which is verified and independent.
- `sb16.c`: **trailing-window re-read (stale-head repair) in `SB_read_DMA`**
  (`MESHPUNK_REREAD_MAX`, auto-init 8-bit only). Byte-exact forensics on a 0.25-rate
  capture (every 11th output sample is the ring byte verbatim at that rate's 11/1
  resample ratio) recovered the full DMA byte stream and proved: mid-block content is a
  perfect copy of the game's source file at a constant lag (rho 1.00), but the first
  4-104 bytes of EVERY block match the source one region (512 bytes) BACK — the guest's
  refill completes after the pump has already consumed the block head, at every rate,
  because the pump reads past a boundary in the same call that raises its IRQ. Two
  splices per block was the residual static (the concealment machinery never sees it:
  its entry requires the IRQ still un-acked at the crossing, and the guest acks early —
  ack ≠ refill-complete). Fix: each pump call re-reads the already-consumed span of the
  current block and overwrites the matching ring bytes in place — the ring buffers
  ~1024 bytes ahead of playback, so the head is silently corrected long before it
  plays. Window = current block always, plus the PREVIOUS block once the guest is a
  proven late-writer (`late_writer` latch: a re-read that differs from the ring is a
  refill landing after our first read — exactly the late-writer behaviour; a half such
  a guest vacates stays untouched until the next wrap, so its positions stay valid for
  a second block period, doubling the repair deadline to 2 block periods — 31.7ms at
  full rate vs the 13-29ms tune refill latency). Until latched the window stays
  current-block-only: an early-writer guest puts next-pass data in the vacated half and
  patching that would splice future audio into the past. `reread_floor` fences ring
  bytes a conceal episode replayed or a seam fade blended (the floor rides the write
  head while a fade drains); wrap handled with a two-chunk read; clamped at the play
  cursor and MESHPUNK_REREAD_MAX; skipped entirely while concealing. Hardware-verified
  at 0.25/0.5 rate (static eliminated); at full rate blocks the guest never refills at
  all (the tune's 2x stretch = every other refill missing) remain concealment's job.
- `sb16.c`: ~~audio consumption governor~~ BUILT AND REVERTED same day (2026-07-27,
  hw-rejected: "sounded like nonsense... destroyed sound accuracy"). An AIMD throttle
  on the resample rate walks the pitch in 5% steps forever (descend/cool/probe), and
  its descent + probe phases are over-capacity by construction, so it guaranteed
  periodic static ON TOP of audible pitch instability. Lesson: adaptive rate trades
  static for pitch warble — the ear forgives a constant rate (the 0.5 setting sounds
  right) and forgives nothing on a wandering one. Do not rebuild.
- `sb16.c`: **resampler phase carried across mixer calls** (`rs_phase[4]` in SB16State,
  passed into `resample_u8m`/`u8s`). Upstream restarts the uc/dc counters from scratch on
  every call and advances the ring position by WHOLE bytes, discarding the fractional
  position inside the current byte — so every 128-frame mixer slice re-emitted that
  fraction. Proven from a 0.25-rate capture, where the ratio is 89/8 and the fraction is
  ~half a byte: a ~7-sample span played twice, bit-identical, at every steep passage; at
  full rate (14/5) the repeat is ~2 samples — the rate-invariant "static" present since
  the first build, audible at signal peaks, unaffected by any seam/concealment work.
  Upstream's desktop build slices at 2048 frames (resets at ~21Hz, inaudible); our
  MIXER_BUF_LEN=512 made it 345Hz. The phase is valid only for the (os, is) pair it was
  made with; stream restart or a rate change resets it naturally. The 16-bit resamplers
  are unchanged (DOS-era games are 8-bit).
- `sb16.c`: **linear interpolation in `resample_u8m`/`resample_u8s`**, replacing upstream's
  sample-and-hold. `uc` counts down from `os` across one input sample, so `(os - uc)` is
  the phase within it. Interpolated in the 16-bit output domain -- doing it in 8-bit would
  quantise the result straight back and gain nothing. At 44100/15800 the hold spans 2-3
  output slots, and 2-4x that again under the rate divider, which is why the artifact gets
  audibly worse as the rate drops. Costs one divide per output sample (~0.5% of wall,
  against `mix` at 1.8%). The 16-bit paths are left alone: DOS-era games are 8-bit.
- `sb16.c`: **SB digital modes** (`meshpunk_sb_digital`, module arg `-sbdigi N`,
  launcher "SB digital": ON / NO DMA / NO IRQ / OFF). Fabricates the classic
  real-hardware misconfigurations so a game's own detection disables what the
  emulator cannot afford. ON(1) = normal. NO DMA(3) = `SB_read_DMA` returns without
  progressing -- the wrong-DMA-jumper condition; DSP handshake and IRQ test pass, a
  DMA verification (test transfer + completion timeout) fails deterministically;
  games with a distinct "DMA broken" branch keep music, disable digitised playback.
  NO IRQ(2) = `sb_raise_irq()` gates every interrupt raise (0xF2 test, DMA
  completion, reset pulse, silence command); the wrong-IRQ-jumper condition -- DSP
  answers, the line is dead, `mixer_regs[0x82]` still sets (card-internal truth, as
  on real hardware). Verified from the Creative Hardware Programming Guide + era
  sources: the 0xF2 IRQ test is the semi-documented standard (Creative's own
  installer reports "Error in Interrupt detection"), and direct mode's per-sample
  timer pacing is official (guide p33). OFF(0) = DSP ports float entirely (reads
  0xFF, writes ignored) -- detection itself fails; note some detectors retry that
  for MINUTES at emulated speed. Adlib (0x388) and mixer ports stay alive in all
  modes. Motive (Dyna): per-sample players demand ~20k timer ISRs/s, several times
  the emulator's whole CPU budget -- the guest starves whenever a sample plays.
  Hw-verified: NO IRQ makes Dyna fully silent every launch (its init is
  all-or-nothing on the SB check). Companion: freedos/*-nb.img boot floppies with
  SET BLASTER removed, for games that trust the variable without probing.
- `i8254.c`: **channel-0 IRQ delivery cap** (`meshpunk_pit_cap_hz`, module arg
  `-pitcap N`, launcher "Timer cap" = 1000). Per-sample digitised-audio players
  reprogram the PIT to the sample rate and feed one sample per interrupt — ~20k
  ISRs/s, several times the emulated CPU's budget, so the guest livelocks whenever a
  sample plays (Dyna). With the cap: a ch0 program faster than the cap still counts
  and reads back truthfully (`pit_get_count` untouched), but pulses in
  `i8254_update_irq` are delivered at most cap-Hz apart (wall-clock spacing via
  `cap_last_us`) and a delivered pulse forgives the backlog (`last_irq_count = d`) —
  skipped periods are dropped, never queued, so the player's sample advances slowly
  in the background while the game runs. Applies only when programmed rate > cap
  (count < PIT_FREQ/cap); the 18.2Hz default and normal tens-to-hundreds-Hz game
  reprograms pass through untouched. Opt-in, default off.
- `sb16.c`: **ring priming gate + one-shot handling** (`s->priming` in SB16State).
  While the input ring is first filling, the resamplers stop early on `i < ilen` and the
  tail of each output buffer keeps mixer_callback's memset zeros — a run of partial
  buffers each ending in a step, audible as a crackle right when a sound starts. The
  callback emits nothing until the ring holds ≥512 bytes (~32ms at 8-bit DOS rates);
  armed by `control(s,1)` on every DMA (re)start and re-armed if the ring drains dry.
  Two one-shot (single-cycle) fixes on top, found via Dyna's bomb-SFX guest hang: (1)
  `control(s,1)` resets `audio_p = audio_q` — a new stream never inherits stale ring
  bytes. Short one-shots the gate held back otherwise ACCUMULATE until the ring limit
  makes `write_audio` accept nothing, which stalls the next transfer's DMA forever and
  its completion IRQ never fires — a game waiting on that IRQ hangs or runs wild. (2)
  the gate releases when the transfer has already stopped (`!dma_running`): a completed
  short one-shot can never reach the threshold, so emit what it delivered instead of
  holding it forever (it would otherwise be silently swallowed).
- `vga.c`: **320-wide 16-colour planar modes published half their timing width**
  (mode 0Dh etc). In `vga_graphic_refresh`, the halved dot clock (`sr[0x01]` bit 3)
  means each stored pixel covers two on screen, so the renderer sets `xdiv = 2` —
  but upstream doubled the published width only for `shift_control == 1`, marking
  the asymmetry `XXX`. With `shift_control == 0` the width stayed at the real pixel
  count while `xdiv` still halved the source index, so only the left half of the
  image was read and it came out exactly twice too wide (hw: Wolf3D's menus,
  clipped). Now doubled for both shift modes — matching mode 13h's own handling
  (publish the timing width; `xdiv` plus the native-res collapse halve it back)
  and QEMU, which doubles `disp_width` in both cases.
- `vga.c`: direct-to-panel rendering. When the linear-8bpp fast path applies,
  `vga_graphic_refresh()` calls `dos_video_emit_row()` per source row instead of writing
  the canvas, and the frontend's scale-and-push pass is skipped entirely. This removes one
  of the frame's three PSRAM crossings — the canvas was written 64k pixels and read back
  76.8k every frame. `dos_video.c` owns the decision (`dos_video_direct_begin()` returns 0
  for anything it cannot handle) so all scaling knowledge stays out of the vendored file.
  Deliberately narrow: point-sampled 8bpp only. Box filtering needs two source rows, which
  a source-driven walk does not have, and `vga_text_refresh()` still renders via the canvas
  — which is also why the canvas cannot be freed and the byteswap cannot be folded into the
  palette (both paths must agree on pixel format).

- `vga.c` / `vga.h`: export the active video mode's rectangle within the framebuffer as
  `meshpunk_vga_x/y/w/h`. Upstream centres the guest's mode inside a fixed-size canvas
  and letterboxes the remainder; the T-Deck frontend needs the content rect so it can
  stretch exactly the active area onto the 320x240 panel instead of scaling the padded
  canvas. Set in both `vga_text_refresh()` and `vga_graphic_refresh()`.
- `ide.c` / `ide.h`: move `BlockDeviceCompletionFunc` and `struct BlockDevice` from ide.c
  into ide.h, and add `ide_attach_bd()`. Upstream can only attach a disk by filename;
  the T-Deck build presents an SD folder as drive C:, which needs a caller-supplied
  backend. `BlockDevice` already has a `get_chs()` hook, so the synthesized disk reports
  its own geometry rather than the IDE layer inferring one.
- `vga.c` / `vga.h`: native-resolution rendering for pixel-doubled graphics modes, behind
  the runtime flag `meshpunk_vga_native` (default on, `-native 0` disables). Low-res VGA
  modes are programmed as high-res timing with each pixel emitted twice across
  (`xdiv == 2`) and each scanline twice down (`multi_scan == 1`) — mode 13h is 320x200 of
  real content rendered as 640x400, so 3 of every 4 pixel writes are duplicates that the
  frontend's rescale then discards. Measured at 43% of wall time in-game. When both
  doublings are present (and `comp_ntsc` is off, whose path decodes 4 pixels at a time)
  `vga_graphic_refresh()` halves `w`/`h` and clears `xdiv`/`multi_scan`, so each source
  pixel and line is read exactly once. Everything downstream — `addr1` stepping, the CGA
  `cr[0x17]` wrap logic, palette lookups — is untouched and still keyed off `y1`.
- `vga.c`: specialised inner loop in `vga_graphic_refresh()` for linear 8-bit chunky
  video (mode 13h and VBE 8bpp — effectively every DOS game). The generic loop
  re-evaluates four loop-invariant things FOR EVERY PIXEL: a runtime integer divide
  (`x / xdiv`), the `shift_control` branch chain, a 6-case `switch (bpp)`, and a
  multiply rebuilding a destination index that only advances by one pixel. Measured
  at ~78 host cycles/pixel to do a palette lookup and a store. The fast path hoists
  all of it, splits `xdiv` 1/2 into shifts, and stores 16 bits once instead of two
  bytes. Everything computed per ROW by the generic path — `addr` with the CGA
  `cr[0x17]` wrap, `i0` centring, `y1` — is used unchanged, and the path is gated to
  `BPP == 16` with none of the SCALE_*/SWAPXY variants defined.
- `pc.c` / `pc.h`: decouple the audio DMA from the instruction stream. The two
  `i8257_dma_run()` calls are removed from `pc_step()` and exposed as
  `pc_audio_dma_pump()`, which the module calls immediately before draining the mixer,
  so the SoundBlaster's buffer is filled once per audio batch rather than once per
  `pc_step()`. Safe to move because the SoundBlaster is the ONLY DMA user — both
  `i8257_dma_register_channel()` calls are in sb16.c and the floppy interface does not
  use DMA. It also removes the cross-core race the original code documents and accepts
  ("XXX: There are races"), since producer (`SB_read_DMA`) and consumer
  (`sb16_audio_callback`) are now always the same thread.
- `fmopl.c` / `sb16.c` / `pcspk.c` / `adlib.c` / `pc.c`: tag the audio synthesis hot path
  `MESHPUNK_HOT` so it links into `.iram.text` and the firmware's ELF loader copies it into
  internal SRAM. Tagged: `YM3812UpdateOne`, `sb16_audio_callback`, `pcspk_callback`,
  `adlib_callback`, `mixer_callback` (3541 bytes total).
  `OPL_CALC_SLOT` is instead `MESHPUNK_INLINE` (always_inline, NO section attribute): it is
  called per-slot per-sample, and an explicit placement would stop GCC inlining it anywhere
  and put a real call in the innermost loop — inlined into a tagged caller its code lands in
  the section regardless. `CALC_FCSLOT` is deliberately untagged: it is on the register-write
  path, not per-sample. These files build with `-mtext-section-literals` (mandatory — l32r is
  PC-relative and fixed at link, so the section must be self-contained) and `build.ps1` audits
  after linking that every l32r target lands in-section. See `../dos_hot.h`.
- `pc.c`: replace the `fprintf(stderr, ...)` in the unhandled-port `default:` cases of
  `pc_io_write`/`pc_io_writew`/`pc_io_writel` with `meshpunk_log_unhandled_port()`, which
  logs each port once. The originals fired on EVERY write, so a game polling an
  unimplemented port (MS Pac-Man hits the joystick port 0x201 in a loop) emitted one
  USB-serial write per access — far more expensive than the emulated instruction.
  Upstream had already commented out the equivalent on the read path.
- `misc.c`: add a `MESHPUNK_NO_TTY` branch stubbing `CaptureKeyboardInput()`,
  `ReadKBByte()` and `IsKBHit()`. Upstream's non-ESP path drives a host terminal via
  `<sys/ioctl.h>`/`<termios.h>`/`<signal.h>`, none of which exist in the module's libc
  surface. The module owns the screen and keyboard and runs with the guest serial port
  disabled, so there is no console to capture. Built with `-DMESHPUNK_NO_TTY`.
- `sb16.c` / `i8257.c`: optimise the SoundBlaster data path. Four independent
  inefficiencies, all measured against `[perf]`'s `dma`/`mix` buckets:
  1. `write_audio()` read up to a full block out of guest PSRAM, THEN computed how
     much ring space was free and discarded the rest — ~980 byte-reads to deliver
     40 bytes, because the ring drains at the guest's sample rate while the caller
     asks for a whole block each time. The free-space calculation now runs first and
     clamps the read; an empty ring breaks out instead of reading and dropping.
  2. `i8257_dma_read_memory()` copied byte-at-a-time with a two-condition bounds
     test per byte. Now bounds-clamped once and `memcpy`d — the call upstream left
     commented out under each loop. Tail-beyond-`phys_mem_size` behaviour is
     unchanged (left untouched, as the original loops did).
  3. The resamplers indexed the ring with `% itlen`, a real integer divide per
     output sample (~51k/s) since `itlen` is a runtime parameter. Now `& (itlen-1)`,
     with a `_Static_assert` pinning `AUDIO_BUF_LEN` to a power of two.
  4. `approx_frac()` — a continued-fraction loop of divisions — ran on every mixer
     call to re-derive the same 44100/`freq` ratio. Memoised on the input pair.
  Also `TMPBUF_LEN` is now `#ifndef`-wrapped and built as 512 (upstream's own
  BUILD_ESP32 value); the 4096 desktop default was a 4KB stack frame per call on
  the module's 64KB stack.
- `pc.c`: wrap `MIXER_BUF_LEN` and `PC_STEP_COUNT` in `#ifndef` so the build can
  override them. Upstream hard-codes one pair for desktop (2048 / 10240) and another
  under `BUILD_ESP32` (128 / 512); we are neither. `PC_STEP_COUNT` bounds how long
  `pc_step()` runs before the main loop can feed the watchdog, and `MIXER_BUF_LEN`
  sizes a stack array inside `mixer_callback()`, which the firmware calls on its own
  sound task. Built with `-DPC_STEP_COUNT=512 -DMIXER_BUF_LEN=512`.

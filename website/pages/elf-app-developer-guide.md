# Meshpunk ELF App Developer Guide

Passages marked **(Coming)** describe T-Display P4 (RISC-V) support that exists in the development tree but has not shipped in any release. Everything else is true of v0.5.0.

**Contents:** [Overview](#overview) · [Before you start](#before-you-start) · [The module contract](#the-module-contract) · [Writing the module](#writing-the-module) · [Building the module](#building-the-module) · [The launcher app](#the-launcher-app) · [Controls](#controls) · [Testing on hardware](#testing-on-hardware) · [Publishing to the App Library](#publishing-to-the-app-library) · [Release checklist](#release-checklist) · [Key files](#key-files)

## Overview

A native Meshpunk app has two parts: a Lua launcher and an ELF module. The device runs one app at a time. The launcher sets everything up, then `_launch_elf` hands the whole device to the module until its `main()` returns.

One run, start to finish:

1. The launcher (Lua) collects settings and calls `_launch_elf`.
2. The firmware tears Lua down and loads the module.
3. The module's `main()` owns the device.
4. When `main()` returns, the firmware rebuilds Lua and shows the home screen.

While the module runs there is no Lua, no app interface and no switching apps. Messages still arrive, because the radio runs in firmware on Core 1. The sections below follow the order you build things in.

## Before you start

You build the module on a PC with the same scripts the bundled games use, and the device loads it when the user presses Play.

**What you need**

- **The firmware repo.** Clone [PhilMo6/meshpunk](https://github.com/PhilMo6/meshpunk) (stable) or [PhilMo6/meshpunk-dev](https://github.com/PhilMo6/meshpunk-dev) (newest). Put your module in `modules/<name>/` beside the bundled ones; the build scripts expect that layout. (Coming) The next scripts also read `src/elf_host.cpp` from there to check your imports.
- **The apps repo, if you will publish.** Clone [PhilMo6/meshpunk-apps](https://github.com/PhilMo6/meshpunk-apps) beside the firmware folder. Scripts such as `modules/doom/build.ps1` copy their output to `..\..\..\meshpunk-apps\apps\<App>\`.
- **PlatformIO.** Building the firmware once (`pio run` in its folder) installs `toolchain-xtensa-esp32s3` (GCC 8.4.0, esp-2021r2-patch5) under `%USERPROFILE%\.platformio\packages\`. One catch on a fresh machine: the firmware's `platformio.ini` pins `toolchain-riscv32-esp` to the copy the P4 project installs under `%USERPROFILE%\.platformio\tools\`, so build `p4/` once before any S3 environment. That build also brings the P4's `toolchain-riscv32-esp` (GCC 14.2.0).
- **Windows PowerShell.** Run a script as `powershell -ExecutionPolicy Bypass -File .\build.ps1`, because Windows refuses unsigned scripts by default. On Linux or macOS, copy the flags into a shell script; the compilers have the same names.
- **A device and a microSD card.** Game data, saves and large assets live on the card.

**Boards a module runs on**

| Board | Chip, PSRAM | Module file | Input while a module runs | Module audio | Firmware |
| --- | --- | --- | --- | --- | --- |
| LilyGo T-Deck / T-Deck Plus | ESP32-S3 (Xtensa), 8 MB | `<name>.app.elf` | keyboard, trackball, touch | speaker or USB audio | stable (v0.5.0) |
| Heltec V4-R8 | ESP32-S3, 8 MB | `<name>.app.elf` | touch pad, IO button | USB audio adapter only; the buzzer plays tones | stable (v0.5.0) |
| Seeed Wio Tracker L2 | ESP32-S3, 8 MB | `<name>.app.elf` | touch pad, WAKE button | speaker or USB audio | stable (v0.5.0) |
| LilyGo T-Display P4 | ESP32-P4 (RISC-V), 32 MB | `<name>.app.rv32.elf` | touch, BOOT button | speaker | coming (development tree, no release yet) |

Every board gives a module the same 320x240 RGB565 screen, so one source tree serves all of them. The three S3 boards run the same `.app.elf`. The P4 needs a RISC-V build: the App Library catalog already carries per-device file lists for it (`files_rv32`), but the P4 firmware itself has not been released.

## The module contract

A module is an ELF shared object whose entry point is `int main(int argc, char** argv)`. The loader (`lib/elf_loader/elf_loader.c`) copies its segments into one PSRAM block, zeroes `.bss`, applies the relocations, and resolves every undefined symbol by name against `host_exports[]` in `src/elf_host.cpp`. There is no other C library: a name missing from that table stops the load with `unresolved symbol: X` in the serial log.

**What the loader accepts**

- Type `ET_DYN`, linked with `-shared -fPIC -nostartfiles -nodefaultlibs -nostdlib -Wl,-e,main`.
- Relocations: Xtensa `NONE`, `32`, `RTLD`, `GLOB_DAT`, `JMP_SLOT` and `RELATIVE`. (Coming) RISC-V `NONE`, `32`, `RELATIVE` and `JUMP_SLOT`. Any other type fails the load.
- Weak undefined symbols, which resolve to NULL.
- An optional `.iram.text` section, copied into internal SRAM on the S3 boards (see Writing the module).

(Coming) On the P4, a module built for Xtensa is refused with `module built for another CPU`.

**What the loader does not do**

- It runs no C++ static constructors for a game module. A global object with a constructor keeps zeroed memory, including a null vtable pointer. Jet 3D walks its own list (`modules/jet3d/ctors_begin.cpp`, `ctors_end.cpp`); the other modules avoid such globals.
- It provides no exception handling, RTTI or libstdc++ runtime. Compile C++ with `-fno-exceptions -fno-rtti`, as `modules/dos/build.ps1` does.
- It exports no `exit()` or `abort()`. Define both in your module and `longjmp` back to `main`, as `modules/doom/main_tdeck.c` does.

**What happens around main()**

1. The launcher calls `_launch_elf(path, args...)`. The call only records the request: up to 16 arguments of up to 255 characters each.
2. The firmware closes the whole Lua state and frees its memory, so the module gets the PSRAM the interface was using.
3. It reads the file, stops LVGL, relaxes both watchdogs, clears the screen and forces landscape. It reads the host flags `-keymap`, `-trkball`, `-kbtoggle` and `-stackkb` from argv; your module receives them too and must skip them. (Coming) On the P4 it swaps `.elf` for `.rv32.elf` before reading.
4. `main` runs on a new task on Core 0 with a 64 KB stack in internal RAM, falling back to 48 and then 32 KB. `-stackkb N` (16 to 64) caps the first try at N KB.
5. A task on Core 1 samples the keyboard at 100 Hz and the trackball and touch at about 33 Hz, and queues key events for `host_get_key`.
6. When `main` returns, the firmware closes the sockets and files you left open (up to 16 files are tracked), frees leaked `malloc` blocks, stops leftover worker tasks, unloads the module, and rebuilds Lua and the launcher.

A failed load or a non-zero return posts a notification to the bell. A return of -1 reads as `exited early (aborted - missing ROM/file?)`; any other value reads as `exited with error N`.

**Rules for main()**

- Poll `host_should_exit()` every frame and return when it is 1. It turns on after Alt+Backspace is held 1.5 s, the touch QUIT zone is held 0.6 s, or a key bound to Quit is pressed.
- Yield once per frame with `host_sleep_ms(1)`. The radio and SD card share the SPI bus with the screen; Doom's main loop yields for that reason.
- Return 0 on a normal exit.

## Writing the module

Your module reaches the device only through the `host_*` calls and the C library subset in `host_exports[]`. There is no SDK header, so paste the block below into one of your own. `src/elf_host.h` declares only the older calls; every definition is in `src/elf_host.cpp`. Wrap the block in `extern "C"` for C++, and include the toolchain's newlib headers (`stdlib.h`, `string.h`, `stdio.h`, `setjmp.h`) as usual.

```c
#include <stdint.h>
#include <stddef.h>

/* Screen: 320x240, RGB565 with the bytes swapped */
void     host_blit_frame(const uint16_t* px, int w, int h);       /* centered */
void     host_blit_rect(const uint16_t* px, int x, int y, int w, int h);
void     host_blit_frame_async(const uint16_t* px, int w, int h);
void     host_blit_rect_async(const uint16_t* px, int x, int y, int w, int h);
void     host_blit_wait(void);
void     host_clear_screen(void);
/* Time */
uint32_t host_get_ticks_ms(void);
uint32_t host_get_ticks_us(void);
void     host_sleep_ms(uint32_t ms);
/* Input */
int      host_get_key(int* pressed, unsigned char* key);
void     host_trackball_read(int* dx, int* dy, int* click);
int      host_trackball_button(void);
int      host_key_mods(void);               /* bit 0 Shift, 1 Alt, 2 Sym */
void     host_kb_blink(void);
int      host_should_exit(void);
/* Sound */
void     host_audio_push(const int16_t* samples, int count, int sample_rate);
void     host_audio_set_pull(void (*cb)(int16_t* out, int count), int sample_rate);
/* Files: drive-prefixed paths, S:/... or L:/... */
void*    host_read_file(const char* path, uint32_t* out_size);
int      host_write_file(const char* path, const void* data, uint32_t size);
/* Memory and worker tasks */
void*    host_malloc_internal(size_t size);
uint32_t host_psram_largest_free(void);
void*    host_spawn_task(void (*fn)(void*), void* arg, int core, int prio, int stack_kb);
int      host_task_join(void* handle, int timeout_ms);
/* Debug */
void     host_log(const char* msg);
void     host_check_heap(const char* tag);
/* WiFi and the cable link: host_net_*, host_udp_*, host_tcp_*, host_link_*
   are declared in src/elf_host.h */
```

A starting point, using the same patterns as Doom and the NES module:

```c
#include <stdint.h>
#include <stdlib.h>
#include <setjmp.h>

extern void     host_blit_frame(const uint16_t* px, int w, int h);
extern int      host_get_key(int* pressed, unsigned char* key);
extern int      host_should_exit(void);
extern void     host_sleep_ms(uint32_t ms);
extern void     host_log(const char* msg);

// No exit()/abort() export: trap them back into main().
static jmp_buf s_exit;
static int     s_code;
void exit(int code) { s_code = code; longjmp(s_exit, 1); }
void abort(void)    { exit(1); }

// 256-entry palette, already byte-swapped for the panel.
static uint16_t pal565[256];
static void set_palette(const uint8_t* rgb) {          // 256 x R,G,B
    for (int i = 0; i < 256; i++) {
        uint16_t c = ((rgb[i*3] & 0xF8) << 8) | ((rgb[i*3+1] & 0xFC) << 3) | (rgb[i*3+2] >> 3);
        pal565[i] = (uint16_t)((c >> 8) | (c << 8));
    }
}

int main(int argc, char** argv) {
    if (setjmp(s_exit)) return s_code;
    uint8_t*  screen = malloc(320 * 200);              // the game's 8-bit frame
    uint16_t* frame  = malloc(320 * 200 * 2);
    if (!screen || !frame) return -1;
    host_log("mygame: started");
    while (!host_should_exit()) {
        int down; unsigned char key;
        while (host_get_key(&down, &key)) { /* hand to the game */ }
        /* run one game tick into screen[] */
        for (int i = 0; i < 320 * 200; i++) frame[i] = pal565[screen[i]];
        host_blit_frame(frame, 320, 200);                // centered: 20 px bars
        host_sleep_ms(1);                               // let radio and SD in
    }
    free(frame); free(screen);
    return 0;
}
```

### Screen

- The module screen is 320x240 on every board. `host_blit_frame(px, w, h)` centers a w x h frame, so 320x200 gets 20-pixel bars top and bottom.
- Pixels are RGB565 with the two bytes swapped: store `(c >> 8) | (c << 8)`. For a palette game, convert through a swapped 256-entry table, as `modules/nes/osd_tdeck.c` does.
- `host_blit_rect(px, x, y, w, h)` pushes a strip at absolute coordinates. The `_async` forms push from Core 1 while you draw into a second buffer; alternate two buffers and call `host_blit_wait()` before freeing either.
- There is no DMA on the shared bus. A 256x224 frame costs about 18 ms of Core 1 time on the T-Deck (11.5 ms of wire time at 80 MHz, at about 65% driver efficiency), so budget about 20 ms for a full 320x200 frame.

### Input

`host_get_key(&pressed, &key)` returns queued press and release edges from a 32-slot queue. Letters arrive lowercase, with Shift as its own key.

| Code | Meaning |
| --- | --- |
| `0x20`-`0x7E` | Keyboard characters; the Sym layer adds digits and symbols |
| `0x08`, `0x0D` | Backspace, Enter |
| `0x1B` | Shift+Backspace (the T-Deck has no Esc key) |
| `0x7F` | Delete (USB keyboard) |
| `0x80` | Shift |
| `0x81`-`0x84` | Up, down, left, right: trackball or USB arrow keys |
| `0x85` | Trackball click |
| `0x86`-`0x8A` | Home, End, PgUp, PgDn, Insert (USB keyboard) |
| `0x8B`-`0x8F` | Ctrl, Alt, GUI, CapsLock, NumLock (USB keyboard) |
| `0x90`-`0x9F` | Reserved |
| `0xA0`-`0xA3` | PrtSc, ScrollLock, Pause, Menu (USB keyboard) |
| `0xB0`-`0xBB` | F1-F12; on the T-Deck, Alt+1 to Alt+0 give F1-F10 |
| `0xA4`-`0xAF`, `0xC0`-`0xFC` | Free for your own action codes |

Give each game action a fixed code and let the launcher bind keys to it (see Controls). `0xFF` (quit) and `0xFD` (screenshot) are reserved; the firmware swallows them.

- **Pointer.** `host_trackball_read(&dx, &dy, &click)` returns raw motion. The first call switches the trackball to raw mode for the rest of the run, so it stops sending `0x81`-`0x85`. A USB mouse's motion lands in the same counters.
- `host_trackball_button()` is the live trackball press level, and `host_key_mods()` returns the live modifiers (bit 0 Shift, bit 1 Alt, bit 2 Sym). `modules/dos/dos_input.c` turns all three into a PS/2 mouse.
- **Touch.** A module never sees touch coordinates. The launcher hands the firmware a layout of rectangles that each send one of your codes, with several held at once. A pointer-driven game on a touch board therefore needs a cursor moved by keys.

### Sound

- `host_audio_push(samples, count, rate)` takes mono signed 16-bit PCM. The mixer upsamples by 44100 / rate, limited to 1-4, so use 11025, 22050 or 44100 Hz.
- Samples go into a 4096-sample ring (about 93 ms at 44.1 kHz) that drops the newest when full. Keep a lead queued: Doom runs about 85 ms ahead and refills twice per frame.
- Aim for peaks near plus or minus 16000, the level the firmware's own tones use; louder mixes clip when sounds overlap.
- `host_audio_set_pull(cb, rate)` makes the sound task on Core 1 call `cb` for exactly the samples it needs. The callback must not block or call `malloc`/`free`; unregister with NULL before freeing what it reads. Pull mode cost the SNES module about a third of its frame rate, so most modules push.

### Files

- `fopen`, `fclose`, `fread`, `fwrite`, `fseek` and `ftell` take the SPI bus lock for you and bounce PSRAM transfers through an 8 KB internal buffer. `fgets`, `fprintf`, `getc` and the other stdio calls are exported as plain newlib calls, without that wrapper.
- `fopen` paths are VFS paths: `/sd/...` for the card, `/littlefs/...` for internal flash. `host_read_file` and `host_write_file` take drive-prefixed paths instead (`S:/...`, `L:/...`; no prefix means the card).
- `host_write_file` creates missing folders. `host_read_file` returns a buffer you release with `free()`.
- There is no `opendir`, `readdir` or `stat`. Have the launcher pass the file names you need; it can list folders with `lib/fileman`.
- Strip a trailing slash before `mkdir`: FatFs fails silently on `dir/`, which once broke Doom's saves.
- Missing or stubbed: `fseeko` and `ftello` (build with `-Dfseeko=fseek -Dftello=ftell`), and `tmpfile`, `tmpnam`, `freopen` and `getenv`, which return NULL.
- `time()` reads a clock nothing sets during a module. Pass real time in argv: the GameBoy launcher sends `-unixtime` from `_rtc_time()`.

### Memory

- `malloc`, `calloc`, `realloc` and `free` come from PSRAM (8 MB on the S3 boards, shared with the firmware). The host frees anything you leak, and the launch log prints free PSRAM and the largest block.
- Code, data and `.bss` load as one contiguous PSRAM block. Allocate multi-megabyte buffers with `malloc` at startup rather than as static arrays, which inflate that one block.
- Internal RAM is scarce. `host_malloc_internal(size)` is for structures of a few hundred bytes touched constantly, and may return NULL. Your 64 KB task stack is internal RAM too, so hot line buffers can live there.

### Timing and worker tasks

- `host_get_ticks_ms()`, `host_get_ticks_us()` and `host_sleep_ms(ms)` cover timing.
- `host_spawn_task(fn, arg, core, prio, stack_kb)` starts at most 2 workers (core 0 or 1, priority 1-4, 2-16 KB stack). Signal them to stop and `host_task_join(handle, timeout_ms)` each one before `main` returns.
- WiFi sockets (`host_udp_*`, `host_tcp_*`, up to 4) and the T-Deck cable link (`host_link_*`) are there if your game needs them.

### Speed on the S3 boards

- Code runs from PSRAM through a 16 KB instruction cache and a 32 KB data cache shared by both cores. The bundled emulators are memory-bound: `-O3` and similar flag changes measured as no gain.
- Compile most files `-Os` and the one or two hottest `-O2`, as Doom and DOS do.
- A small hot path can run from internal SRAM: put it in section `.iram.text` and compile that file with `-mtext-section-literals`. The section attribute stops GCC inlining the function, so check the result with `nm`.

## Building the module

Start from the build script of the module closest to yours and edit its source lists. `modules/dos/build.ps1` is the most complete template: hidden visibility, dead-code removal, `-lgcc` after the objects, and an `.iram.text` audit. `modules/doom/build.ps1` is the plainest C game port. Each script compiles changed sources into `obj\`, links `<name>.app.elf`, then lists every undefined symbol for you to check against `host_exports[]`. (Coming) The next scripts do that comparison themselves, mark misses with `<-- NOT IN host_exports[]`, and add `-Arch rv32` for the P4.

```powershell
cd modules\mygame
powershell -ExecutionPolicy Bypass -File .\build.ps1             # <name>.app.elf for the S3 boards
# Coming: powershell -ExecutionPolicy Bypass -File .\build.ps1 -Arch rv32  -> <name>.app.rv32.elf for the P4
```

**Flags and why they are there**

| Flag | Reason |
| --- | --- |
| `-shared -fPIC` | The loader takes only `ET_DYN` shared objects. |
| `-mlongcalls` (Xtensa) | Host functions sit beyond the reach of a direct call from PSRAM. |
| `-nostartfiles -nodefaultlibs -nostdlib -Wl,-e,main` | No startup code and no C library; `main` is the entry point. |
| `-fvisibility=hidden` and `-Wl,-Bsymbolic` | Internal globals bind directly instead of through the GOT; only host imports stay dynamic. About 10% smaller on the NGPC module. |
| `-ffunction-sections -fdata-sections -Wl,--gc-sections` | Drops unused code and data. |
| `-Wl,-z,nocombreloc` (Xtensa) | Without it, any code change can trip an assert in the Xtensa linker (`elf32-xtensa.c:3288`). |
| `-Wl,--no-relax` | Xtensa relaxation in this binutils is unreliable. (Coming) The RISC-V build compiles with `-mno-relax` for the same reason. |
| `-lgcc` after the objects (Xtensa) | Links 64-bit and soft-float helpers into the module. Placed before the objects it pulls nothing, and the helpers must then be host exports. |
| `-DNDEBUG` | `assert()` would import `__assert_func`, which is not exported. |
| (Coming) `-march=rv32imafc_zicsr_zifencei -mabi=ilp32f -mno-relax`, no `-lgcc` (RISC-V) | Matches the P4 firmware. Its `libgcc.a` is not position-independent, so every helper must be a host export. |

**Check the output before you copy it**

1. Look for the success line, such as `Success: doom.app.elf (624.8 KB)`.
2. Read the undefined-symbol list under it and find every name in `host_exports[]` in `src/elf_host.cpp`. A name that is not there means the device will refuse the module. Avoid that call, implement it in your module, or ask for a new export, which needs a firmware release and a higher `min_fw`. (Coming) The next scripts print `<-- NOT IN host_exports[]` after such a name, and the RISC-V build stops on one.
3. Check by hand with `xtensa-esp32s3-elf-readelf -W --dyn-syms mygame.app.elf`, looking at the `UND` rows.
4. After editing a shared header, delete `obj\` and rebuild. The scripts recompile a source only when it is newer than its object; `dos/build.ps1` also checks the newest header.
5. Compare the new file with the old one (`fc /b old.elf new.elf`). Identical bytes after a source edit mean your change never reached the binary.

Keep `.ps1` files ASCII-only. Windows PowerShell 5.1 reads a script without a byte-order mark as Windows-1252, so an em dash or a curly quote breaks the parser far from the real line.

## The launcher app

The launcher is an ordinary Meshpunk Lua app: its `main.lua` draws the menu, stores settings and starts the module with `_launch_elf`. Copy `apps/GameBoy/` from meshpunk-apps (about 680 lines) and rename it; it is the template the other game launchers follow. It picks ROMs with `lib/filepick`, so a launcher copied from it needs `min_fw = 15`; drop the picker if you need a lower level.

**Folder**

```text
Games/MyGame/
  main.lua             launcher (a folder with main.lua is an app)
  readme.lua           help page for the Read Me app
  mygame.app.elf       the module (Coming: a mygame.app.rv32.elf beside it for the P4)
  controls.cfg         written by the launcher at run time
```

Apps live in `L:/lua/apps/<Category>/<App>/` on internal flash or `S:/meshpunk/apps/<Category>/<App>/` on the card. To test, copy your folder to `/meshpunk/apps/Games/MyGame/` on the SD card and restart: the app list is scanned at boot.

**What main.lua receives and returns**

- `local app_dir = ...` is its own folder as a drive path, such as `S:/meshpunk/apps/Games/MyGame`.
- Return nothing once the screen is built, or return a function. The loader then calls that function once per tick until it returns true; GameBoy scans its ROM folders this way so a big folder cannot trip the watchdog.
- Make exactly one root with `apps.new_root{...}` and swap views as its children. Leave with `apps.go_home()`.
- Swap views as GameBoy's `show_screen` does: build the new container, call `nav.replace(new, { flags = nav.ROLLOVER + nav.SCROLL_FIRST })`, then `apps.delete_view(old)`. Keep every button a direct child of that container so trackball navigation reaches it.

**Paths**

Lua uses drive prefixes (`L:` flash, `S:` card); the module's `fopen` needs VFS paths. Convert every file name you pass:

```lua
local function to_vfs_path(p)
    if p:sub(1, 2) == "S:" then return "/sd" .. p:sub(3) end
    if p:sub(1, 2) == "L:" then return "/littlefs" .. p:sub(3) end
    return p
end
```

The `.elf` path itself stays in drive form. (Coming) On the P4 the firmware swaps `.elf` for `.rv32.elf`, so one launcher serves every board.

**Launching**

```lua
playBtn:onClicked(function()
    status:set{ text = "Loading..." }
    save_config()
    local km = kb:keymap_string()                 -- nil when nothing is bound
    lvgl.Timer{ period = 50, cb = function(t)
        t:delete()
        local args = { app_dir .. "/mygame.app.elf",
                       "-data", to_vfs_path(DATA_DIR),
                       "-trkball", kb:trkball_string() }
        local ok, ts = pcall(_rtc_time)           -- module time() is not set
        if ok and type(ts) == "number" and ts > 0 then
            args[#args + 1] = "-unixtime"; args[#args + 1] = tostring(ts)
        end
        if km then args[#args + 1] = "-keymap"; args[#args + 1] = km end
        if _elf_touch_layout and pad then         -- touch boards
            local tl = pad:zones()
            if tl then _elf_touch_layout(tl) end
        end
        _launch_elf(table.unpack(args))
    end }
end)
```

The 50 ms timer lets "Loading..." draw before Lua is torn down. Save settings first: when the module exits, the device rebuilds Lua and shows the home screen, not your launcher.

**Help page and credits**

- `readme.lua` runs in a sandbox with no `io`, `os` or `require`. It receives `local caps, dev = ...` and returns `{ body = text }`. `caps` has `keyboard`, `trackball`, `touch` and `kbd_backlight`; `dev` has `name`, `audio`, `screen_w`, `screen_h` and, from firmware 15, `usb_power`. The Read Me app lists the page automatically.
- Describe the device being read: branch on `caps.keyboard` for controls, as `apps/GameBoy/readme.lua` does. Write keys as `name - value` lines, because the theme font is proportional and space-aligned columns break.
- Give the launcher an About screen naming the engine, its license and where its source is published, plus a line saying no game data is included.

## Controls

Bindings live in the launcher, not the module. `lib/keybind` turns the user's choices into the `-keymap` string the firmware applies, and `lib/padlayout` gives touch boards an on-screen pad. Both ship with the firmware (`FW_API` 8 and 9).

**Key bindings (lib/keybind)**

```lua
local ACTIONS = {
    { id = "up",    label = "Up",     out = 0xC0, key1 = "w", key2 = "TrkUp"  },
    { id = "click", label = "Select", out = 0xC1, key1 = "m", key2 = "TrkClk" },
    { id = "menu",  label = "Menu",   out = 0x1B, key1 = "BkSpc" },
}
kb = keybind.new{
    actions = ACTIONS, root = root, show_screen = show_screen,
    font = FONT, accent = ACCENT,
    on_back = function() create_main_screen() end, on_save = save_config,
    trackball = { momentum = true, impulse = 15, friction = 82, thresh = 4 },
}
```

- `out` is your module's code for the action; `key1` and `key2` are default keys by name (`a`-`z`, `$`, `Space`, `Enter`, `BkSpc`, `Shift`, `TrkUp`, `TrkDn`, `TrkLt`, `TrkRt`, `TrkClk`) or by code.
- keybind adds a Quit action (default `y`, firmware code `0xFF`) and an unbound Screenshot action (`0xFD`) to every launcher.
- `kb:open()` shows the Controls screen. `kb:keymap_string()` gives the `-keymap` value and `kb:trkball_string()` the `-trkball` value. `kb:save_lines(f)` and `kb:load_line(line)` keep the bindings in your own config file.
- The keymap is a pure remapper: keys that are not bound reach the module with their raw code. With ASCII action codes (GameBoy uses `w` for Up) the default key keeps working after a rebind; codes from the free ranges (`0xC0` and up) act only through a binding.
- Match the other launchers where an action has an obvious twin: A = `m` (and trackball click), B = `n`, X = `k`, Y = `j`, Start = Enter, Back = Backspace.

**Leaving a game**

- Alt+Backspace held 1.5 s (built-in or USB keyboard), the Quit key, or the touch QUIT pad held 0.6 s. Your module only sees `host_should_exit()` turn 1.
- On a T-Deck whose keyboard runs old firmware (legacy mode), holds and chords do not register, so the Quit key is the only keyboard exit. Mention it in your help text.

**Typing during play**

If the game needs real text entry, such as save names, pass `-kbtoggle 1`. Alt+Enter then switches the bindings off and on, and the keyboard backlight blinks once; `-kbtoggle 0` starts with bindings off. On a legacy keyboard the same switch is `p`, Backspace, Enter in a row.

**Touch pads (lib/padlayout)**

- A preset is a list of zones `{ id, out, label, x, y, w, h }` in 320x240 screen coordinates: up to 48 zones, labels up to 7 characters. The panel sets how many fingers count at once: five on the T-Deck and Wio L2 (GT911), two on the Heltec (CHSC6X).
- Add your own QUIT zone with `out = keybind.QUIT`; the firmware fires it only after a 0.6 s hold. A Screenshot pad is added to every preset for you.
- `pad:open{ show_screen, font, accent, on_back }` is the editor where the user drags, resizes or turns off pads. Edits are saved in `L:/touch_layouts/<app>.cfg` by zone id, so changing out codes later keeps them.
- Feature-check both halves, because a store app can land on older firmware: `pcall(require, "lib/padlayout")`, then `if _elf_touch_layout and pad then`. Call `_elf_touch_layout(pad:zones())` before every `_launch_elf`.
- During play, the board's button or Shift+Alt cycles touch mode: off, pad, pad with labels hidden, on-screen keyboard. The keyboard sends plain ASCII to your module.

## Testing on hardware

The serial log tells you almost everything: open the board's USB port at 115200 baud (for example `pio device monitor -b 115200`) before you press Play. The device runs one thing at a time, so plan each test as configure in the launcher, play, quit, change, play again.

**Log lines to look for**

| Line | What it tells you |
| --- | --- |
| `[elf_host] PSRAM free: N, largest block: M` | Memory available before your module loads |
| `[elf_loader] total load size: N bytes` | The one PSRAM block your code, data and `.bss` need |
| `[elf_loader] load base: 0x..., bias: 0x...` | Where the module landed; you need the bias to decode a crash |
| `[elf_loader] ERROR: unresolved symbol: X` | You import something `host_exports[]` lacks |
| `[elf_host] task created with 64KB stack` | Which stack size fitted |
| `[elf_mod] ...` | Your own `host_log()` text |
| `[elf_host] module task finished (result=R), min stack free: N bytes` | Exit code and stack headroom; lower `-stackkb` only with headroom to spare |
| `[elf_host] freed N leaked module allocations` | Blocks you did not free; harmless, but worth knowing |

With Lua torn down, a failed launch would otherwise look like the app just closing. The firmware therefore also posts each failure to the notification bell, such as `App launch failed: mygame.app.elf didn't load (PSRAM largest 1234KB)`.

**Reading a crash (S3 boards)**

A CPU fault inside your module prints a block starting `*** [ELF FAULT] core=0 exccause=28 PC=0x42xxxxxx excvaddr=0x...`, then registers and stack words, and the watchdog resets the device. Your module's code runs from the PSRAM instruction bus, 0x06000000 above its data address, so its link address is:

```text
offset = PC - 0x06000000 - bias          (bias from the load-base line)
xtensa-esp32s3-elf-addr2line -f -C -e mygame.app.elf 0x<offset>
```

Cause 28 (LoadProhibited) and 29 (StoreProhibited) are bad pointers, often NULL plus a small offset in `excvaddr`. Cause 3 (LoadStoreError) is a byte or halfword access to instruction RAM. (Coming) The P4 build has no module fault hook yet, so faults there go to the standard ESP-IDF panic output.

**Habits that save time**

- Keep a perf line: `host_log` frames per second, audio samples delivered and the worst frame gap every 5 s. Remove it before release.
- `host_check_heap("tag")` prints the stack headroom and both heaps' integrity, which narrows a corruption to the step that caused it.
- A USB device in host mode (gamepad, mouse, audio) takes over the USB port, so serial output stops. Test with serial first, then with accessories.
- Test on each board you claim. The T-Deck has a keyboard and trackball; the Heltec and Wio L2 have only touch and one button.

## Publishing to the App Library

The App Library downloads `catalog.toml` from [meshpunk-apps](https://github.com/PhilMo6/meshpunk-apps) over WiFi and installs every file an entry lists; an app exists for devices only once its entry is merged. You publish by pull request with three parts: the app files under `apps/<id>/`, the module's full source under `module-src/<name>/`, and the catalog entry.

```toml
[[apps]]
id = "MyGame"                  # folder under apps/: letters, digits, - and _
name = "My Game"                # install folder and launcher label
author = "Your name"
version = "1.0.0"
type = "elf"                    # the App Library labels it [ELF]
description = "One line shown in the App Library"
category = "Games"
min_fw = 9                      # 15 if the launcher uses lib/filepick, as the GameBoy template does
files = ["readme.lua", "main.lua", "mygame.app.elf"]
```

**Entry rules**

- `files` lists every file, including `readme.lua` and data in subfolders (`games/level1/map.bin` is fine). Anything not listed is never downloaded.
- A device offers an update only when the catalog `version` is numerically newer than the installed one. To roll back, publish the old files under a higher version.
- Updates overwrite in place, so `controls.cfg` and saves in the app folder survive; files you drop from `files` stay behind on devices.
- Bump the version once per release. Further changes before it is merged ride in the same version.
- `drivers = ["gamepad"]` installs USB drivers alongside the app.
- (Coming) `files_rv32 = [...]` names the P4 build's files; a P4 device downloads that list instead of the Xtensa modules.

**Choosing min_fw**

A device below `min_fw` shows "Needs FW" instead of Install. Set it to the level of the newest feature you use: a missing host export stops the module loading, and a missing Lua library stops the launcher.

| Feature | Level |
| --- | --- |
| `lib/filepick`, `kb:keymap_string(opts)` | 15 (stable since v0.5.0) |
| `host_net_*`, `host_udp_*`, `host_tcp_*`, `host_link_dgram_*` | 13 |
| `lib/padlayout`, `_elf_touch_layout` | 9 |
| `lib/keybind`, `-stackkb`, `host_blit_rect_async`, `host_blit_wait` | 8 |
| `host_trackball_button`, `host_key_mods`, `host_kb_blink`, `truncate`, `-kbtoggle` | 6 |
| `host_spawn_task`, `host_task_join` | 4 |
| `host_link_*` cable link | 2 |

The stable release v0.5.0 is level 15. To check a module, list its `UND` symbols with `readelf --dyn-syms` and find each one in `git show v0.5.0:src/elf_host.cpp`. Link `-lgcc` after your objects so libgcc helpers added since the release are never imported.

**Source, licenses and credits**

- Copy the module's whole tree (sources, `build.ps1`, license files, the built `.elf`) to `module-src/<name>/`. Most engines are copyleft, and the source has to travel with the binary the store distributes.
- Keep upstream code in its own folder with a `VENDOR.md` naming the upstream URL and commit, the licenses and every local edit, and tag each edit `MESHPUNK` in the source. `module-src/dos/tiny386-src/VENDOR.md` is the model.
- Credit the engine in five places: the launcher's About screen (pointing at `module-src/<name>`), the first line of `readme.lua`, the license files in the module tree, `VENDOR.md`, and the firmware README's Credits list (ask Phil to add it).
- Never ship copyrighted game data. Say "supply your own" and look for the files on the card.
- ELF apps get extra review: a module has full access to the hardware.

## Release checklist

- [ ] The module's undefined-symbol list contains only names that are in `host_exports[]`, for each architecture you ship.
- [ ] `main` polls `host_should_exit()`, yields every frame and returns 0; `exit()` and `abort()` are trapped.
- [ ] Worker tasks are joined, pull audio is unregistered and async blits are waited on before anything is freed.
- [ ] Tested on every board you claim: keyboard and trackball on the T-Deck, touch pad and QUIT hold on the Heltec V4 or Wio L2.
- [ ] The launcher makes one root with `apps.new_root`, saves settings before `_launch_elf`, converts paths to `/sd/...` and leaves with `apps.go_home()`.
- [ ] keybind actions have sensible defaults, and the pad preset has a QUIT zone; both are feature-checked.
- [ ] `readme.lua` reads correctly on keyboard and touch devices and is in `files`.
- [ ] The About screen and the first line of `readme.lua` credit the engine and point at `module-src/<name>`.
- [ ] `module-src/<name>/` holds the full tree, license files, `VENDOR.md` and an `.elf` byte-identical to the one in `apps/<id>/`.
- [ ] The catalog entry lists every file, has a new version, and its `min_fw` was checked against the release tag.
- [ ] Perf lines and debug logging are removed.
- [ ] No copyrighted game data is anywhere in the pull request.

## Key files

| File | Read it for |
| --- | --- |
| `src/elf_host.h` | The older `host_*` prototypes; the full set is in Writing the module |
| `src/elf_host.cpp` | `host_exports[]`, the input task, blit paths, launch and cleanup |
| `lib/elf_loader/elf_loader.c` | Accepted relocations and `.iram.text` (Coming: the P4 cache sync) |
| `src/version.h` | `FW_API` history: what each level added, the basis for `min_fw` |
| `src/display/display_dev.h` | The 320x240 module video contract |
| `src/usb/usb_drv_hid.cpp` | Key codes a USB keyboard sends |
| `modules/test_elf/` | The smallest module: a test pattern and an exit wait |
| `modules/doom/` | A plain C game port: exit trap, color conversion, push audio, OPL music |
| `modules/dos/` | The fullest build script, mouse emulation and PC scancodes (`dos_input.c`) |
| `modules/jet3d/` | A C++ module that runs its own static constructors |
| `data/lua/lib/apps.lua` | The app contract: `new_root`, `go_home`, `delete_view`, phased init |
| `data/lua/lib/keybind.lua`, `padlayout.lua`, `fileman.lua` | Launcher libraries for bindings, touch pads and file listing |
| `meshpunk-apps/apps/GameBoy/` | The reference launcher and `readme.lua` |
| `meshpunk-apps/README.md`, `catalog.toml` | Catalog fields and every published entry |
| `meshpunk-apps/module-src/` | Published module sources and their `VENDOR.md` files |

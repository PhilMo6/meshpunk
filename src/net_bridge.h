#pragma once

// ELF-module network exports (WiFi state + lwIP UDP/TCP sockets). The
// module-facing functions are declared with the rest of the host contract in
// elf_host.h; this header carries only the firmware-side hook. Kept in its
// own translation unit so lwIP's socket headers stay out of elf_host.cpp.

// Close every socket a module left open and restore WiFi modem sleep.
// elf_host calls it after every module run (normal exit, exit() longjmp,
// crash) — a module has no other way to leak a socket or a bound port.
void net_bridge_close_all();

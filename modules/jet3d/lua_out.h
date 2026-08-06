// Redirects Lua's console output away from raw stdio.
//
// Lua writes print() output with fwrite(stdout) and errors/warnings with
// fprintf(stderr) (see llimits.h). Those exported stdio functions reach the UART
// WITHOUT taking SLOG_LOCK, so they race the firmware tasks that do take it —
// notably the mesh task logging RX on Core 1 while this module runs on Core 0.
// Everything here funnels into host_log(), which goes through SerialMux under
// the lock.
//
// llimits.h guards all three with #if !defined(...), so force-including this
// header ahead of the Lua sources (see -include in build.ps1) overrides them
// without editing the vendored tree.
//
// Only console output is redirected. Lua's io library still uses fwrite/fread on
// real FILE handles, which the host already wraps with the SPI lock.

#ifndef JET_LUA_OUT_H
#define JET_LUA_OUT_H

#include <stddef.h>

#if defined(__cplusplus)
extern "C" {
#endif

// Accumulates into a line buffer and emits on newline or when full: print()
// arrives in several pieces (value, tab, value, newline) and one log line per
// piece would be unreadable.
void jet_lua_write(const char* s, size_t len);

// Every lua_writestringerror call site passes a string as the single argument.
void jet_lua_write_err(const char* fmt, const char* arg);

#if defined(__cplusplus)
}
#endif

#define lua_writestring(s, l)       jet_lua_write((const char*)(s), (size_t)(l))
#define lua_writeline()             jet_lua_write("\n", 1)
#define lua_writestringerror(s, p)  jet_lua_write_err((s), (const char*)(p))

#endif // JET_LUA_OUT_H

// Lua engine layer for the jet3d module.
//
// The module runs its OWN Lua interpreter, statically linked into the ELF and
// unrelated to the firmware's Lua state (which the host tears down before any
// ELF module starts). Game code is a directory on disk containing main.lua.
//
// Callbacks a game may define as globals:
//   jet.load()        once, after main.lua has been executed
//   jet.update(dt)    every frame, dt in seconds
//   jet.draw()        every frame, after update and before the scene renders

#ifndef JET_LUA_H
#define JET_LUA_H

#include <stdint.h>

namespace Renderer { class Scene; }

// Create the interpreter and register the jet.* API against `scene`.
// Returns false if the state could not be created.
bool jet_lua_open(Renderer::Scene* scene, int screenW, int screenH);

// Execute a game's main.lua. `path` is a host VFS path ("/sd/games/demo/main.lua").
// The containing directory is registered as the package path root so the game
// can require() its own modules. Returns false and logs on any error.
bool jet_lua_run_file(const char* path);

// Invoke jet.load(). Returns false and logs if the callback raised an error.
bool jet_lua_call_load(void);

// Invoke jet.update(dt) then jet.draw(). Either may be absent.
// Returns false and logs if a callback raised an error.
bool jet_lua_call_frame(float dt);

// Release every Object/Material/Texture the game allocated, then close the
// interpreter. Safe to call when jet_lua_open() failed.
void jet_lua_close(void);

// Key state feed. `key` is the host key code (ASCII for printable keys, the
// module extension codes for the rest); `pressed` is 1 on press, 0 on release.
void jet_lua_set_key(unsigned char key, int pressed);

// Launcher override: when 0, jet.scene.fog() is ignored so a game's fog calls
// cannot re-enable it. Exists so distance behaviour (LOD swaps, draw range)
// can be observed without fog hiding it.
void jet_lua_set_fog_enabled(int on);

// Trackball deltas accumulate until Lua reads them via jet.input.trackball().
void jet_lua_add_trackball(int dx, int dy, int click);

// Per-frame timing values reported by jet.timer.
void jet_lua_set_timing(float fps, float uptime);

// True once the game has called jet.quit().
bool jet_lua_wants_quit(void);

#endif // JET_LUA_H

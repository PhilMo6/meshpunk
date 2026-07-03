#pragma once

// Unified drive-aware filesystem bridge: Lua -> C++ (see fs_bridge.cpp).
// Registers the _fs_* binding family on the given Lua state.

struct lua_State;

void fs_bridge_register(lua_State* L);

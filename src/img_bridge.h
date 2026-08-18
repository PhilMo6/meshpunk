#pragma once

// In-memory image buffers: Lua -> C++ (see img_bridge.cpp).
// Registers the _img_* binding family on the given Lua state.

struct lua_State;

void img_bridge_register(lua_State* L);

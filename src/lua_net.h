#pragma once

// Lua client TCP/TLS sockets (_tcp_open + socket handle methods) serviced by
// a Core-1 worker task. Contract, limits and states: src/lua_net.cpp.

struct lua_State;

void lua_net_register_lua(lua_State *L);

// Close every socket (reason becomes each socket's close reason) and wait for
// the worker task to exit, freeing its internal-SRAM stack. Called before Lua
// teardown and before standby turns WiFi off. Safe when nothing is open.
void lua_net_close_all(const char *reason);

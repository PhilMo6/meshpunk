// T-Deck netgame glue — shared by main_tdeck.c (launcher spec), d_main.c /
// d_loop.c / d_net.c (option application, transport selection, start
// callback), net_tdeck.c (the two transports) and net_tdeck_wait.c (status
// screen + the connect / launch waits).
//
// Every choice is made in the Lua launcher and arrives as ONE argument,
// "-netgame <spec>": comma-separated tokens
//   host | join=usb | join=lan | join=<host[:port]>
//   players=N (host: auto-launch when N players are in, 2..4)
//   dm=0|1|2  skill=1..5  ep=N  map=N  nomon=1  respawn=1  fast=1  timer=MIN
// The game never asks a question of its own: while it connects or waits it
// only shows status text, drawn with the game's own font.

#ifndef NET_TDECK_H
#define NET_TDECK_H

#include "doomtype.h"
#include "net_defs.h"

typedef enum
{
    TDN_MODE_NONE,          // single player (no -netgame)
    TDN_MODE_HOST,
    TDN_MODE_JOIN_USB,
    TDN_MODE_JOIN_LAN,      // broadcast search on the WiFi network
    TDN_MODE_JOIN_ADDR      // join=<host[:port]>
} tdeck_netmode_t;

typedef struct
{
    tdeck_netmode_t mode;
    char addr[64];          // join=<host[:port]>
    int players;            // host: launch when this many players are in
    int deathmatch;         // 0 co-op, 1 deathmatch, 2 altdeath
    int skill;              // 1..5, 0 = game default
    int episode;            // 0 = game default
    int map;                // 0 = game default
    int nomonsters;
    int respawn;
    int fast;
    int timer;              // minutes, 0 = none
} tdeck_netopts_t;

extern tdeck_netopts_t tdeck_netopts;

// Parse a -netgame spec into tdeck_netopts (main_tdeck.c, before D_DoomMain).
void TDeck_NetParseSpec(const char *spec);

// Transports (net_tdeck.c): UDP over the WiFi station, datagrams over the
// USB peer cable. Both implement Chocolate Doom's net_module_t contract.
extern net_module_t net_udp_module;
extern net_module_t net_usb_module;

// The single address on the other end of the USB cable.
net_addr_t *NET_USB_PeerAddress(void);

// Status screen (net_tdeck_wait.c): two centred lines of text on a black
// screen, drawn with the HUD font. Safe from D_ConnectNetGame onward
// (zone, video buffers and the WAD are up by then).
void TDeck_NetStatus(const char *line1, const char *line2);

// Show a message for a moment, leave the netgame cleanly, exit to the
// launcher.
void TDeck_NetFatal(const char *line1, const char *line2);

// D_InitNetGame's body for -netgame: brings up the server (host), finds and
// connects to the host (join), then waits for the launch. Returns true when
// a netgame is on (this client is connected), false for single player.
boolean TDeck_NetConnect(net_connect_data_t *connect_data);

// D_StartNetGame's wait-for-start callback (d_net.c passes it).
boolean TDeck_NetStartCallback(int ready_players, int num_players);

// Service a live netgame connection (send keepalives, take received
// packets). No-op when no netgame is connected; rate-limited internally.
// W_ReadLump calls it so the long lump-I/O phases — engine init, sound
// precache, level loads — cannot starve the connection into its 30-second
// timeout; everywhere else the game's own loops pump.
void TDeck_NetPump(void);

#endif /* NET_TDECK_H */

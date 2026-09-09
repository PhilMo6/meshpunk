// Status screen + the connect / launch waits for T-Deck netgames. Replaces
// Chocolate Doom's net_gui.c (a textscreen dialog): there are no dialogs and
// no questions here — the launcher already made every choice — only status
// lines drawn with the game's own HUD font, and the menu/quit key to back
// out. The host launches the game itself the moment the launcher-chosen
// number of players is in (Chocolate Doom's -nodes rule).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "doomdef.h"
#include "doomtype.h"
#include "doomgeneric.h"
#include "doomkeys.h"
#include "deh_str.h"
#include "hu_stuff.h"
#include "i_system.h"
#include "i_timer.h"
#include "i_video.h"
#include "m_misc.h"
#include "v_patch.h"
#include "v_video.h"
#include "w_wad.h"
#include "z_zone.h"

#include "net_client.h"
#include "net_defs.h"
#include "net_gui.h"        // NET_WaitForLaunch is defined here
#include "net_io.h"
#include "net_loop.h"
#include "net_query.h"
#include "net_server.h"
#include "net_tdeck.h"

extern patch_t *hu_font[HU_FONTSIZE];               // hu_stuff.c
extern void M_WriteText(int x, int y, char *string); // m_menu.c
extern int  M_StringWidth(char *string);
extern void D_QuitNetGame(void);                     // d_loop.c

// net_client.c (MESHPUNK additions): connect timeout + per-iteration poll.
extern int net_connect_timeout_ms;
extern boolean (*net_connect_poll)(void);

// Host exports (elf_host.h).
extern int      host_should_exit(void);
extern int      host_net_status(void);
extern unsigned host_net_local_ip(void);
extern int      host_link_status(void);

#define CONNECT_TIMEOUT_MS  120000

// ── Status screen ────────────────────────────────────────────────────────────

static boolean status_ready = false;
static char status_l1[64];
static char status_l2[64];

// Bring up just enough of the game to draw text: the video buffer, the HUD
// font and the palette. All of it is the game's own; nothing here depends on
// R_Init or the menu having run.
static void StatusInit(void)
{
    if (status_ready)
        return;

    I_InitGraphics();                   // idempotent (MESHPUNK guard)
    V_RestoreBuffer();                  // draw into I_VideoBuffer
    if (hu_font[0] == NULL)
        HU_Init();
    I_SetPalette(W_CacheLumpName(DEH_String("PLAYPAL"), PU_CACHE));

    status_ready = true;
}

static void DrawCentred(int y, char *s)
{
    int x = (SCREENWIDTH - M_StringWidth(s)) / 2;

    if (x < 0)
        x = 0;

    M_WriteText(x, y, s);
}

void TDeck_NetStatus(const char *line1, const char *line2)
{
    if (line2 == NULL)
        line2 = "";

    // Only a change in text is worth a frame.
    if (status_ready
     && strcmp(line1, status_l1) == 0 && strcmp(line2, status_l2) == 0)
    {
        return;
    }

    M_StringCopy(status_l1, line1, sizeof(status_l1));
    M_StringCopy(status_l2, line2, sizeof(status_l2));

    StatusInit();

    memset(I_VideoBuffer, 0, SCREENWIDTH * SCREENHEIGHT);
    DrawCentred(80, status_l1);
    if (status_l2[0] != '\0')
        DrawCentred(96, status_l2);
    DrawCentred(176, "Menu key cancels");

    I_FinishUpdate();
}

// ── Quit handling ────────────────────────────────────────────────────────────

// The launcher's Menu/Esc binding (KEY_ESCAPE) or the firmware's own quit
// (exit chord, bindable Quit) backs out of any wait.
static boolean QuitRequested(void)
{
    int pressed;
    unsigned char key;

    while (DG_GetKey(&pressed, &key))
    {
        if (pressed && key == KEY_ESCAPE)
            return true;
    }

    return host_should_exit() != 0;
}

// Leave the netgame cleanly (peers see a disconnect, not a timeout) and
// return to the launcher as a normal exit.
static void QuitToLauncher(void)
{
    TDeck_NetStatus("Leaving the game...", "");
    D_QuitNetGame();
    exit(0);
}

void TDeck_NetFatal(const char *line1, const char *line2)
{
    unsigned int until = I_GetTimeMS() + 2500;

    printf("netgame: %s %s\n", line1, line2 != NULL ? line2 : "");
    TDeck_NetStatus(line1, line2);

    while ((int) (until - I_GetTimeMS()) > 0 && !QuitRequested())
        I_Sleep(50);

    D_QuitNetGame();
    exit(0);
}

// ── Connection service during I/O stalls ─────────────────────────────────────

// Keepalives are only sent (and the peer's silence timer only reset) when the
// netcode is pumped, and a connection drops after 30 seconds without traffic
// (net_common.c). Engine init, the sound precache and level loads are long
// runs of lump reads with no pump anywhere in them, so W_ReadLump services
// the connection through this. No-op without a netgame; rate-limited here.
void TDeck_NetPump(void)
{
    static unsigned int last_pump;
    unsigned int now;

    if (!net_client_connected)
        return;

    now = I_GetTimeMS();

    if (now - last_pump < 250)
        return;

    last_pump = now;

    NET_CL_Run();
    NET_SV_Run();
}

// ── Connect ──────────────────────────────────────────────────────────────────

static const char *connect_line = "";
static boolean connect_aborted = false;

// Runs inside NET_CL_Connect's loop: keep the status up, watch for quit.
static boolean ConnectPoll(void)
{
    TDeck_NetStatus(connect_line, "");

    if (QuitRequested())
    {
        connect_aborted = true;
        return false;
    }

    return true;
}

static net_addr_t *lan_found = NULL;

static void LanFoundCallback(net_addr_t *addr, net_querydata_t *data,
                             unsigned int ping_time, void *user_data)
{
    if (lan_found == NULL)
        lan_found = addr;
}

// Broadcast queries until a host answers or the user backs out. One query
// round (net_query.c) gives up after ~6s; the host may still be loading its
// WAD, so rounds repeat.
static net_addr_t *FindLanHost(void)
{
    lan_found = NULL;

    for (;;)
    {
        NET_StartLANQuery();

        while (lan_found == NULL && NET_Query_Poll(LanFoundCallback, NULL))
        {
            TDeck_NetStatus("Searching the WiFi network", "for a host...");

            if (QuitRequested())
                QuitToLauncher();

            I_Sleep(10);
        }

        if (lan_found != NULL)
            return lan_found;
    }
}

static void FormatIP(char *buf, int len, unsigned int ip)
{
    M_snprintf(buf, len, "%u.%u.%u.%u",
               (ip >> 24) & 0xff, (ip >> 16) & 0xff, (ip >> 8) & 0xff, ip & 0xff);
}

boolean TDeck_NetConnect(net_connect_data_t *connect_data)
{
    net_addr_t *addr = NULL;
    boolean cable = (host_link_status() & 3) >= 1;
    boolean wifi = host_net_status() != 0;

    switch (tdeck_netopts.mode)
    {
        case TDN_MODE_NONE:
            return false;

        case TDN_MODE_HOST:
            if (!cable && !wifi)
                TDeck_NetFatal("No USB link and no WiFi", "nothing to host on");

            NET_SV_Init();
            NET_SV_AddModule(&net_loop_server_module);
            if (wifi)
                NET_SV_AddModule(&net_udp_module);
            if (cable)
                NET_SV_AddModule(&net_usb_module);
            // Private game: no master-server registration.

            net_loop_client_module.InitClient();
            addr = net_loop_client_module.ResolveAddress(NULL);
            connect_line = "Starting the host...";
            break;

        case TDN_MODE_JOIN_USB:
            if (!cable)
                TDeck_NetFatal("No USB link", "connect the cable and retry");

            addr = NET_USB_PeerAddress();
            connect_line = "Connecting over the USB cable...";
            break;

        case TDN_MODE_JOIN_LAN:
            if (!wifi)
                TDeck_NetFatal("WiFi is not connected", "");

            addr = FindLanHost();
            connect_line = "Connecting to the host...";
            break;

        case TDN_MODE_JOIN_ADDR:
            if (!wifi)
                TDeck_NetFatal("WiFi is not connected", "");

            net_udp_module.InitClient();
            addr = net_udp_module.ResolveAddress(tdeck_netopts.addr);

            if (addr == NULL)
                TDeck_NetFatal("Cannot resolve the address", tdeck_netopts.addr);

            connect_line = "Connecting to the host...";
            break;
    }

    // A joiner may launch before the host has finished loading its WAD, so
    // the connect waits far longer than Chocolate Doom's 5s — cancellable.
    net_connect_timeout_ms = CONNECT_TIMEOUT_MS;
    net_connect_poll = ConnectPoll;
    connect_aborted = false;
    TDeck_NetStatus(connect_line, "");

    if (!NET_CL_Connect(addr, connect_data))
    {
        net_connect_poll = NULL;

        if (connect_aborted)
            QuitToLauncher();

        TDeck_NetFatal("Could not reach the host", "");
    }

    net_connect_poll = NULL;
    printf("D_InitNetGame: Connected to %s\n", NET_AddrToString(addr));

    NET_WaitForLaunch();

    return true;
}

// ── Wait for launch ──────────────────────────────────────────────────────────

static boolean WadMismatch(net_waitdata_t *wd)
{
    return memcmp(net_local_wad_sha1sum, wd->wad_sha1sum,
                  sizeof(sha1_digest_t)) != 0
        || wd->is_freedoom != net_local_is_freedoom;
}

// What the other players can reach this host on.
static void HostAddressLine(char *buf, int len)
{
    unsigned int ip = host_net_local_ip();
    boolean cable = (host_link_status() & 3) >= 1;
    char ipbuf[24];

    if (ip != 0)
    {
        FormatIP(ipbuf, sizeof(ipbuf), ip);
        M_snprintf(buf, len, cable ? "WiFi %s + USB cable" : "WiFi address %s", ipbuf);
    }
    else if (cable)
    {
        M_StringCopy(buf, "USB cable", len);
    }
    else
    {
        M_StringCopy(buf, "", len);
    }
}

void NET_WaitForLaunch(void)
{
    char l1[64], l2[64];
    boolean launched = false;

    while (net_waiting_for_launch)
    {
        NET_CL_Run();
        NET_SV_Run();

        if (!net_client_connected)
            TDeck_NetFatal("Lost the connection", "to the host");

        if (net_client_received_wait_data)
        {
            net_waitdata_t *wd = &net_client_wait_data;

            if (wd->is_controller)
            {
                M_snprintf(l1, sizeof(l1), "Waiting for players: %d of %d",
                           wd->num_players, tdeck_netopts.players);
                HostAddressLine(l2, sizeof(l2));

                if (!launched
                 && wd->num_players + wd->num_drones >= tdeck_netopts.players)
                {
                    NET_CL_LaunchGame();
                    launched = true;
                }
            }
            else
            {
                M_StringCopy(l1, "Waiting for the host to start", sizeof(l1));

                if (WadMismatch(wd))
                    M_StringCopy(l2, "WAD differs from the host!", sizeof(l2));
                else
                    M_snprintf(l2, sizeof(l2), "Players in: %d", wd->num_players);
            }
        }
        else
        {
            M_StringCopy(l1, "Connected", sizeof(l1));
            M_StringCopy(l2, "waiting for game info...", sizeof(l2));
        }

        TDeck_NetStatus(l1, l2);

        if (QuitRequested())
            QuitToLauncher();

        I_Sleep(20);
    }
}

// ── Wait for start (D_StartNetGame's BlockUntilStart) ────────────────────────

boolean TDeck_NetStartCallback(int ready_players, int num_players)
{
    char l2[64];

    M_snprintf(l2, sizeof(l2), "%d of %d players ready", ready_players, num_players);
    TDeck_NetStatus("Starting the game...", l2);

    if (QuitRequested())
        QuitToLauncher();

    return true;
}

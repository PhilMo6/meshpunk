// T-Deck transports for Chocolate Doom's netcode (net_module_t contract,
// net_defs.h) plus the launcher spec parser. Modelled on net_sdl.c: the UDP
// module keeps a table of ip:port address handles so the netcode can compare
// addresses by pointer; the USB module has exactly one peer. Both are
// datagram transports with no delivery guarantee — net_common.c's own
// ack/resend layer runs on top, exactly as it does over real UDP.
//
// Host functions come from the firmware (elf_host.h, FW_API 13).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "doomtype.h"
#include "i_system.h"
#include "m_misc.h"
#include "net_defs.h"
#include "net_io.h"
#include "net_packet.h"
#include "net_tdeck.h"

// ── Host exports ─────────────────────────────────────────────────────────────

extern int      host_net_status(void);
extern unsigned host_net_resolve(const char *name);
extern int      host_udp_open(int port);
extern int      host_udp_send(int sock, unsigned ip, int port, const void *data, int len);
extern int      host_udp_recv(int sock, unsigned *ip, int *port, void *buf, int max);
extern void     host_net_close(int sock);
extern int      host_link_status(void);
extern int      host_link_dgram_open(void);
extern void     host_link_dgram_close(void);
extern int      host_link_dgram_send(const void *data, int len);
extern int      host_link_dgram_recv(void *buf, int max);

#define DEFAULT_PORT   2342
#define RECV_MAX       1500

tdeck_netopts_t tdeck_netopts;

// ── Launcher spec ────────────────────────────────────────────────────────────

static int SpecInt(const char *tok, const char *key, int *out)
{
    size_t klen = strlen(key);

    if (strncmp(tok, key, klen) == 0 && tok[klen] == '=')
    {
        *out = atoi(tok + klen + 1);
        return 1;
    }

    return 0;
}

void TDeck_NetParseSpec(const char *spec)
{
    char buf[256];
    char *tok, *next;

    memset(&tdeck_netopts, 0, sizeof(tdeck_netopts));
    tdeck_netopts.players = 2;

    if (spec == NULL)
        return;

    M_StringCopy(buf, spec, sizeof(buf));

    for (tok = buf; tok != NULL && *tok != '\0'; tok = next)
    {
        next = strchr(tok, ',');
        if (next != NULL)
        {
            *next = '\0';
            ++next;
        }

        if (strcmp(tok, "host") == 0)
            tdeck_netopts.mode = TDN_MODE_HOST;
        else if (strcmp(tok, "join=usb") == 0)
            tdeck_netopts.mode = TDN_MODE_JOIN_USB;
        else if (strcmp(tok, "join=lan") == 0)
            tdeck_netopts.mode = TDN_MODE_JOIN_LAN;
        else if (strncmp(tok, "join=", 5) == 0)
        {
            tdeck_netopts.mode = TDN_MODE_JOIN_ADDR;
            M_StringCopy(tdeck_netopts.addr, tok + 5, sizeof(tdeck_netopts.addr));
        }
        else if (SpecInt(tok, "players", &tdeck_netopts.players)
              || SpecInt(tok, "dm", &tdeck_netopts.deathmatch)
              || SpecInt(tok, "skill", &tdeck_netopts.skill)
              || SpecInt(tok, "ep", &tdeck_netopts.episode)
              || SpecInt(tok, "map", &tdeck_netopts.map)
              || SpecInt(tok, "nomon", &tdeck_netopts.nomonsters)
              || SpecInt(tok, "respawn", &tdeck_netopts.respawn)
              || SpecInt(tok, "fast", &tdeck_netopts.fast)
              || SpecInt(tok, "timer", &tdeck_netopts.timer))
        {
            // handled
        }
        else
        {
            printf("netgame: unknown option '%s'\n", tok);
        }
    }

    if (tdeck_netopts.players < 2)
        tdeck_netopts.players = 2;
    if (tdeck_netopts.players > NET_MAXPLAYERS)
        tdeck_netopts.players = NET_MAXPLAYERS;
}

// ── UDP transport ────────────────────────────────────────────────────────────

typedef struct
{
    net_addr_t net_addr;
    unsigned int ip;        // host order
    int port;
} udp_addr_t;

static udp_addr_t **udp_table = NULL;
static int udp_table_size = 0;
static int udp_sock = -1;
static int udp_port = DEFAULT_PORT;
static byte udp_recvbuf[RECV_MAX];

static net_addr_t *UDP_FindAddress(unsigned int ip, int port)
{
    udp_addr_t *entry;
    int empty = -1;
    int i;

    for (i = 0; i < udp_table_size; ++i)
    {
        if (udp_table[i] != NULL
         && udp_table[i]->ip == ip && udp_table[i]->port == port)
        {
            return &udp_table[i]->net_addr;
        }

        if (empty < 0 && udp_table[i] == NULL)
            empty = i;
    }

    if (empty < 0)
    {
        // Grow the pointer table; entries themselves never move, so the
        // handles the netcode holds stay valid.
        int new_size = udp_table_size == 0 ? 16 : udp_table_size * 2;
        udp_addr_t **new_table = calloc(new_size, sizeof(udp_addr_t *));

        if (new_table == NULL)
            I_Error("UDP_FindAddress: out of memory");

        if (udp_table != NULL)
        {
            memcpy(new_table, udp_table, sizeof(udp_addr_t *) * udp_table_size);
            free(udp_table);
        }

        empty = udp_table_size;
        udp_table = new_table;
        udp_table_size = new_size;
    }

    entry = calloc(1, sizeof(udp_addr_t));
    if (entry == NULL)
        I_Error("UDP_FindAddress: out of memory");

    entry->ip = ip;
    entry->port = port;
    entry->net_addr.module = &net_udp_module;
    entry->net_addr.handle = entry;
    udp_table[empty] = entry;

    return &entry->net_addr;
}

static void UDP_FreeAddress(net_addr_t *addr)
{
    int i;

    for (i = 0; i < udp_table_size; ++i)
    {
        if (udp_table[i] != NULL && addr == &udp_table[i]->net_addr)
        {
            free(udp_table[i]);
            udp_table[i] = NULL;
            return;
        }
    }
}

// One socket serves both roles: the server binds the game port, a client
// takes any port. Whichever comes first wins; a later call is a no-op.
static boolean UDP_Open(int port)
{
    if (udp_sock >= 0)
        return true;

    if (!host_net_status())
    {
        printf("net_udp: WiFi is not connected\n");
        return false;
    }

    udp_sock = host_udp_open(port);

    if (udp_sock < 0)
    {
        printf("net_udp: cannot open a UDP socket on port %d\n", port);
        return false;
    }

    return true;
}

static boolean UDP_InitClient(void)
{
    return UDP_Open(0);
}

static boolean UDP_InitServer(void)
{
    return UDP_Open(udp_port);
}

static void UDP_SendPacket(net_addr_t *addr, net_packet_t *packet)
{
    unsigned int ip;
    int port;

    if (udp_sock < 0)
        return;

    if (addr == &net_broadcast_addr)
    {
        ip = 0xFFFFFFFFu;
        port = udp_port;
    }
    else
    {
        udp_addr_t *entry = addr->handle;
        ip = entry->ip;
        port = entry->port;
    }

    // Unreliable by contract: a failed send is a lost packet, and the
    // connection layer resends whatever mattered.
    host_udp_send(udp_sock, ip, port, packet->data, (int) packet->len);
}

static boolean UDP_RecvPacket(net_addr_t **addr, net_packet_t **packet)
{
    unsigned int ip;
    int port;
    int n;

    if (udp_sock < 0)
        return false;

    n = host_udp_recv(udp_sock, &ip, &port, udp_recvbuf, RECV_MAX);

    if (n <= 0)
        return false;

    *packet = NET_NewPacket(n);
    memcpy((*packet)->data, udp_recvbuf, n);
    (*packet)->len = n;

    *addr = UDP_FindAddress(ip, port);

    return true;
}

static void UDP_AddrToString(net_addr_t *addr, char *buffer, int buffer_len)
{
    udp_addr_t *entry = addr->handle;

    M_snprintf(buffer, buffer_len, "%u.%u.%u.%u",
               (entry->ip >> 24) & 0xff, (entry->ip >> 16) & 0xff,
               (entry->ip >> 8) & 0xff, entry->ip & 0xff);

    if (entry->port != DEFAULT_PORT)
    {
        char portbuf[10];
        M_snprintf(portbuf, sizeof(portbuf), ":%i", entry->port);
        M_StringConcat(buffer, portbuf, buffer_len);
    }
}

static net_addr_t *UDP_ResolveAddress(char *address)
{
    char host[64];
    char *colon;
    int port = udp_port;
    unsigned int ip;

    M_StringCopy(host, address, sizeof(host));
    colon = strchr(host, ':');

    if (colon != NULL)
    {
        *colon = '\0';
        port = atoi(colon + 1);
    }

    ip = host_net_resolve(host);

    if (ip == 0)
        return NULL;

    return UDP_FindAddress(ip, port);
}

net_module_t net_udp_module =
{
    UDP_InitClient,
    UDP_InitServer,
    UDP_SendPacket,
    UDP_RecvPacket,
    UDP_AddrToString,
    UDP_FreeAddress,
    UDP_ResolveAddress,
};

// ── USB cable transport ──────────────────────────────────────────────────────

static net_addr_t usb_peer;
static boolean usb_open = false;
static byte usb_recvbuf[RECV_MAX];

net_addr_t *NET_USB_PeerAddress(void)
{
    usb_peer.module = &net_usb_module;
    usb_peer.handle = NULL;
    return &usb_peer;
}

static boolean USB_Init(void)
{
    if (usb_open)
        return true;

    if ((host_link_status() & 3) < 1)
    {
        printf("net_usb: no peer link session\n");
        return false;
    }

    if (!host_link_dgram_open())
    {
        printf("net_usb: dgram service unavailable\n");
        return false;
    }

    usb_open = true;
    return true;
}

static void USB_SendPacket(net_addr_t *addr, net_packet_t *packet)
{
    if (!usb_open)
        return;

    // Broadcast and the peer are the same thing on a cable.
    host_link_dgram_send(packet->data, (int) packet->len);
}

static boolean USB_RecvPacket(net_addr_t **addr, net_packet_t **packet)
{
    int n;

    if (!usb_open)
        return false;

    n = host_link_dgram_recv(usb_recvbuf, RECV_MAX);

    if (n <= 0)
        return false;

    if (n > RECV_MAX)
        n = RECV_MAX;

    *packet = NET_NewPacket(n);
    memcpy((*packet)->data, usb_recvbuf, n);
    (*packet)->len = n;

    *addr = NET_USB_PeerAddress();

    return true;
}

static void USB_AddrToString(net_addr_t *addr, char *buffer, int buffer_len)
{
    M_StringCopy(buffer, "usb", buffer_len);
}

static void USB_FreeAddress(net_addr_t *addr)
{
    // The peer address is static; the netcode frees it after every
    // unmatched packet and at disconnect, which must be harmless.
}

static net_addr_t *USB_ResolveAddress(char *address)
{
    if (strcmp(address, "usb") == 0)
        return NET_USB_PeerAddress();

    return NULL;
}

net_module_t net_usb_module =
{
    USB_Init,
    USB_Init,
    USB_SendPacket,
    USB_RecvPacket,
    USB_AddrToString,
    USB_FreeAddress,
    USB_ResolveAddress,
};

// ELF-module network exports: WiFi state + lwIP UDP/TCP sockets. The
// module-facing contract is documented in elf_host.h next to the other
// host_exports; elf_host.cpp lists them in its table. Kept apart from
// elf_host.cpp so lwIP's socket headers (and their POSIX-name macros) stay
// out of that file. Socket idioms follow the Arduino WiFi library's own
// (WiFiUdp.cpp / WiFiClient.cpp / WiFiServer.cpp on this framework).
//
// Every socket a module opens is tracked here (NET_MAX_SOCKS) so
// net_bridge_close_all() can reclaim it after the run whatever way the
// module ended. The first tracked socket switches WiFi modem sleep OFF for
// the run: the Arduino default (WIFI_PS_MIN_MODEM) lets the radio doze
// between beacons, which adds up to a beacon interval of receive latency per
// packet — fine for a download, fatal for a lockstep game. The mode in force
// before is restored at the last close.

#include "net_bridge.h"
#include "elf_host.h"
#include "meshpunk_sync.h"   // SLog

#include <WiFi.h>
#include <esp_wifi.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <lwip/inet.h>
#include <errno.h>
#include <string.h>

#define NET_MAX_SOCKS 4

static int            s_socks[NET_MAX_SOCKS] = { -1, -1, -1, -1 };
static int            s_nsocks = 0;
static wifi_ps_type_t s_ps_saved = WIFI_PS_MIN_MODEM;

static bool tracked(int fd) {
    if (fd < 0) return false;
    for (int i = 0; i < NET_MAX_SOCKS; i++)
        if (s_socks[i] == fd) return true;
    return false;
}

// Adopt a freshly opened socket; a full table closes it and reports -1.
static int track(int fd) {
    if (fd < 0) return -1;
    for (int i = 0; i < NET_MAX_SOCKS; i++) {
        if (s_socks[i] < 0) {
            if (s_nsocks == 0) {
                if (esp_wifi_get_ps(&s_ps_saved) != ESP_OK) s_ps_saved = WIFI_PS_MIN_MODEM;
                esp_wifi_set_ps(WIFI_PS_NONE);
            }
            s_socks[i] = fd;
            s_nsocks++;
            return fd;
        }
    }
    close(fd);
    SLog.println("[net] module socket table full");
    return -1;
}

static void untrack(int fd) {
    for (int i = 0; i < NET_MAX_SOCKS; i++) {
        if (s_socks[i] == fd) {
            s_socks[i] = -1;
            s_nsocks--;
            if (s_nsocks == 0) esp_wifi_set_ps(s_ps_saved);
            return;
        }
    }
}

void net_bridge_close_all() {
    for (int i = 0; i < NET_MAX_SOCKS; i++) {
        if (s_socks[i] >= 0) {
            SLog.printf("[net] closing socket %d left open by the module\n", s_socks[i]);
            close(s_socks[i]);
            s_socks[i] = -1;
        }
    }
    if (s_nsocks) {
        s_nsocks = 0;
        esp_wifi_set_ps(s_ps_saved);
    }
}

static void set_nonblock(int fd) {
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}

static void fill_addr(struct sockaddr_in* a, unsigned ip, int port) {
    memset(a, 0, sizeof(*a));
    a->sin_family = AF_INET;
    a->sin_port = htons((uint16_t)port);
    a->sin_addr.s_addr = htonl(ip);
}

static bool would_block() {
    return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINPROGRESS;
}

extern "C" {

int host_net_status(void) {
    return WiFi.status() == WL_CONNECTED ? 1 : 0;
}

unsigned host_net_local_ip(void) {
    if (WiFi.status() != WL_CONNECTED) return 0;
    // IPAddress's uint32 is the address in memory order = network order.
    return ntohl((uint32_t)WiFi.localIP());
}

unsigned host_net_resolve(const char* name) {
    if (!name || !*name) return 0;
    uint32_t a = ipaddr_addr(name);            // dotted quad: no lookup
    if (a != IPADDR_NONE) return ntohl(a);
    if (WiFi.status() != WL_CONNECTED) return 0;
    struct hostent* h = lwip_gethostbyname(name);
    if (!h || !h->h_addr_list || !h->h_addr_list[0]) return 0;
    struct in_addr in;
    memcpy(&in, h->h_addr_list[0], sizeof(in));
    return ntohl(in.s_addr);
}

int host_udp_open(int port) {
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a;
    fill_addr(&a, INADDR_ANY, port < 0 ? 0 : port);
    if (bind(fd, (struct sockaddr*)&a, sizeof(a)) < 0) {
        close(fd);
        return -1;
    }
    set_nonblock(fd);
    return track(fd);
}

int host_udp_send(int sock, unsigned ip, int port, const void* data, int len) {
    if (!tracked(sock) || !data || len < 0) return -1;
    struct sockaddr_in a;
    fill_addr(&a, ip, port);
    int n = sendto(sock, data, (size_t)len, 0, (struct sockaddr*)&a, sizeof(a));
    if (n < 0) return would_block() ? 0 : -1;
    return n;
}

int host_udp_recv(int sock, unsigned* ip, int* port, void* buf, int max) {
    if (!tracked(sock) || !buf || max <= 0) return -1;
    struct sockaddr_in a;
    socklen_t alen = sizeof(a);
    int n = recvfrom(sock, buf, (size_t)max, MSG_DONTWAIT,
                     (struct sockaddr*)&a, &alen);
    if (n < 0) return would_block() ? 0 : -1;
    if (ip)   *ip   = ntohl(a.sin_addr.s_addr);
    if (port) *port = ntohs(a.sin_port);
    return n;
}

int host_tcp_connect(unsigned ip, int port, int timeout_ms) {
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return -1;
    set_nonblock(fd);
    struct sockaddr_in a;
    fill_addr(&a, ip, port);
    int r = lwip_connect(fd, (struct sockaddr*)&a, sizeof(a));
    if (r < 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }
    if (r < 0) {
        // In progress: writable within timeout_ms means the handshake ended;
        // SO_ERROR says how.
        if (timeout_ms < 0) timeout_ms = 0;
        fd_set wf;
        FD_ZERO(&wf);
        FD_SET(fd, &wf);
        struct timeval tv;
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        r = select(fd + 1, NULL, &wf, NULL, &tv);
        int err = 0;
        socklen_t elen = sizeof(err);
        if (r <= 0 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) < 0 || err != 0) {
            close(fd);
            return -1;
        }
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return track(fd);
}

int host_tcp_listen(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a;
    fill_addr(&a, INADDR_ANY, port);
    if (bind(fd, (struct sockaddr*)&a, sizeof(a)) < 0 || listen(fd, 2) < 0) {
        close(fd);
        return -1;
    }
    set_nonblock(fd);
    return track(fd);
}

int host_tcp_accept(int lsock, unsigned* ip, int* port) {
    if (!tracked(lsock)) return -1;
    struct sockaddr_in a;
    socklen_t alen = sizeof(a);
    int fd = lwip_accept(lsock, (struct sockaddr*)&a, &alen);
    if (fd < 0) return -1;                      // nothing pending (or error)
    set_nonblock(fd);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    if (ip)   *ip   = ntohl(a.sin_addr.s_addr);
    if (port) *port = ntohs(a.sin_port);
    return track(fd);
}

int host_tcp_send(int sock, const void* data, int len) {
    if (!tracked(sock) || !data || len < 0) return -1;
    int n = send(sock, data, (size_t)len, MSG_DONTWAIT);
    if (n < 0) return would_block() ? 0 : -1;
    return n;
}

int host_tcp_recv(int sock, void* buf, int max) {
    if (!tracked(sock) || !buf || max <= 0) return -1;
    int n = recv(sock, buf, (size_t)max, MSG_DONTWAIT);
    if (n == 0) return -1;                      // orderly close by the peer
    if (n < 0) return would_block() ? 0 : -1;
    return n;
}

void host_net_close(int sock) {
    if (!tracked(sock)) return;
    close(sock);
    untrack(sock);
}

} // extern "C"

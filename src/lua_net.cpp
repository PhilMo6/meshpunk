// lua_net.cpp — Lua client TCP sockets, plain or TLS.
//
// Bindings (lua_net_register_lua):
//   _tcp_open(host, port, [opts]) -> sock | nil, err
//       opts: tls (bool, default false)
//             verify ("ca" = IDF certificate bundle | "pin" = certificate
//                     SHA-256; default "ca"; TLS only)
//             pin (64 hex chars, "pin" mode; absent = capture: the handshake
//                  completes into state "untrusted" so info() can show the
//                  certificate, and send is refused)
//             timeout_ms (connect timeout, default 15000)
//   sock:state() -> "connecting" | "open" | "untrusted" | "closed"[, reason]
//   sock:send(str) -> #str | nil, err     -- all-or-nothing into the TX buffer
//   sock:recv([max]) -> str | nil, reason -- "" = nothing pending; nil once
//                                            closed and the RX buffer drained
//   sock:info() -> { host, port, tls, bytes_in, bytes_out, cert_sha256,
//                    cert_subject, cert_issuer, cert_valid_to } | nil
//   sock:close()                           -- also run by __gc
//
// 4 sockets, serviced by a Core-1 worker task (16KB stack, priority 1)
// created at the first open and self-deleted when no socket remains, so its
// internal-SRAM stack is only spent while sockets exist. No worker step
// blocks: DNS uses lwIP's callback API under an 8s deadline, connects poll a
// non-blocking fd, and the TLS handshake runs one mbedtls_ssl_handshake call
// per pass (WANT_READ/WANT_WRITE = call again with the same arguments —
// mbedTLS's documented retry contract; the framework's ssl_client.cpp loops
// the identical call).
//
// Buffers (16KB RX + 8KB TX per socket) and the TLS contexts live in PSRAM.
// The rings are single-producer/single-consumer (RX: worker writes, Lua
// reads; TX: Lua writes, worker reads): counters change only under s_mutex,
// each data region is touched only by its one producer/consumer, so no Lua
// call ever blocks on socket work.
//
// Close reasons: "dns failed", "connect timeout", "refused",
// "connect failed", "tls: <detail>", "certificate changed", "peer closed",
// "reset", "wifi down", "standby", "lua teardown", "closed".

#include "lua_net.h"
#include "meshpunk_sync.h"   // SLog

#include <Arduino.h>
#include <WiFi.h>
extern "C" {
#include <lua.h>
#include <lauxlib.h>
}
#include <lwip/sockets.h>
#include <lwip/inet.h>
#include <lwip/dns.h>
#include <errno.h>
#include <string.h>

#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/net_sockets.h"   // mbedtls_net_send/recv (fd-pointer bio)
#include "mbedtls/sha256.h"
#include "mbedtls/x509_crt.h"
#include "esp_err.h"
#include "esp_heap_caps.h"

// The IDF certificate-bundle attach from the prebuilt libmbedtls.a (the
// bundle blob is embedded in that archive). Declared directly instead of
// including esp_crt_bundle.h: the WiFiClientSecure library ships a header of
// the same name whose functions are arduino_-prefixed copies with a separate
// bundle table.
extern "C" esp_err_t esp_crt_bundle_attach(void *conf);

#define LNET_SLOTS          4
#define LNET_RX_SIZE        (16 * 1024)
#define LNET_TX_SIZE        (8 * 1024)
#define LNET_HOST_MAX       128
#define LNET_REASON_MAX     64
#define LNET_DNS_TIMEOUT_MS 8000
#define LNET_HS_TIMEOUT_MS  20000

enum LNetState : uint8_t {
  NS_FREE = 0,
  NS_RESOLVE,      // -> "connecting"
  NS_CONNECT,      // -> "connecting"
  NS_HANDSHAKE,    // -> "connecting"
  NS_OPEN,         // -> "open"
  NS_UNTRUSTED,    // -> "untrusted" (pin capture: handshake done, no pin)
  NS_CLOSED,       // -> "closed" (RX kept for draining until the handle detaches)
};

struct LNetTls {
  mbedtls_entropy_context  entropy;
  mbedtls_ctr_drbg_context drbg;
  mbedtls_ssl_config       conf;
  mbedtls_ssl_context      ssl;
};

struct NetSlot {
  uint8_t  state;
  uint32_t gen;              // bumped per open; DNS callbacks and Lua handles carry it
  bool     want_close;       // Lua/close_all asked; the worker performs the close
  bool     handle_detached;  // Lua handle gone: free RX at close, slot -> FREE
  char     pending_reason[LNET_REASON_MAX]; // reason for a want_close
  char     reason[LNET_REASON_MAX];         // set once at close, first writer wins
  char     host[LNET_HOST_MAX];
  uint16_t port;
  bool     tls;
  bool     pin_mode;
  bool     have_pin;
  uint8_t  pin[32];
  uint32_t timeout_ms;
  uint32_t deadline;         // millis deadline of the current phase

  bool     dns_requested;
  volatile bool     dns_done;
  volatile bool     dns_ok;
  volatile uint32_t dns_ip;  // network byte order

  int      fd;
  LNetTls *tc;               // PSRAM; TLS runs only
  bool     tls_ready;        // mbedtls_ssl_setup done (close_notify is valid)

  uint8_t *rx; uint32_t rx_start, rx_used;
  uint8_t *tx; uint32_t tx_start, tx_used;
  uint32_t tx_inflight;      // chunk length latched across a WANT_WRITE retry

  bool     have_cert;
  char     cert_subject[96], cert_issuer[96], cert_valid_to[24], cert_sha[65];
  uint8_t  cert_sha_raw[32];

  uint32_t bytes_in, bytes_out;
};

struct LNetHandle {
  uint8_t  slot;
  uint32_t gen;
  bool     detached;
  char     freason[LNET_REASON_MAX]; // state/recv answer once detached
};

static NetSlot           s_slots[LNET_SLOTS];
static SemaphoreHandle_t s_mutex = nullptr;
static bool              s_task_running = false;

// ── helpers ─────────────────────────────────────────────────────────────────

static void lock()   { xSemaphoreTake(s_mutex, portMAX_DELAY); }
static void unlock() { xSemaphoreGive(s_mutex); }

static void set_reason(NetSlot *s, const char *r) {
  if (s->reason[0] || !r) return;   // first writer wins
  strlcpy(s->reason, r, sizeof(s->reason));
}

static bool slot_active(const NetSlot *s) {
  return s->state >= NS_RESOLVE && s->state <= NS_UNTRUSTED;
}

// DNS callback (lwIP tcpip task). arg packs slot (high bits) + the low 20
// bits of the request's gen, so a late callback for an earlier open no-ops.
static void lnet_dns_cb(const char *name, const ip_addr_t *ipaddr, void *arg) {
  (void)name;
  uint32_t v = (uint32_t)(uintptr_t)arg;
  int i = (int)(v >> 20);
  uint32_t genbits = v & 0xFFFFF;
  if (i < 0 || i >= LNET_SLOTS) return;
  lock();
  NetSlot *s = &s_slots[i];
  if (s->state == NS_RESOLVE && (s->gen & 0xFFFFF) == genbits) {
    s->dns_ok = (ipaddr != nullptr && IP_IS_V4(ipaddr));
    s->dns_ip = s->dns_ok ? ip_2_ip4(ipaddr)->addr : 0;
    s->dns_done = true;
  }
  unlock();
}

// ── worker: close path ──────────────────────────────────────────────────────

// Frees everything but (when the handle is still attached) the RX buffer, so
// recv can drain what arrived before the close.
static void lnet_close_slot(int i, NetSlot *s, const char *why) {
  if (s->tc) {
    if (s->tls_ready) mbedtls_ssl_close_notify(&s->tc->ssl);
    mbedtls_ssl_free(&s->tc->ssl);
    mbedtls_ssl_config_free(&s->tc->conf);
    mbedtls_ctr_drbg_free(&s->tc->drbg);
    mbedtls_entropy_free(&s->tc->entropy);
    heap_caps_free(s->tc);
    s->tc = nullptr;
    s->tls_ready = false;
  }
  if (s->fd >= 0) { close(s->fd); s->fd = -1; }
  if (s->tx) { heap_caps_free(s->tx); s->tx = nullptr; }
  lock();
  set_reason(s, s->pending_reason[0] ? s->pending_reason : why);
  s->want_close = false;
  if (s->handle_detached) {
    if (s->rx) { heap_caps_free(s->rx); s->rx = nullptr; }
    s->state = NS_FREE;
  } else {
    s->state = NS_CLOSED;
  }
  unlock();
  SLog.printf("[NET] %d %s:%u closed (%s)\n", i, s->host, s->port, s->reason);
}

// ── worker: per-slot step ───────────────────────────────────────────────────

static bool lnet_step(int i, NetSlot *s, bool wifi_up) {
  // Snapshot under the mutex: acquiring it here pairs with _tcp_open's
  // release, so a slot never appears active before its fields are visible.
  lock();
  uint8_t st = s->state;
  bool want_close = s->want_close;
  unlock();
  if (st < NS_RESOLVE || st > NS_UNTRUSTED) return false;

  if (want_close) { lnet_close_slot(i, s, "closed"); return false; }
  if (!wifi_up)   { lnet_close_slot(i, s, "wifi down"); return false; }

  uint32_t now = millis();

  switch (st) {

  case NS_RESOLVE: {
    if (!s->dns_requested) {
      s->dns_requested = true;
      s->deadline = now + LNET_DNS_TIMEOUT_MS;
      ip_addr_t addr;
      void *arg = (void *)(uintptr_t)(((uint32_t)i << 20) | (s->gen & 0xFFFFF));
      err_t e = dns_gethostbyname_addrtype(s->host, &addr, lnet_dns_cb, arg,
                                           LWIP_DNS_ADDRTYPE_IPV4);
      if (e == ERR_OK) {           // cache hit: no callback comes
        s->dns_ok = IP_IS_V4(&addr);
        s->dns_ip = s->dns_ok ? ip_2_ip4(&addr)->addr : 0;
        s->dns_done = true;
      } else if (e != ERR_INPROGRESS) {
        lnet_close_slot(i, s, "dns failed");
        return false;
      }
    }
    if (s->dns_done) {
      if (!s->dns_ok) { lnet_close_slot(i, s, "dns failed"); return false; }
      int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
      if (fd < 0) { lnet_close_slot(i, s, "connect failed"); return false; }
      fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
      struct sockaddr_in a;
      memset(&a, 0, sizeof(a));
      a.sin_family = AF_INET;
      a.sin_port = htons(s->port);
      a.sin_addr.s_addr = s->dns_ip;
      s->fd = fd;
      int r = lwip_connect(fd, (struct sockaddr *)&a, sizeof(a));
      if (r < 0 && errno != EINPROGRESS) {
        lnet_close_slot(i, s, errno == ECONNREFUSED ? "refused" : "connect failed");
        return false;
      }
      s->deadline = now + s->timeout_ms;
      lock(); s->state = NS_CONNECT; unlock();
    } else if ((int32_t)(now - s->deadline) >= 0) {
      lnet_close_slot(i, s, "dns failed");
      return false;
    }
    return true;
  }

  case NS_CONNECT: {
    fd_set wf;
    FD_ZERO(&wf);
    FD_SET(s->fd, &wf);
    struct timeval tv = {0, 0};
    int r = select(s->fd + 1, nullptr, &wf, nullptr, &tv);
    if (r > 0) {
      int err = 0;
      socklen_t elen = sizeof(err);
      if (getsockopt(s->fd, SOL_SOCKET, SO_ERROR, &err, &elen) < 0 || err != 0) {
        lnet_close_slot(i, s, err == ECONNREFUSED ? "refused" : "connect failed");
        return false;
      }
      int one = 1;
      setsockopt(s->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
      if (s->tls) {
        s->deadline = now + LNET_HS_TIMEOUT_MS;
        lock(); s->state = NS_HANDSHAKE; unlock();
      } else {
        lock(); s->state = NS_OPEN; unlock();
        SLog.printf("[NET] %d %s:%u open\n", i, s->host, s->port);
      }
    } else if ((int32_t)(now - s->deadline) >= 0) {
      lnet_close_slot(i, s, "connect timeout");
      return false;
    }
    return true;
  }

  case NS_HANDSHAKE: {
    LNetTls *tc = s->tc;
    if (!s->tls_ready) {
      mbedtls_entropy_init(&tc->entropy);
      mbedtls_ctr_drbg_init(&tc->drbg);
      mbedtls_ssl_config_init(&tc->conf);
      mbedtls_ssl_init(&tc->ssl);
      int ret = mbedtls_ctr_drbg_seed(&tc->drbg, mbedtls_entropy_func,
                                      &tc->entropy, nullptr, 0);
      if (ret == 0)
        ret = mbedtls_ssl_config_defaults(&tc->conf, MBEDTLS_SSL_IS_CLIENT,
                                          MBEDTLS_SSL_TRANSPORT_STREAM,
                                          MBEDTLS_SSL_PRESET_DEFAULT);
      if (ret == 0) {
        if (s->pin_mode) {
          // Chain verification off; trust is the SHA-256 pin check below.
          mbedtls_ssl_conf_authmode(&tc->conf, MBEDTLS_SSL_VERIFY_NONE);
        } else {
          mbedtls_ssl_conf_authmode(&tc->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
          if (esp_crt_bundle_attach(&tc->conf) != ESP_OK) ret = -1;
        }
      }
      if (ret == 0) {
        mbedtls_ssl_conf_rng(&tc->conf, mbedtls_ctr_drbg_random, &tc->drbg);
        ret = mbedtls_ssl_set_hostname(&tc->ssl, s->host);
      }
      if (ret == 0) ret = mbedtls_ssl_setup(&tc->ssl, &tc->conf);
      if (ret != 0) {
        char why[LNET_REASON_MAX];
        snprintf(why, sizeof(why), "tls: setup -0x%04x", (unsigned)-ret);
        lnet_close_slot(i, s, why);
        return false;
      }
      // The bio context is a pointer to the fd: mbedtls_net_context's first
      // member is the int fd, the layout ssl_client.cpp relies on too.
      mbedtls_ssl_set_bio(&tc->ssl, &s->fd, mbedtls_net_send, mbedtls_net_recv,
                          nullptr);
      s->tls_ready = true;
    }

    int ret = mbedtls_ssl_handshake(&tc->ssl);
    if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
      if ((int32_t)(now - s->deadline) >= 0) {
        lnet_close_slot(i, s, "tls: handshake timeout");
        return false;
      }
      return true;
    }
    if (ret != 0) {
      // Certificate rejections surface as -0x2700 (VERIFY_FAILED) or as the
      // bundle callback's -0x3000 (X509_FATAL_ERROR); either way the verify
      // flags carry the human reason. 0 = no cert problem, ~0 = verification
      // never ran — both fall back to the raw code.
      char why[LNET_REASON_MAX];
      uint32_t flags = mbedtls_ssl_get_verify_result(&tc->ssl);
      if (flags != 0 && flags != 0xFFFFFFFFu) {
        char vbuf[128] = {0};
        mbedtls_x509_crt_verify_info(vbuf, sizeof(vbuf), "", flags);
        char *nl = strchr(vbuf, '\n');
        if (nl) *nl = '\0';
        snprintf(why, sizeof(why), "tls: %s", vbuf);
      } else {
        snprintf(why, sizeof(why), "tls: -0x%04x", (unsigned)-ret);
      }
      lnet_close_slot(i, s, why);
      return false;
    }

    // Handshake complete: record the peer certificate.
    const mbedtls_x509_crt *crt = mbedtls_ssl_get_peer_cert(&tc->ssl);
    if (crt) {
      mbedtls_x509_dn_gets(s->cert_subject, sizeof(s->cert_subject), &crt->subject);
      mbedtls_x509_dn_gets(s->cert_issuer, sizeof(s->cert_issuer), &crt->issuer);
      snprintf(s->cert_valid_to, sizeof(s->cert_valid_to), "%04d-%02d-%02d",
               crt->valid_to.year, crt->valid_to.mon, crt->valid_to.day);
      mbedtls_sha256_context sha;
      mbedtls_sha256_init(&sha);
      mbedtls_sha256_starts(&sha, false);
      mbedtls_sha256_update(&sha, crt->raw.p, crt->raw.len);
      mbedtls_sha256_finish(&sha, s->cert_sha_raw);
      mbedtls_sha256_free(&sha);
      for (int b = 0; b < 32; b++)
        snprintf(&s->cert_sha[b * 2], 3, "%02x", s->cert_sha_raw[b]);
      lock();               // publish: info() reads have_cert under the mutex
      s->have_cert = true;
      unlock();
    }
    if (s->pin_mode) {
      if (!s->have_pin) {
        lock(); s->state = NS_UNTRUSTED; unlock();
        SLog.printf("[NET] %d %s:%u untrusted (pin capture)\n", i, s->host, s->port);
        return true;
      }
      if (!s->have_cert || memcmp(s->pin, s->cert_sha_raw, 32) != 0) {
        lnet_close_slot(i, s, "certificate changed");
        return false;
      }
    }
    lock(); s->state = NS_OPEN; unlock();
    SLog.printf("[NET] %d %s:%u open (tls)\n", i, s->host, s->port);
    return true;
  }

  case NS_OPEN: {
    // TX. Counters are snapshotted under the mutex (which also makes the
    // producer's data writes visible); the tail region itself is stable —
    // Lua only appends at the head.
    for (;;) {
      lock();
      uint32_t used = s->tx_used;
      uint32_t start = s->tx_start;
      unlock();
      if (used == 0) break;
      uint32_t contig = used;
      uint32_t wrap = LNET_TX_SIZE - start;
      if (contig > wrap) contig = wrap;
      uint32_t chunk = s->tx_inflight ? s->tx_inflight : contig;
      int n;
      if (s->tls) {
        n = mbedtls_ssl_write(&s->tc->ssl, s->tx + start, chunk);
        if (n == MBEDTLS_ERR_SSL_WANT_WRITE || n == MBEDTLS_ERR_SSL_WANT_READ) {
          s->tx_inflight = chunk;   // retry next pass with identical args
          break;
        }
      } else {
        n = send(s->fd, s->tx + start, chunk, MSG_DONTWAIT);
        if (n < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) break;
      }
      if (n <= 0) { lnet_close_slot(i, s, "reset"); return false; }
      s->tx_inflight = 0;
      s->bytes_out += n;
      lock();
      s->tx_start = (s->tx_start + n) % LNET_TX_SIZE;
      s->tx_used -= n;
      unlock();
      if ((uint32_t)n < chunk) break;   // partial: socket is full for now
    }

    // RX, bounded per pass. Same snapshot rule; the write region is stable
    // (recv advances start and used together, so the write position and the
    // free space only move in the worker's favor between snapshots).
    for (int loops = 0; loops < 8; loops++) {
      lock();
      uint32_t rused = s->rx_used;
      uint32_t rstart = s->rx_start;
      unlock();
      uint32_t freeb = LNET_RX_SIZE - rused;
      if (freeb == 0) break;            // full buffer = TCP backpressure
      uint32_t wpos = (rstart + rused) % LNET_RX_SIZE;
      uint32_t contig = LNET_RX_SIZE - wpos;
      if (contig > freeb) contig = freeb;
      int n;
      if (s->tls) {
        n = mbedtls_ssl_read(&s->tc->ssl, s->rx + wpos, contig);
        if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) break;
        if (n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || n == 0) {
          lnet_close_slot(i, s, "peer closed");
          return false;
        }
      } else {
        n = recv(s->fd, s->rx + wpos, contig, MSG_DONTWAIT);
        if (n < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) break;
        if (n == 0) { lnet_close_slot(i, s, "peer closed"); return false; }
      }
      if (n < 0) { lnet_close_slot(i, s, "reset"); return false; }
      s->bytes_in += n;
      lock();
      s->rx_used += n;
      unlock();
    }
    return true;
  }

  case NS_UNTRUSTED:
    // Held for the trust flow: no reads, no writes; only close ends it.
    return true;

  default:
    return false;
  }
}

// ── worker task ─────────────────────────────────────────────────────────────

static void lnet_task(void *) {
  SLog.printf("[NET] worker start core=%d\n", xPortGetCoreID());
  for (;;) {
    bool wifi_up = (WiFi.status() == WL_CONNECTED);
    bool any = false;
    for (int i = 0; i < LNET_SLOTS; i++)
      any |= lnet_step(i, &s_slots[i], wifi_up);
    if (!any) {
      lock();
      // Re-check under the mutex: an open may have raced the sweep above.
      for (int i = 0; i < LNET_SLOTS; i++) any |= slot_active(&s_slots[i]);
      if (!any) {
        s_task_running = false;
        unlock();
        SLog.println("[NET] worker exit");
        vTaskDelete(NULL);   // frees the 16KB stack; never returns
      }
      unlock();
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ── Lua handle helpers ──────────────────────────────────────────────────────

static LNetHandle *check_handle(lua_State *L) {
  return (LNetHandle *)luaL_checkudata(L, 1, "lua_net_sock");
}

// Slot for an attached handle; nullptr once detached (or on a gen mismatch,
// which the detach-only reclaim rule makes unreachable in practice).
static NetSlot *handle_slot(LNetHandle *h) {
  if (h->detached) return nullptr;
  NetSlot *s = &s_slots[h->slot];
  if (s->gen != h->gen) return nullptr;
  return s;
}

// close() and __gc share this. Snapshots the reason into the handle, then
// hands the slot to the worker (or reclaims it inline when already closed —
// also the path used during Lua teardown, when the worker is gone).
static void handle_detach(LNetHandle *h) {
  if (h->detached) return;
  lock();
  NetSlot *s = &s_slots[h->slot];
  if (s->gen == h->gen && s->state != NS_FREE) {
    strlcpy(h->freason, s->reason[0] ? s->reason : "closed", sizeof(h->freason));
    s->handle_detached = true;
    if (s->state == NS_CLOSED) {
      if (s->rx) { heap_caps_free(s->rx); s->rx = nullptr; }
      s->state = NS_FREE;
    } else {
      if (!s->pending_reason[0]) strlcpy(s->pending_reason, "closed", sizeof(s->pending_reason));
      s->want_close = true;
    }
  } else if (!h->freason[0]) {
    strlcpy(h->freason, "closed", sizeof(h->freason));
  }
  h->detached = true;
  unlock();
}

// ── Lua methods ─────────────────────────────────────────────────────────────

static int l_sock_state(lua_State *L) {
  LNetHandle *h = check_handle(L);
  lock();
  NetSlot *s = handle_slot(h);
  uint8_t st = s ? s->state : NS_FREE;
  char reason[LNET_REASON_MAX];
  strlcpy(reason, s ? s->reason : h->freason, sizeof(reason));
  unlock();
  if (!s || st == NS_CLOSED || st == NS_FREE) {
    lua_pushstring(L, "closed");
    lua_pushstring(L, reason[0] ? reason : "closed");
    return 2;
  }
  if (st == NS_OPEN)      { lua_pushstring(L, "open");      return 1; }
  if (st == NS_UNTRUSTED) { lua_pushstring(L, "untrusted"); return 1; }
  lua_pushstring(L, "connecting");
  return 1;
}

static int l_sock_send(lua_State *L) {
  LNetHandle *h = check_handle(L);
  size_t len;
  const char *data = luaL_checklstring(L, 2, &len);
  if (len == 0) { lua_pushinteger(L, 0); return 1; }
  const char *err = nullptr;
  lock();
  NetSlot *s = handle_slot(h);
  if (!s || s->state == NS_CLOSED)      err = "closed";
  else if (s->state == NS_UNTRUSTED)    err = "untrusted";
  else if (s->state != NS_OPEN)         err = "not open";
  else if (len > (size_t)(LNET_TX_SIZE - s->tx_used)) err = "buffer full";
  else {
    uint32_t wpos = (s->tx_start + s->tx_used) % LNET_TX_SIZE;
    uint32_t contig = LNET_TX_SIZE - wpos;
    if (contig >= len) {
      memcpy(s->tx + wpos, data, len);
    } else {
      memcpy(s->tx + wpos, data, contig);
      memcpy(s->tx, data + contig, len - contig);
    }
    s->tx_used += len;
  }
  unlock();
  if (err) { lua_pushnil(L); lua_pushstring(L, err); return 2; }
  lua_pushinteger(L, (lua_Integer)len);
  return 1;
}

static int l_sock_recv(lua_State *L) {
  LNetHandle *h = check_handle(L);
  lua_Integer max = luaL_optinteger(L, 2, 4096);
  if (max < 1) max = 1;
  if (max > LNET_RX_SIZE) max = LNET_RX_SIZE;

  lock();
  NetSlot *s = handle_slot(h);
  uint32_t avail = s ? s->rx_used : 0;
  uint8_t st = s ? s->state : NS_FREE;
  char reason[LNET_REASON_MAX];
  strlcpy(reason, s ? s->reason : h->freason, sizeof(reason));
  unlock();

  if (avail == 0) {
    if (!s || st == NS_CLOSED || st == NS_FREE) {
      lua_pushnil(L);
      lua_pushstring(L, reason[0] ? reason : "closed");
      return 2;
    }
    lua_pushstring(L, "");
    return 1;
  }

  uint32_t want = avail < (uint32_t)max ? avail : (uint32_t)max;
  // The buffer alloc can longjmp, so it runs with nothing held; the worker
  // only ADDS to RX in between, so `want` bytes at the tail stay valid.
  luaL_Buffer b;
  char *p = luaL_buffinitsize(L, &b, want);
  lock();
  uint32_t contig = LNET_RX_SIZE - s->rx_start;
  if (contig >= want) {
    memcpy(p, s->rx + s->rx_start, want);
  } else {
    memcpy(p, s->rx + s->rx_start, contig);
    memcpy(p + contig, s->rx, want - contig);
  }
  s->rx_start = (s->rx_start + want) % LNET_RX_SIZE;
  s->rx_used -= want;
  unlock();
  luaL_pushresultsize(&b, want);
  return 1;
}

static int l_sock_info(lua_State *L) {
  LNetHandle *h = check_handle(L);
  lock();
  NetSlot *s = handle_slot(h);
  if (!s || s->state == NS_FREE) { unlock(); lua_pushnil(L); return 1; }
  char host[LNET_HOST_MAX], subj[96], iss[96], vto[24], sha[65];
  uint16_t port = s->port;
  bool tls = s->tls, have_cert = s->have_cert;
  uint32_t bin = s->bytes_in, bout = s->bytes_out;
  strlcpy(host, s->host, sizeof(host));
  strlcpy(subj, s->cert_subject, sizeof(subj));
  strlcpy(iss, s->cert_issuer, sizeof(iss));
  strlcpy(vto, s->cert_valid_to, sizeof(vto));
  strlcpy(sha, s->cert_sha, sizeof(sha));
  unlock();
  lua_newtable(L);
  lua_pushstring(L, host);          lua_setfield(L, -2, "host");
  lua_pushinteger(L, port);         lua_setfield(L, -2, "port");
  lua_pushboolean(L, tls);          lua_setfield(L, -2, "tls");
  lua_pushinteger(L, (lua_Integer)bin);  lua_setfield(L, -2, "bytes_in");
  lua_pushinteger(L, (lua_Integer)bout); lua_setfield(L, -2, "bytes_out");
  if (have_cert) {
    lua_pushstring(L, sha);  lua_setfield(L, -2, "cert_sha256");
    lua_pushstring(L, subj); lua_setfield(L, -2, "cert_subject");
    lua_pushstring(L, iss);  lua_setfield(L, -2, "cert_issuer");
    lua_pushstring(L, vto);  lua_setfield(L, -2, "cert_valid_to");
  }
  return 1;
}

static int l_sock_close(lua_State *L) {
  handle_detach(check_handle(L));
  return 0;
}

static int l_sock_gc(lua_State *L) {
  handle_detach(check_handle(L));
  return 0;
}

// ── _tcp_open ───────────────────────────────────────────────────────────────

static bool parse_pin(const char *hex, uint8_t out[32]) {
  if (!hex || strlen(hex) != 64) return false;
  for (int i = 0; i < 32; i++) {
    int v = 0;
    for (int k = 0; k < 2; k++) {
      char c = hex[i * 2 + k];
      v <<= 4;
      if (c >= '0' && c <= '9') v |= c - '0';
      else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
      else return false;
    }
    out[i] = (uint8_t)v;
  }
  return true;
}

static int l_tcp_open(lua_State *L) {
  const char *host = luaL_checkstring(L, 1);
  int port = (int)luaL_checkinteger(L, 2);

  bool tls = false, pin_mode = false, have_pin = false;
  uint8_t pin[32];
  uint32_t timeout_ms = 15000;

  if (!lua_isnoneornil(L, 3)) {
    luaL_checktype(L, 3, LUA_TTABLE);
    lua_getfield(L, 3, "tls");
    tls = lua_toboolean(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 3, "verify");
    if (lua_isstring(L, -1)) {
      const char *v = lua_tostring(L, -1);
      if (strcmp(v, "pin") == 0) pin_mode = true;
      else if (strcmp(v, "ca") != 0) {
        lua_pop(L, 1);
        lua_pushnil(L); lua_pushstring(L, "bad verify mode");
        return 2;
      }
    }
    lua_pop(L, 1);
    lua_getfield(L, 3, "pin");
    if (lua_isstring(L, -1)) {
      if (!parse_pin(lua_tostring(L, -1), pin)) {
        lua_pop(L, 1);
        lua_pushnil(L); lua_pushstring(L, "bad pin");
        return 2;
      }
      have_pin = true;
    }
    lua_pop(L, 1);
    lua_getfield(L, 3, "timeout_ms");
    if (lua_isnumber(L, -1)) {
      lua_Integer t = lua_tointeger(L, -1);
      if (t < 1000) t = 1000;
      if (t > 120000) t = 120000;
      timeout_ms = (uint32_t)t;
    }
    lua_pop(L, 1);
  }

  size_t hlen = strlen(host);
  if (hlen == 0 || hlen >= LNET_HOST_MAX || port < 1 || port > 65535) {
    lua_pushnil(L); lua_pushstring(L, "bad host or port");
    return 2;
  }
  if (WiFi.status() != WL_CONNECTED) {
    lua_pushnil(L); lua_pushstring(L, "wifi not connected");
    return 2;
  }

  // The userdata first: lua_newuserdata can longjmp, and after it nothing on
  // the success path allocates through Lua.
  LNetHandle *h = (LNetHandle *)lua_newuserdata(L, sizeof(LNetHandle));
  memset(h, 0, sizeof(*h));
  h->detached = true;   // armed only once a slot is bound
  luaL_getmetatable(L, "lua_net_sock");
  lua_setmetatable(L, -2);

  uint8_t *rx = (uint8_t *)heap_caps_malloc(LNET_RX_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  uint8_t *tx = (uint8_t *)heap_caps_malloc(LNET_TX_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  LNetTls *tc = nullptr;
  if (tls) tc = (LNetTls *)heap_caps_calloc(1, sizeof(LNetTls), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!rx || !tx || (tls && !tc)) {
    if (rx) heap_caps_free(rx);
    if (tx) heap_caps_free(tx);
    if (tc) heap_caps_free(tc);
    lua_pushnil(L); lua_pushstring(L, "no memory");
    return 2;
  }

  lock();
  int idx = -1;
  for (int i = 0; i < LNET_SLOTS; i++)
    if (s_slots[i].state == NS_FREE) { idx = i; break; }
  if (idx < 0) {
    unlock();
    heap_caps_free(rx); heap_caps_free(tx);
    if (tc) heap_caps_free(tc);
    lua_pushnil(L); lua_pushstring(L, "no free socket");
    return 2;
  }
  NetSlot *s = &s_slots[idx];
  uint32_t gen = s->gen + 1;
  memset(s, 0, sizeof(*s));
  s->gen = gen;
  s->fd = -1;
  strlcpy(s->host, host, sizeof(s->host));
  s->port = (uint16_t)port;
  s->tls = tls;
  s->pin_mode = tls && pin_mode;
  s->have_pin = s->pin_mode && have_pin;
  if (s->have_pin) memcpy(s->pin, pin, 32);
  s->timeout_ms = timeout_ms;
  s->rx = rx;
  s->tx = tx;
  s->tc = tc;
  // Dotted-quad fast path: no DNS round.
  uint32_t a = ipaddr_addr(host);
  if (a != IPADDR_NONE) {
    s->dns_requested = true;
    s->dns_done = true;
    s->dns_ok = true;
    s->dns_ip = a;
    s->deadline = millis() + LNET_DNS_TIMEOUT_MS;
  }
  s->state = NS_RESOLVE;
  bool need_task = !s_task_running;
  if (need_task) {
    if (xTaskCreatePinnedToCore(lnet_task, "lua_net", 16 * 1024, nullptr, 1,
                                nullptr, 1) == pdPASS) {
      s_task_running = true;
    } else {
      s->state = NS_FREE;
      s->rx = nullptr; s->tx = nullptr; s->tc = nullptr;
      unlock();
      heap_caps_free(rx); heap_caps_free(tx);
      if (tc) heap_caps_free(tc);
      lua_pushnil(L); lua_pushstring(L, "no task memory");
      return 2;
    }
  }
  h->slot = (uint8_t)idx;
  h->gen = gen;
  h->detached = false;
  unlock();
  SLog.printf("[NET] %d %s:%d connecting%s\n", idx, host, port, tls ? " (tls)" : "");
  return 1;   // the userdata
}

// ── registration + teardown ─────────────────────────────────────────────────

void lua_net_register_lua(lua_State *L) {
  if (!s_mutex) s_mutex = xSemaphoreCreateMutex();
  luaL_newmetatable(L, "lua_net_sock");
  lua_newtable(L);
  lua_pushcfunction(L, l_sock_state); lua_setfield(L, -2, "state");
  lua_pushcfunction(L, l_sock_send);  lua_setfield(L, -2, "send");
  lua_pushcfunction(L, l_sock_recv);  lua_setfield(L, -2, "recv");
  lua_pushcfunction(L, l_sock_info);  lua_setfield(L, -2, "info");
  lua_pushcfunction(L, l_sock_close); lua_setfield(L, -2, "close");
  lua_setfield(L, -2, "__index");
  lua_pushcfunction(L, l_sock_gc);    lua_setfield(L, -2, "__gc");
  lua_pop(L, 1);
  lua_register(L, "_tcp_open", l_tcp_open);
}

void lua_net_close_all(const char *reason) {
  if (!s_mutex) return;
  lock();
  bool running = s_task_running;
  for (int i = 0; i < LNET_SLOTS; i++) {
    NetSlot *s = &s_slots[i];
    if (slot_active(s)) {
      strlcpy(s->pending_reason, reason ? reason : "closed", sizeof(s->pending_reason));
      s->want_close = true;
    }
  }
  unlock();
  if (!running) return;
  // No worker step blocks, so the close sweep finishes within a few passes.
  for (int waited = 0; waited < 200; waited++) {
    lock();
    running = s_task_running;
    unlock();
    if (!running) return;
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  SLog.println("[NET] close_all: worker still running after 2s");
}

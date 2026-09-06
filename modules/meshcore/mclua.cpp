// mclua.cpp — the meshcore package Lua surface (ABI v2 lua_open/lua_tick).
//
// The _mesh_* binding bodies and the RX-event drain, moved VERBATIM from
// firmware main.cpp (regions tagged below); divergences from the firmware
// text are marked "MC-PKG divergence". The package registers ALL 72
// _mesh_* names — the store-reader aliases too (they run over the exported
// mstore surface), so released apps behave identically under the package.

#include "punkmesh.h"
#include "mc_bridges.h"
#include "mc_internal.h"
#include "shim/meshpunk_sync.h"
#include "shim/notify.h"
#include "shim/esp_heap_caps.h"

extern "C" {
#include <lauxlib.h>   // punkmesh.h pulls lua.h only; the bindings use luaL_*
}

extern PunkMesh* the_mesh;   // mcmain.cpp

// The RX-push functions (punkmesh.cpp — signatures verbatim from there).
void lua_mesh_push_direct_message(lua_State*, const char*, uint8_t, bool, uint32_t, const char*, float, float, uint16_t, const uint8_t*, const uint8_t*);
void lua_mesh_push_channel_message(lua_State*, const char*, uint8_t, bool, uint32_t, const char*, float, float, int, uint16_t, const uint8_t*, const uint8_t*);
void lua_mesh_push_contact_update(lua_State*, const char*, uint8_t);
void lua_mesh_push_ack(lua_State*, uint32_t, int32_t);
void lua_mesh_push_room_message(lua_State*, const char*, const char*, uint8_t, bool, uint32_t, const char*, float, float, uint16_t, const uint8_t*, const uint8_t*);
void lua_mesh_push_cli_response(lua_State*, const char*, const char*, uint32_t);
void lua_mesh_push_login_result(lua_State*, const char*, bool, uint8_t, uint32_t);
void lua_mesh_push_status_text(lua_State*, const char*, const char*);
void lua_mesh_push_send_retry(lua_State*, uint32_t, uint8_t, uint8_t);
void lua_mesh_push_conn_lost(lua_State*, const char*);

// Firmware emoji decomposer (exported; the composer is declared in
// punkmesh.cpp itself).
extern "C" char* emoji_decompose(const char* in);

// radio_apply_params/radio_apply_tx_power come from mc_bridges.h (shared
// with the companion's radio-command handlers).

// ── main.cpp region 2388-4293: binding bodies + wire-text helpers ──────────
// ── Mesh bridge: Lua → C++ ──────────────────────────────────────

// Prepare outgoing message text for the wire: expand composed PUA emoji back
// to their real Unicode sequences (peers must receive standard emoji — the
// PUA form only exists in this device's UI space), then normalize smart
// quotes into the fixed wire buffer. Finally trim any multi-byte codepoint
// split by the byte-wise 160-cap truncation so the wire text stays valid
// UTF-8 (decompose expansion makes hitting the cap likelier).
static void prepare_outgoing_text(const char *raw, char *out, size_t outlen) {
  char *expanded = emoji_decompose(raw);
  normalize_smart_quotes(expanded ? expanded : raw, out, outlen);
  if (expanded) free(expanded);

  size_t w = strlen(out);
  if (w == 0) return;
  size_t lead = w;
  while (lead > 0 && ((unsigned char)out[lead - 1] & 0xC0) == 0x80) lead--;
  if (lead == 0) return;                      // all continuation bytes — leave it
  unsigned char lb = (unsigned char)out[lead - 1];
  size_t need = (lb & 0x80) == 0    ? 1 :
                (lb & 0xE0) == 0xC0 ? 2 :
                (lb & 0xF0) == 0xE0 ? 3 :
                (lb & 0xF8) == 0xF0 ? 4 : 1;
  if (lead - 1 + need > w) out[lead - 1] = '\0';   // drop the partial tail
}

// Send a public/group channel message from Lua
// Usage from Lua: _mesh_send_public("Hello mesh!")
static int lua_mesh_send_public(lua_State *L) {
  const char *raw = luaL_checkstring(L, 1);
  // Normalize smart quotes so both the wire message and the local echo
  // render cleanly on receivers whose base font lacks U+2018-U+201D.
  char text[160];
  prepare_outgoing_text(raw, text, sizeof(text));

  SLog.printf("[MESH TX] lua_mesh_send_public called, text=\"%s\"\n", text);

  MESH_LOCK();
  int pub_idx = the_mesh->publicChannelIdx();   // Public is a normal channel; resolve by name
  if (pub_idx < 0) {
    MESH_UNLOCK();
    SLog.println("[MESH TX] ERROR: No public channel configured!");
    lua_pushboolean(L, 0);
    lua_pushstring(L, "No public channel configured");
    return 2;
  }

  uint32_t timestamp = the_mesh->getRTCClock()->getCurrentTime();
  uint8_t tx_hash[MAX_HASH_SIZE];
  bool ok = the_mesh->sendAndPersistChannelMsg(pub_idx, timestamp, text, strlen(text), tx_hash);
  MESH_UNLOCK();

  lua_pushboolean(L, ok ? 1 : 0);
  if (ok) {
    char hex[MAX_HASH_SIZE * 2 + 1];
    mesh::Utils::toHex(hex, tx_hash, MAX_HASH_SIZE);
    lua_pushstring(L, hex);
  } else {
    lua_pushnil(L);
  }
  return 2;
}

// Send a direct message to a contact by name prefix
// Usage from Lua: _mesh_send_direct("alice", "Hey!")
static int lua_mesh_send_direct(lua_State *L) {
  const char *name_prefix = luaL_checkstring(L, 1);
  const char *raw = luaL_checkstring(L, 2);
  char text[160];
  prepare_outgoing_text(raw, text, sizeof(text));

  MESH_LOCK();
  ContactInfo *recipient = the_mesh->searchContactsByPrefix(name_prefix);
  if (!recipient) {
    MESH_UNLOCK();
    lua_pushboolean(L, 0);
    lua_pushstring(L, "Contact not found");
    return 2;
  }

  uint32_t timestamp = the_mesh->getRTCClock()->getCurrentTime();

  auto r = the_mesh->sendAndPersistDM(*recipient, timestamp, 0, text);
  if (r.code != MSG_SEND_FAILED && r.expected_ack != 0) {
    // Track this send in the retry ladder (3 tries via path, then the path
    // resets and 2 more go flooded). Device-UI sends only — BLE sends run
    // the phone app's own retry logic.
    the_mesh->armPendingSend(*recipient, r.expected_ack, timestamp, text,
                             r.code == MSG_SEND_SENT_DIRECT, r.est_timeout);
  }
  MESH_UNLOCK();

  if (r.code == MSG_SEND_FAILED) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, "Send failed");
    return 2;
  }

  // Returns: ok, route("flood"/"direct"), expected_ack (uint32, 0 if none),
  // hash (hex string for flood, else nil). The UI uses expected_ack to match
  // the delivery result delivered later via messages.__dispatch_ack().
  bool is_flood = (r.code == MSG_SEND_SENT_FLOOD);
  lua_pushboolean(L, 1);
  lua_pushstring(L, is_flood ? "flood" : "direct");
  lua_pushinteger(L, (lua_Integer)r.expected_ack);
  if (r.has_hash) {
    char hex[MAX_HASH_SIZE * 2 + 1];
    mesh::Utils::toHex(hex, r.tx_hash, MAX_HASH_SIZE);
    lua_pushstring(L, hex);
  } else {
    lua_pushnil(L);
  }
  return 4;
}

// Get this node's info (name, pubkey hex, freq, tx power)
// Usage from Lua: local info = _mesh_get_node_info()
static int lua_mesh_get_node_info(lua_State *L) {
  lua_newtable(L);

  MESH_LOCK();
  lua_pushstring(L, the_mesh->_prefs.node_name);
  lua_setfield(L, -2, "name");

  // Public key as hex string
  char hex[PUB_KEY_SIZE * 2 + 1];
  mesh::Utils::toHex(hex, the_mesh->self_id.pub_key, PUB_KEY_SIZE);
  lua_pushstring(L, hex);
  lua_setfield(L, -2, "pubkey");

  lua_pushnumber(L, the_mesh->_prefs.freq);
  lua_setfield(L, -2, "freq");

  lua_pushinteger(L, the_mesh->_prefs.tx_power_dbm);
  lua_setfield(L, -2, "tx_power");

  lua_pushnumber(L, the_mesh->_prefs.node_lat);
  lua_setfield(L, -2, "lat");

  lua_pushnumber(L, the_mesh->_prefs.node_lon);
  lua_setfield(L, -2, "lon");

  lua_pushnumber(L, the_mesh->_prefs.bandwidth);
  lua_setfield(L, -2, "bandwidth");

  lua_pushinteger(L, the_mesh->_prefs.spreading_factor);
  lua_setfield(L, -2, "spreading_factor");

  lua_pushinteger(L, the_mesh->_prefs.coding_rate);
  lua_setfield(L, -2, "coding_rate");

  lua_pushboolean(L, the_mesh->_prefs.contact_overwrite != 0);
  lua_setfield(L, -2, "contact_overwrite");

  lua_pushboolean(L, the_mesh->_prefs.archive_contacts != 0);
  lua_setfield(L, -2, "archive_contacts");
  MESH_UNLOCK();

  return 1;
}

static int lua_mesh_export_private_key(lua_State *L) {
  MESH_LOCK();
  char hex[PRV_KEY_SIZE * 2 + 1];
  mesh::Utils::toHex(hex, the_mesh->getPrivateKey(), PRV_KEY_SIZE);
  hex[PRV_KEY_SIZE * 2] = '\0';
  MESH_UNLOCK();
  lua_pushstring(L, hex);
  return 1;
}

static int lua_mesh_import_private_key(lua_State *L) {
  const char* hex = luaL_checkstring(L, 1);
  if (strlen(hex) != PRV_KEY_SIZE * 2) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, "key must be 128 hex chars");
    return 2;
  }
  uint8_t prv[PRV_KEY_SIZE];
  if (!mesh::Utils::fromHex(prv, PRV_KEY_SIZE, hex)) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, "invalid hex");
    return 2;
  }
  if (!mesh::LocalIdentity::validatePrivateKey(prv)) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, "key validation failed");
    return 2;
  }
  MESH_LOCK();
  the_mesh->self_id.readFrom(prv, PRV_KEY_SIZE);
  bool ok = the_mesh->saveIdentity();
  MESH_UNLOCK();
  if (ok) {
    // MC-PKG divergence: a protocol module never restarts the device (the
    // firmware version rebooted here). Identity is SAVED; the caller reboots
    // via _system_reboot — 5f parity item for the Settings/Identity flow.
    MCH->log("identity imported and saved - reboot required");
    lua_pushboolean(L, 1);
    lua_pushstring(L, "reboot required");
    return 2;
  }
  lua_pushboolean(L, 0);
  lua_pushstring(L, "file write failed");
  return 2;
}

static int lua_mesh_generate_identity(lua_State *L) {
  MESH_LOCK();
  the_mesh->self_id = mesh::LocalIdentity(the_mesh->getRNG());
  int count = 0;
  while (count < 10 && (the_mesh->self_id.pub_key[0] == 0x00 || the_mesh->self_id.pub_key[0] == 0xFF)) {
    the_mesh->self_id = mesh::LocalIdentity(the_mesh->getRNG());
    count++;
  }
  bool ok = the_mesh->saveIdentity();
  MESH_UNLOCK();
  if (ok) {
    // MC-PKG divergence: no device restart from a protocol module (see
    // import above) — saved; the caller reboots via _system_reboot.
    MCH->log("new identity generated and saved - reboot required");
    lua_pushboolean(L, 1);
    lua_pushstring(L, "reboot required");
    return 2;
  }
  lua_pushboolean(L, 0);
  lua_pushstring(L, "file write failed");
  return 2;
}

// Get contact list
// Push one contact as a Lua table (shared by the live and union caches).
static void push_contact_table(lua_State *L, const ContactInfo &c, bool archived) {
  lua_newtable(L);

  lua_pushstring(L, c.name);
  lua_setfield(L, -2, "name");

  lua_pushinteger(L, c.type);
  lua_setfield(L, -2, "type");

  lua_pushinteger(L, c.out_path_len);
  lua_setfield(L, -2, "path_len");

  lua_pushinteger(L, c.lastmod);   // "last seen" = our RX clock (0 = unheard since boot)
  lua_setfield(L, -2, "last_seen");

  lua_pushinteger(L, c.lastmod);
  lua_setfield(L, -2, "lastmod");

  lua_pushinteger(L, c.last_advert_timestamp);   // sender's advert clock — recorded only
  lua_setfield(L, -2, "sender_advert_ts");

  char hex[PUB_KEY_SIZE * 2 + 1];
  mesh::Utils::toHex(hex, c.id.pub_key, PUB_KEY_SIZE);
  lua_pushstring(L, hex);
  lua_setfield(L, -2, "pubkey");

  lua_pushstring(L, the_mesh->getTypeName(c.type));
  lua_setfield(L, -2, "type_name");

  lua_pushboolean(L, (c.flags & 0x01) != 0);
  lua_setfield(L, -2, "favorite");

  // out_path as array of hex hashes. out_path_len 0xFF is the
  // OUT_PATH_UNKNOWN sentinel (no route learned) — it must NOT be decoded
  // as size/count (it reads as 63 hashes of 4 bytes and used to overflow
  // the hex buffer); unknown routes get an empty path table.
  {
    lua_newtable(L);
    if (c.out_path_len != OUT_PATH_UNKNOWN) {
      uint8_t hash_size = (c.out_path_len >> 6) + 1;
      uint8_t hash_count = c.out_path_len & 63;
      char h[9];  // up to 4-byte hashes (8 hex chars + NUL)
      for (int j = 0; j < hash_count && (j + 1) * hash_size <= MAX_PATH_SIZE; j++) {
        mesh::Utils::toHex(h, &c.out_path[j * hash_size], hash_size);
        lua_pushstring(L, h);
        lua_rawseti(L, -2, j + 1);
      }
    }
    lua_setfield(L, -2, "path");
  }

  lua_pushnumber(L, c.gps_lat / 1000000.0);
  lua_setfield(L, -2, "lat");
  lua_pushnumber(L, c.gps_lon / 1000000.0);
  lua_setfield(L, -2, "lon");

  if (archived) {
    lua_pushboolean(L, 1);
    lua_setfield(L, -2, "archived");
  }
}

// Usage from Lua: local contacts = _mesh_get_contacts([include_archived])
//
// Cached: rebuilding ~500 contact tables (pubkey hex, path arrays, ...)
// costs ~10ms under MESH_LOCK, and the Map app asks on every marker redraw.
// PunkMesh bumps contacts_generation on every mutation (they all funnel
// through saveContacts), so between changes this returns a cheap copy of a
// cached master table. The OUTER array is fresh per call — callers may
// table.sort it in place (Messenger does) — while the per-contact subtables
// are shared with the cache and must be treated as read-only. A 10s TTL
// backstops any mutation path that might miss the generation bump (e.g.
// BLE companion ops run outside MESH_LOCK, so a bump could in theory race).
//
// With include_archived = true the result also contains archived contacts
// (those evicted from the live table or removed; marked archived=true),
// deduped by pubkey with the live entry winning. That variant has its own
// cached master keyed on both generation counters.
static int s_contacts_ref = LUA_NOREF;       // live-only master
static uint32_t s_contacts_gen = 0;
static uint32_t s_contacts_built_ms = 0;
static int s_contacts_count = 0;

static int s_union_ref = LUA_NOREF;          // live + archived master
static uint32_t s_union_gen = 0;
static uint32_t s_union_arch_gen = 0;
static uint32_t s_union_built_ms = 0;
static int s_union_count = 0;

// Copy the live contact table into a transient PSRAM snapshot under MESH_LOCK.
// Returns the buffer (caller frees) or NULL; *out_n = contacts copied. Exists
// so the Lua pushes below run with NO locks held: lua_push* can longjmp on a
// true OOM, and an escape while MESH_LOCK is held would deadlock the mesh task
// permanently — strictly worse than the OOM itself. Bonus: the lock is now held
// only for a memcpy loop, not table pushes + archive-file I/O.
static ContactInfo* snapshot_live_contacts(int* out_n) {
  *out_n = 0;
  MESH_LOCK();
  int n = the_mesh->getNumContacts();
  ContactInfo* live = (ContactInfo*)heap_caps_malloc(
      sizeof(ContactInfo) * (n > 0 ? n : 1), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (live) {
    ContactInfo c;
    int nlive = 0;
    for (int i = 0; i < n; i++) {
      if (the_mesh->getContactByIdx(i, c)) live[nlive++] = c;
    }
    *out_n = nlive;
  }
  MESH_UNLOCK();
  return live;
}

static int lua_mesh_get_contacts(lua_State *L) {
  bool include_archived = lua_toboolean(L, 1);

  int ref;
  int count;

  if (!include_archived) {
    MESH_LOCK();
    uint32_t gen = the_mesh->contacts_generation;
    MESH_UNLOCK();
    bool fresh = (s_contacts_ref != LUA_NOREF) && (gen == s_contacts_gen) &&
                 (millis() - s_contacts_built_ms < 10000);
    if (!fresh) {
      int nlive = 0;
      ContactInfo* live = snapshot_live_contacts(&nlive);
      if (!live) {
        // No snapshot memory: serve the stale cache if one exists, else empty.
        if (s_contacts_ref == LUA_NOREF) {
          lua_newtable(L);
          return 1;
        }
      } else {
        lua_newtable(L);
        for (int i = 0; i < nlive; i++) {
          push_contact_table(L, live[i], false);
          lua_rawseti(L, -2, i + 1);
        }
        heap_caps_free(live);

        if (s_contacts_ref != LUA_NOREF) {
          luaL_unref(L, LUA_REGISTRYINDEX, s_contacts_ref);
        }
        s_contacts_count = nlive;
        s_contacts_ref = luaL_ref(L, LUA_REGISTRYINDEX);  // pops the master
        s_contacts_gen = gen;
        s_contacts_built_ms = millis();
      }
    }
    ref = s_contacts_ref;
    count = s_contacts_count;
  } else {
    MESH_LOCK();
    uint32_t gen = the_mesh->contacts_generation;
    uint32_t agen = the_mesh->archive_generation;
    MESH_UNLOCK();
    bool fresh = (s_union_ref != LUA_NOREF) && (gen == s_union_gen) &&
                 (agen == s_union_arch_gen) &&
                 (millis() - s_union_built_ms < 10000);
    if (!fresh) {
      int nlive = 0;
      ContactInfo* live = snapshot_live_contacts(&nlive);
      if (!live) {
        if (s_union_ref == LUA_NOREF) {
          lua_newtable(L);
          return 1;
        }
      } else {
        // Archived contacts live on disk only. Read a transient, deduped view
        // here (freed immediately after) so the archive costs ZERO steady-state
        // PSRAM — this whole branch only runs when the user has "show archived"
        // on, and is cached for 10s. The on-map display is bounded; the disk
        // archive keeps everything (re-add can still pull back any contact).
        // readArchivedDeduped does its own SPI locking — no MESH_LOCK needed.
        const int ARCH_DISPLAY_MAX = 1000;
        ContactInfo* abuf = (ContactInfo*)heap_caps_malloc(
            sizeof(ContactInfo) * ARCH_DISPLAY_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        int na = 0;
        if (abuf) na = the_mesh->readArchivedDeduped(abuf, ARCH_DISPLAY_MAX);

        lua_newtable(L);
        int idx = 1;
        for (int i = 0; i < nlive; i++) {
          push_contact_table(L, live[i], false);
          lua_rawseti(L, -2, idx++);
        }
        if (abuf) {
          for (int i = 0; i < na; i++) {
            // Live wins by pubkey (checked against the snapshot) — also
            // self-heals entries left behind when a contact re-adverted in.
            bool is_live = false;
            for (int j = 0; j < nlive; j++) {
              if (memcmp(live[j].id.pub_key, abuf[i].id.pub_key, PUB_KEY_SIZE) == 0) {
                is_live = true;
                break;
              }
            }
            if (!is_live) {
              push_contact_table(L, abuf[i], true);
              lua_rawseti(L, -2, idx++);
            }
          }
          heap_caps_free(abuf);
        }
        heap_caps_free(live);

        if (s_union_ref != LUA_NOREF) {
          luaL_unref(L, LUA_REGISTRYINDEX, s_union_ref);
        }
        s_union_count = idx - 1;
        s_union_ref = luaL_ref(L, LUA_REGISTRYINDEX);  // pops the master
        s_union_gen = gen;
        s_union_arch_gen = agen;
        s_union_built_ms = millis();
      }
    }
    ref = s_union_ref;
    count = s_union_count;
  }

  // Hand out a fresh outer array sharing the cached per-contact tables.
  lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
  lua_createtable(L, count, 0);
  for (int i = 1; i <= count; i++) {
    lua_rawgeti(L, -2, i);
    lua_rawseti(L, -2, i);
  }
  lua_remove(L, -2);  // drop the master, leave the copy
  return 1;
}

// _mesh_drop_contacts_cache(): release the cached contact master tables (live-only
// + live+archived). They're pinned in the Lua registry via luaL_ref, so they
// survive app teardown and GC — ~388KB for 500 contacts, parked mid-heap. Only the
// Map and Messenger consume them, so the launcher drops them on app launch to give
// a heavy app (Doom/PICO-8) the contiguous PSRAM back. The next _mesh_get_contacts
// call rebuilds from scratch (the generation/TTL logic is unchanged — clearing the
// refs just forces a fresh build). Lua-state only (Core 0), so no MESH_LOCK needed.
static int lua_mesh_drop_contacts_cache(lua_State *L) {
  if (s_contacts_ref != LUA_NOREF) {
    luaL_unref(L, LUA_REGISTRYINDEX, s_contacts_ref);
    s_contacts_ref = LUA_NOREF;
    s_contacts_count = 0;
    s_contacts_gen = 0;
    s_contacts_built_ms = 0;
  }
  if (s_union_ref != LUA_NOREF) {
    luaL_unref(L, LUA_REGISTRYINDEX, s_union_ref);
    s_union_ref = LUA_NOREF;
    s_union_count = 0;
    s_union_gen = 0;
    s_union_arch_gen = 0;
    s_union_built_ms = 0;
  }
  return 0;
}

// _mesh_archive_read(offset, max) -> contacts_table, next_offset, done
// One batch of archived contacts from the disk log, for the Map's progressive
// "show archived" loader. Stateless (byte-offset based) so the mesh task keeps
// appending between batches. Raw lines (no dedup/live-skip) — caller decides.
static int lua_mesh_archive_read(lua_State *L) {
  uint32_t offset = (uint32_t)luaL_optinteger(L, 1, 0);
  int max_count = (int)luaL_optinteger(L, 2, 150);
  if (max_count < 1) max_count = 1;
  if (max_count > 300) max_count = 300;  // bound the transient buffer

  ContactInfo *buf = (ContactInfo *)heap_caps_malloc(
      sizeof(ContactInfo) * max_count, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buf) {
    lua_newtable(L);
    lua_pushinteger(L, offset);
    lua_pushboolean(L, true);
    return 3;
  }

  uint32_t next_offset = offset;
  bool done = true;
  // No MESH_LOCK: only touches the archive file (sd_spi serialized inside),
  // not the live contact table.
  int n = the_mesh->readArchiveBatch(offset, max_count, buf, &next_offset, &done);

  lua_newtable(L);
  for (int i = 0; i < n; i++) {
    const ContactInfo &c = buf[i];
    // LEAN entry — only what the Map needs to draw a gray dot and open the
    // re-add popup (name/pubkey/type/last_seen/lat/lon). NO path array / lastmod
    // / favorite, so thousands can be held for a fraction of the PSRAM the full
    // push_contact_table would cost.
    lua_newtable(L);
    lua_pushstring(L, c.name);                          lua_setfield(L, -2, "name");
    char hex[PUB_KEY_SIZE * 2 + 1];
    mesh::Utils::toHex(hex, c.id.pub_key, PUB_KEY_SIZE);
    lua_pushstring(L, hex);                             lua_setfield(L, -2, "pubkey");
    lua_pushstring(L, the_mesh->getTypeName(c.type));   lua_setfield(L, -2, "type_name");
    lua_pushinteger(L, (lua_Integer)c.lastmod); lua_setfield(L, -2, "last_seen");  // our RX clock
    lua_pushnumber(L, c.gps_lat / 1000000.0);           lua_setfield(L, -2, "lat");
    lua_pushnumber(L, c.gps_lon / 1000000.0);           lua_setfield(L, -2, "lon");
    lua_pushboolean(L, 1);                              lua_setfield(L, -2, "archived");
    lua_rawseti(L, -2, i + 1);
  }
  heap_caps_free(buf);

  lua_pushinteger(L, (lua_Integer)next_offset);
  lua_pushboolean(L, done);
  return 3;
}

// _mesh_archive_compact() -> before, after (record counts) | nil, errcode
// Streaming dedup rewrite of the archive log (one record per pubkey, newest
// wins, live contacts dropped) + index rebuild. No MESH_LOCK here —
// compactArchive manages its own bounded lock windows so the radio never
// stalls for the whole rewrite. Nothing crosses into Lua but two integers.
static int lua_mesh_archive_compact(lua_State *L) {
  uint32_t before = 0, after = 0;
  int rc = the_mesh->compactArchive(&before, &after);
  if (rc != 0) {
    lua_pushnil(L);
    lua_pushinteger(L, rc);
    return 2;
  }
  lua_pushinteger(L, (lua_Integer)before);
  lua_pushinteger(L, (lua_Integer)after);
  return 2;
}

// _mesh_archive_count() -> records currently in the log (duplicates included).
// One file stat — no scan, no lock beyond the SD bus.
static int lua_mesh_archive_count(lua_State *L) {
  lua_pushinteger(L, (lua_Integer)the_mesh->archiveRecordCount());
  return 1;
}

// Helper for _mesh_search_contact_names: ASCII-lowercase `name`, and if it
// contains `q` (already lowercased) and isn't a name we've already collected,
// append the ORIGINAL-case name to the result table (at the top of the Lua stack)
// and record its lowercased form in `seen`. Returns the new match count.
static int search_try_add_name(lua_State *L, const char *name, const char *q,
                               char seen[][32], int count, int max) {
  if (count >= max) return count;
  char low[32];
  int ln = 0;
  for (const char *p = name; *p && ln < 31; p++) {
    char ch = *p;
    if (ch >= 'A' && ch <= 'Z') ch += 32;   // ASCII lower (matches Lua :lower())
    low[ln++] = ch;
  }
  low[ln] = '\0';
  if (!strstr(low, q)) return count;                 // no substring match
  for (int j = 0; j < count; j++)
    if (strcmp(seen[j], low) == 0) return count;     // name already collected
  strncpy(seen[count], low, 31);
  seen[count][31] = '\0';
  lua_pushstring(L, name);                            // original-case name
  lua_rawseti(L, -2, count + 1);                      // result[count+1] = name
  return count + 1;
}

// _mesh_search_contact_names(query, include_archived, max) -> { name, ... }
// Case-insensitive (ASCII) substring search over contact names, returning ONLY
// the matching names (deduped by name, <= max). Replaces the Map search's old
// _mesh_get_contacts(true) + Lua filter, which materialized the entire ~1500-
// entry union table (~1MB — the worst single PSRAM fragmenter) just to pull out
// a few names. Live names matched under MESH_LOCK; archived streamed from the
// disk log in batches (no lock — archive file only, sd_spi serialized inside).
static int lua_mesh_search_contact_names(lua_State *L) {
  const char *query = luaL_checkstring(L, 1);
  bool inc_arch = lua_toboolean(L, 2);
  int max = (int)luaL_optinteger(L, 3, 40);
  if (max < 1) max = 1;
  if (max > 64) max = 64;          // bounds the on-stack dedup table

  char q[48];
  int qn = 0;
  for (const char *p = query; *p && qn < (int)sizeof(q) - 1; p++) {
    char ch = *p;
    if (ch >= 'A' && ch <= 'Z') ch += 32;
    q[qn++] = ch;
  }
  q[qn] = '\0';

  lua_newtable(L);                 // result array — stays at the stack top
  if (qn == 0 || !the_mesh) return 1;

  char seen[64][32];               // lowercased collected names (dedup)
  int count = 0;

  // Live contacts.
  MESH_LOCK();
  ContactInfo c;
  int nlive = the_mesh->getNumContacts();
  for (int i = 0; i < nlive && count < max; i++) {
    if (the_mesh->getContactByIdx(i, c)) {
      count = search_try_add_name(L, c.name, q, seen, count, max);
    }
  }
  MESH_UNLOCK();

  // Archived contacts (streamed from disk; raw lines, name-deduped above so a
  // re-archived/duplicate pubkey can't show the same name twice).
  if (inc_arch && count < max) {
    const int BATCH = 48;
    ContactInfo *abuf = (ContactInfo *)heap_caps_malloc(
        sizeof(ContactInfo) * BATCH, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (abuf) {
      uint32_t off = 0;
      bool done = false;
      while (!done && count < max) {
        uint32_t next = off;
        int n = the_mesh->readArchiveBatch(off, BATCH, abuf, &next, &done);
        for (int i = 0; i < n && count < max; i++) {
          count = search_try_add_name(L, abuf[i].name, q, seen, count, max);
        }
        if (n <= 0 || next == off) break;   // no progress -> stop
        off = next;
      }
      heap_caps_free(abuf);
    }
  }
  return 1;
}

// _mesh_readd_contact(pubkey_hex) -> bool
// Move an archived contact back into the live mesh table (route reset to
// flood; it re-establishes on the next path exchange).
static int lua_mesh_readd_contact(lua_State *L) {
  const char *pubkey_hex = luaL_checkstring(L, 1);

  uint8_t pub_key[PUB_KEY_SIZE];
  if (strlen(pubkey_hex) < PUB_KEY_SIZE * 2 ||
      !mesh::Utils::fromHex(pub_key, PUB_KEY_SIZE, pubkey_hex)) {
    lua_pushboolean(L, 0);
    return 1;
  }

  MESH_LOCK();
  bool ok = the_mesh->readdArchivedContact(pub_key);
  MESH_UNLOCK();

  lua_pushboolean(L, ok ? 1 : 0);
  return 1;
}

static int lua_mesh_get_contact_paths(lua_State *L) {
  const char *pubkey_hex = luaL_checkstring(L, 1);

  uint8_t pub_key[PUB_KEY_SIZE];
  mesh::Utils::fromHex(pub_key, PUB_KEY_SIZE, pubkey_hex);

  lua_newtable(L);

  MESH_LOCK();
  // The live contact (if any) — used to flag which record is the CURRENT
  // out_path, so the picker can mark it and offer the others.
  ContactInfo *contact = the_mesh->lookupContactByPubKey(pub_key, PUB_KEY_SIZE);

  ContactPathHistory *h = nullptr;
  for (int i = 0; i < the_mesh->_path_history_count; i++) {
    if (memcmp(the_mesh->_path_history[i].pub_key, pub_key, PUB_KEY_SIZE) == 0) {
      h = &the_mesh->_path_history[i];
      break;
    }
  }
  if (h && h->count > 0) {
    static const char *src_names[] = { "msg", "ack", "path_update", "advert" };
    for (int i = 0; i < h->count; i++) {
      PathRecord &r = h->records[i];
      lua_newtable(L);

      uint8_t hash_size = (r.path_len >> 6) + 1;
      uint8_t hash_count = r.path_len & 63;
      uint16_t byte_len = (uint16_t)hash_count * hash_size;
      if (byte_len > MAX_PATH_SIZE) byte_len = MAX_PATH_SIZE;

      // path as array of hex hashes
      {
        lua_newtable(L);
        char hex[7];
        for (int j = 0; j < hash_count && (j + 1) * hash_size <= MAX_PATH_SIZE; j++) {
          mesh::Utils::toHex(hex, &r.path[j * hash_size], hash_size);
          lua_pushstring(L, hex);
          lua_rawseti(L, -2, j + 1);
        }
        lua_setfield(L, -2, "path");

        lua_pushinteger(L, hash_count);
        lua_setfield(L, -2, "hops");
      }

      // Raw round-trip form for _mesh_set_contact_path ("Use this path").
      lua_pushinteger(L, r.path_len);
      lua_setfield(L, -2, "path_len");
      {
        char phex[MAX_PATH_SIZE * 2 + 1];
        mesh::Utils::toHex(phex, r.path, byte_len);
        lua_pushstring(L, phex);
        lua_setfield(L, -2, "path_hex");
      }

      // Is this record the contact's CURRENT out_path?
      bool is_current = contact &&
                        contact->out_path_len != OUT_PATH_UNKNOWN &&
                        (uint16_t)contact->out_path_len == r.path_len &&
                        memcmp(contact->out_path, r.path, byte_len) == 0;
      lua_pushboolean(L, is_current ? 1 : 0);
      lua_setfield(L, -2, "current");

      lua_pushboolean(L, r.is_direct);
      lua_setfield(L, -2, "direct");

      lua_pushnumber(L, r.snr);
      lua_setfield(L, -2, "snr");

      lua_pushnumber(L, r.rssi);
      lua_setfield(L, -2, "rssi");

      lua_pushinteger(L, r.trip_time_ms);
      lua_setfield(L, -2, "trip_time_ms");

      lua_pushinteger(L, r.success_count);
      lua_setfield(L, -2, "success");

      lua_pushinteger(L, r.failure_count);
      lua_setfield(L, -2, "failure");

      lua_pushinteger(L, r.timestamp);
      lua_setfield(L, -2, "timestamp");

      int src_idx = r.source < 4 ? r.source : 0;
      lua_pushstring(L, src_names[src_idx]);
      lua_setfield(L, -2, "source");

      lua_rawseti(L, -2, i + 1);
    }
  }
  MESH_UNLOCK();

  return 1;
}

// Set a contact's CURRENT out_path from a history record ("Use this path"
// in the Paths picker). Takes the raw round-trip form that
// _mesh_get_contact_paths exposes per record (path_len + path_hex).
// path_len 0 with empty hex = zero-hop direct. Persisted via saveOneContact.
// Usage: local ok, err = _mesh_set_contact_path(pubkey_hex, path_len, path_hex)
static int lua_mesh_set_contact_path(lua_State *L) {
  const char *pubkey_hex = luaL_checkstring(L, 1);
  int path_len = luaL_checkinteger(L, 2);
  const char *path_hex = luaL_optstring(L, 3, "");

  if (strlen(pubkey_hex) != PUB_KEY_SIZE * 2) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, "Bad pubkey");
    return 2;
  }
  if (path_len < 0 || path_len >= OUT_PATH_UNKNOWN) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, "Bad path_len");
    return 2;
  }
  uint16_t byte_len = (uint16_t)(path_len & 63) * ((path_len >> 6) + 1);
  if (byte_len > MAX_PATH_SIZE || strlen(path_hex) != (size_t)byte_len * 2) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, "Bad path");
    return 2;
  }

  uint8_t pub_key[PUB_KEY_SIZE];
  mesh::Utils::fromHex(pub_key, PUB_KEY_SIZE, pubkey_hex);
  uint8_t path_bytes[MAX_PATH_SIZE];
  if (byte_len > 0) mesh::Utils::fromHex(path_bytes, byte_len, path_hex);

  MESH_LOCK();
  ContactInfo *c = the_mesh->lookupContactByPubKey(pub_key, PUB_KEY_SIZE);
  if (!c) {
    MESH_UNLOCK();
    lua_pushboolean(L, 0);
    lua_pushstring(L, "Contact not found");
    return 2;
  }
  memset(c->out_path, 0, sizeof(c->out_path));
  if (byte_len > 0) memcpy(c->out_path, path_bytes, byte_len);
  c->out_path_len = (uint8_t)path_len;
  c->lastmod = the_mesh->getRTCClock()->getCurrentTime();
  the_mesh->saveOneContact(*c);   // persists + bumps contacts_generation
  MESH_UNLOCK();

  lua_pushboolean(L, 1);
  return 1;
}

// Get all observed paths for a message by hash.
// Tries RAM buffer first; falls back to persisted log file.
// Usage: _mesh_get_message_paths(hash_hex)                     -- RAM only
//        _mesh_get_message_paths(hash_hex, channel_idx)        -- RAM → channel file
//        _mesh_get_message_paths(hash_hex, -1, peer_name)      -- RAM → DM file
static int lua_mesh_get_message_paths(lua_State *L) {
  const char *hash_hex = luaL_checkstring(L, 1);
  int channel_idx = luaL_optinteger(L, 2, 0);
  const char *peer = luaL_optstring(L, 3, nullptr);

  if (strlen(hash_hex) != MAX_HASH_SIZE * 2) {
    lua_newtable(L);
    return 1;
  }
  uint8_t hash[MAX_HASH_SIZE];
  mesh::Utils::fromHex(hash, MAX_HASH_SIZE, hash_hex);

  MESH_LOCK();

  // Try RAM buffer first
  MsgPathEntry *e = the_mesh->findMsgPaths(hash);
  if (e && e->path_count > 0) {
    lua_newtable(L);
    for (int i = 0; i < e->path_count; i++) {
      ObservedPath &op = e->paths[i];
      lua_newtable(L);

      uint8_t hash_size = (op.path_len >> 6) + 1;
      uint8_t hop_count = op.path_len & 63;
      lua_newtable(L);
      char hex[7];
      for (int j = 0; j < hop_count && (j + 1) * hash_size <= MAX_PATH_SIZE; j++) {
        mesh::Utils::toHex(hex, &op.path[j * hash_size], hash_size);
        lua_pushstring(L, hex);
        lua_rawseti(L, -2, j + 1);
      }
      lua_setfield(L, -2, "path");

      lua_pushinteger(L, hop_count);
      lua_setfield(L, -2, "hops");

      lua_pushboolean(L, op.is_direct);
      lua_setfield(L, -2, "direct");

      lua_pushnumber(L, op.snr);
      lua_setfield(L, -2, "snr");

      lua_pushnumber(L, op.rssi);
      lua_setfield(L, -2, "rssi");

      lua_rawseti(L, -2, i + 1);
    }
    MESH_UNLOCK();
    return 1;
  }

  // RAM miss — fall back to persisted log file
  int r = the_mesh->lookupPersistedPaths(L, hash_hex, channel_idx, peer);
  MESH_UNLOCK();
  return r;
}

// Send self advertisement
// Usage from Lua: _mesh_send_advert()          -- flood (default)
//                  _mesh_send_advert("zerohop") -- zero-hop only
static int lua_mesh_send_advert(lua_State *L) {
  const char *mode = luaL_optstring(L, 1, "flood");
  MESH_LOCK();
  auto pkt = the_mesh->buildSelfAdvert();
  if (pkt) {
    if (strcmp(mode, "zerohop") == 0) {
      the_mesh->sendZeroHop(pkt, (uint32_t)0);
    } else {
      the_mesh->sendFlood(pkt, (uint32_t)0, the_mesh->pathHashSize());
    }
  }
  MESH_UNLOCK();
  lua_pushboolean(L, pkt ? 1 : 0);
  return 1;
}

// Get number of contacts
static int lua_mesh_get_num_contacts(lua_State *L) {
  MESH_LOCK();
  int n = the_mesh->getNumContacts();
  MESH_UNLOCK();
  lua_pushinteger(L, n);
  return 1;
}

// Set a node config value
// Usage from Lua: _mesh_set_config("name", "MyNode")
//                 _mesh_set_config("freq", "915.525")
//                 _mesh_set_config("tx", "20")
//                 _mesh_set_config("bw", "250")
//                 _mesh_set_config("sf", "10")
//                 _mesh_set_config("cr", "5")
//                 _mesh_set_config("lat", "37.7749")
//                 _mesh_set_config("lon", "-122.4194")
static int lua_mesh_set_config(lua_State *L) {
  const char *key = luaL_checkstring(L, 1);
  const char *value = luaL_checkstring(L, 2);

  MESH_LOCK();
  if (strcmp(key, "name") == 0) {
    strncpy(the_mesh->_prefs.node_name, value, sizeof(the_mesh->_prefs.node_name) - 1);
    the_mesh->_prefs.node_name[sizeof(the_mesh->_prefs.node_name) - 1] = '\0';
    the_mesh->savePrefs();
    SLog.printf("Node name set to: %s\n", the_mesh->_prefs.node_name);
    lua_pushboolean(L, 1);
  } else if (strcmp(key, "freq") == 0) {
    the_mesh->_prefs.freq = atof(value);
    the_mesh->savePrefs();
    radio_apply_params(the_mesh->_prefs.freq, the_mesh->_prefs.bandwidth,
                       the_mesh->_prefs.spreading_factor, the_mesh->_prefs.coding_rate);
    SLog.printf("Frequency set to: %.3f (applied)\n", the_mesh->_prefs.freq);
    lua_pushboolean(L, 1);
  } else if (strcmp(key, "tx") == 0) {
    the_mesh->_prefs.tx_power_dbm = atoi(value);
    the_mesh->savePrefs();
    radio_apply_tx_power(the_mesh->_prefs.tx_power_dbm);
    SLog.printf("TX power set to: %d dBm (applied)\n", the_mesh->_prefs.tx_power_dbm);
    lua_pushboolean(L, 1);
  } else if (strcmp(key, "lat") == 0) {
    the_mesh->_prefs.node_lat = atof(value);
    the_mesh->savePrefs();
    lua_pushboolean(L, 1);
  } else if (strcmp(key, "lon") == 0) {
    the_mesh->_prefs.node_lon = atof(value);
    the_mesh->savePrefs();
    lua_pushboolean(L, 1);
  } else if (strcmp(key, "bw") == 0) {
    the_mesh->_prefs.bandwidth = atof(value);
    the_mesh->savePrefs();
    radio_apply_params(the_mesh->_prefs.freq, the_mesh->_prefs.bandwidth,
                       the_mesh->_prefs.spreading_factor, the_mesh->_prefs.coding_rate);
    SLog.printf("Bandwidth set to: %.1f kHz (applied)\n", the_mesh->_prefs.bandwidth);
    lua_pushboolean(L, 1);
  } else if (strcmp(key, "sf") == 0) {
    the_mesh->_prefs.spreading_factor = atoi(value);
    the_mesh->savePrefs();
    radio_apply_params(the_mesh->_prefs.freq, the_mesh->_prefs.bandwidth,
                       the_mesh->_prefs.spreading_factor, the_mesh->_prefs.coding_rate);
    SLog.printf("Spreading factor set to: %d (applied)\n", the_mesh->_prefs.spreading_factor);
    lua_pushboolean(L, 1);
  } else if (strcmp(key, "cr") == 0) {
    the_mesh->_prefs.coding_rate = atoi(value);
    the_mesh->savePrefs();
    radio_apply_params(the_mesh->_prefs.freq, the_mesh->_prefs.bandwidth,
                       the_mesh->_prefs.spreading_factor, the_mesh->_prefs.coding_rate);
    SLog.printf("Coding rate set to: %d (applied)\n", the_mesh->_prefs.coding_rate);
    lua_pushboolean(L, 1);
  } else if (strcmp(key, "contact_overwrite") == 0) {
    the_mesh->_prefs.contact_overwrite = (atoi(value) != 0) ? 1 : 0;
    the_mesh->savePrefs();
    SLog.printf("Contact overwrite set to: %s\n", the_mesh->_prefs.contact_overwrite ? "ON" : "OFF");
    lua_pushboolean(L, 1);
  } else if (strcmp(key, "archive_contacts") == 0) {
    the_mesh->_prefs.archive_contacts = (atoi(value) != 0) ? 1 : 0;
    the_mesh->savePrefs();
    SLog.printf("Archive contacts set to: %s\n", the_mesh->_prefs.archive_contacts ? "ON" : "OFF");
    lua_pushboolean(L, 1);
  } else {
    MESH_UNLOCK();
    lua_pushboolean(L, 0);
    lua_pushstring(L, "Unknown config key");
    return 2;
  }
  MESH_UNLOCK();

  return 1;
}

// ── New Mesh bridge functions for full MeshCore integration ──────

// Get all channels
// Usage: local channels = _mesh_get_channels()
// Returns: {{idx=0, name="Public", has_key=true}, ...}
static int lua_mesh_get_channels(lua_State *L) {
  lua_newtable(L);
  int idx = 1;

  MESH_LOCK();
  for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
    ChannelDetails cd;
    if (the_mesh->getChannel(i, cd)) {
      // Check if channel has a non-empty name
      if (cd.name[0] != '\0') {
        lua_newtable(L);

        lua_pushinteger(L, i);
        lua_setfield(L, -2, "idx");

        lua_pushstring(L, cd.name);
        lua_setfield(L, -2, "name");

        // Check if secret is non-zero
        bool has_key = false;
        for (int j = 0; j < PUB_KEY_SIZE; j++) {
          if (cd.channel.secret[j] != 0) { has_key = true; break; }
        }
        lua_pushboolean(L, has_key ? 1 : 0);
        lua_setfield(L, -2, "has_key");

        lua_rawseti(L, -2, idx++);
      }
    }
  }
  MESH_UNLOCK();

  return 1;
}

// Set a channel by index
// Usage: _mesh_set_channel(1, "MyChannel", "base64psk")
//        _mesh_set_channel(1, "", "")  -- delete channel
static int lua_mesh_set_channel(lua_State *L) {
  int ch_idx = luaL_checkinteger(L, 1);
  const char *name = luaL_checkstring(L, 2);
  const char *psk = luaL_optstring(L, 3, "");

  if (ch_idx < 0 || ch_idx >= MAX_GROUP_CHANNELS) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, "Channel index out of range");
    return 2;
  }

  MESH_LOCK();
  if (strlen(name) == 0) {
    // Delete channel: set empty name and zero secret
    ChannelDetails cd;
    memset(&cd, 0, sizeof(cd));
    the_mesh->setChannel(ch_idx, cd);
    the_mesh->saveChannels();
    MESH_UNLOCK();
    lua_pushboolean(L, 1);
    return 1;
  }

  // Check if it's a hashtag channel (name starts with #)
  if (name[0] == '#') {
    // Hashtag channel: secret = first 16 bytes of sha256(name)
    ChannelDetails cd;
    memset(&cd, 0, sizeof(cd));
    strncpy(cd.name, name, sizeof(cd.name) - 1);
    // Compute sha256 of the channel name to derive key
    uint8_t hash[32];
    mesh::Utils::sha256(hash, 32, (const uint8_t*)name, strlen(name));
    memcpy(cd.channel.secret, hash, 16);
    mesh::Utils::sha256(cd.channel.hash, sizeof(cd.channel.hash), cd.channel.secret, 16);
    the_mesh->setChannel(ch_idx, cd);
    the_mesh->saveChannels();
    MESH_UNLOCK();
    lua_pushboolean(L, 1);
    return 1;
  }

  // Normal channel with PSK
  ChannelDetails *result = the_mesh->addChannel(name, psk);
  if (!result) {
    // addChannel only works for new slots, try setChannel directly
    // Parse the base64 PSK manually
    ChannelDetails cd;
    memset(&cd, 0, sizeof(cd));
    strncpy(cd.name, name, sizeof(cd.name) - 1);
    // Use the existing setChannel which will compute the hash
    // But we need to decode base64 first
    extern unsigned int decode_base64(unsigned char const *src, unsigned int slen, unsigned char *target);
    int len = decode_base64((unsigned char *)psk, strlen(psk), cd.channel.secret);
    if (len != 16 && len != 32) {
      MESH_UNLOCK();
      lua_pushboolean(L, 0);
      lua_pushstring(L, "Invalid PSK length (need 16 or 32 bytes)");
      return 2;
    }
    bool ok = the_mesh->setChannel(ch_idx, cd);
    if (ok) the_mesh->saveChannels();
    MESH_UNLOCK();
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
  }

  the_mesh->saveChannels();
  MESH_UNLOCK();
  lua_pushboolean(L, 1);
  return 1;
}

// Public chat (slot 0) delete / restore / state. Public is now a normal deletable
// channel; the deletion persists (channels-file marker) so it survives reboot, and
// it can be re-added with its well-known PSK.
static int lua_mesh_delete_public(lua_State *L) {
  MESH_LOCK();
  the_mesh->deletePublic();
  MESH_UNLOCK();
  lua_pushboolean(L, 1);
  return 1;
}
static int lua_mesh_restore_public(lua_State *L) {
  MESH_LOCK();
  the_mesh->restorePublic();
  MESH_UNLOCK();
  lua_pushboolean(L, 1);
  return 1;
}
static int lua_mesh_public_deleted(lua_State *L) {
  MESH_LOCK();
  bool d = the_mesh->isPublicDeleted();
  MESH_UNLOCK();
  lua_pushboolean(L, d ? 1 : 0);
  return 1;
}

// Send a message to a specific channel by index
// Usage: _mesh_send_channel(1, "Hello channel!")
static int lua_mesh_send_channel(lua_State *L) {
  int ch_idx = luaL_checkinteger(L, 1);
  const char *raw = luaL_checkstring(L, 2);
  char text[160];
  prepare_outgoing_text(raw, text, sizeof(text));

  MESH_LOCK();
  uint32_t timestamp = the_mesh->getRTCClock()->getCurrentTime();
  uint8_t tx_hash[MAX_HASH_SIZE];
  bool ok = the_mesh->sendAndPersistChannelMsg(ch_idx, timestamp, text, strlen(text), tx_hash);
  MESH_UNLOCK();

  lua_pushboolean(L, ok ? 1 : 0);
  if (ok) {
    char hex[MAX_HASH_SIZE * 2 + 1];
    mesh::Utils::toHex(hex, tx_hash, MAX_HASH_SIZE);
    lua_pushstring(L, hex);
  } else {
    lua_pushnil(L);
  }
  return 2;
}

// Remove a contact by name prefix
// Usage: _mesh_remove_contact("alice")
static int lua_mesh_remove_contact(lua_State *L) {
  const char *name_prefix = luaL_checkstring(L, 1);

  MESH_LOCK();
  ContactInfo *c = the_mesh->searchContactsByPrefix(name_prefix);
  if (!c) {
    MESH_UNLOCK();
    lua_pushboolean(L, 0);
    lua_pushstring(L, "Contact not found");
    return 2;
  }

  // Preserve in the archive before removal so it can be re-added later
  the_mesh->archiveContact(*c);
  bool ok = the_mesh->removeContact(*c);
  if (ok) the_mesh->saveContacts();
  MESH_UNLOCK();

  lua_pushboolean(L, ok ? 1 : 0);
  return 1;
}

// Clear all contacts
// Usage: _mesh_clear_contacts()
static int lua_mesh_clear_contacts(lua_State *L) {
  MESH_LOCK();
  the_mesh->clearContacts();
  the_mesh->saveContacts();
  MESH_UNLOCK();
  SLog.println("[MESH] All contacts cleared");
  lua_pushboolean(L, 1);
  return 1;
}

// Reset path to a contact (force flood routing next time)
// Usage: _mesh_reset_path("alice")
static int lua_mesh_reset_path(lua_State *L) {
  const char *name_prefix = luaL_checkstring(L, 1);

  MESH_LOCK();
  ContactInfo *c = the_mesh->searchContactsByPrefix(name_prefix);
  if (!c) {
    MESH_UNLOCK();
    lua_pushboolean(L, 0);
    lua_pushstring(L, "Contact not found");
    return 2;
  }

  the_mesh->resetPathTo(*c);
  the_mesh->saveOneContact(*c);
  MESH_UNLOCK();

  lua_pushboolean(L, 1);
  return 1;
}

// Set or clear the favourite flag (bit 0) on a contact
// Usage: _mesh_set_contact_favorite("alice", true)
static int lua_mesh_set_contact_favorite(lua_State *L) {
  const char *name_prefix = luaL_checkstring(L, 1);
  bool fav = lua_toboolean(L, 2);

  MESH_LOCK();
  ContactInfo *c = the_mesh->searchContactsByPrefix(name_prefix);
  if (!c) {
    MESH_UNLOCK();
    lua_pushboolean(L, 0);
    lua_pushstring(L, "Contact not found");
    return 2;
  }

  if (fav) c->flags |= 0x01;
  else     c->flags &= ~0x01;
  the_mesh->saveOneContact(*c);
  MESH_UNLOCK();

  lua_pushboolean(L, 1);
  return 1;
}

// Export a contact as hex biz card string
// Usage: local hex = _mesh_export_contact("alice")
static int lua_mesh_export_contact(lua_State *L) {
  const char *name_prefix = luaL_checkstring(L, 1);

  MESH_LOCK();
  ContactInfo *c = the_mesh->searchContactsByPrefix(name_prefix);
  if (!c) {
    MESH_UNLOCK();
    lua_pushnil(L);
    lua_pushstring(L, "Contact not found");
    return 2;
  }

  uint8_t buf[256];
  uint8_t len = the_mesh->exportContact(*c, buf);
  MESH_UNLOCK();
  if (len == 0) {
    lua_pushnil(L);
    lua_pushstring(L, "No advert data for contact");
    return 2;
  }

  char hex[513];
  mesh::Utils::toHex(hex, buf, len);

  // Return "meshcore://" prefixed hex string
  String card = "meshcore://" + String(hex);
  lua_pushstring(L, card.c_str());
  return 1;
}

// Import a contact from hex biz card string
// Usage: _mesh_import_contact("meshcore://abcdef...")
static int lua_mesh_import_contact(lua_State *L) {
  const char *card = luaL_checkstring(L, 1);

  MESH_LOCK();
  the_mesh->importCard(card);
  MESH_UNLOCK();
  lua_pushboolean(L, 1);
  return 1;
}

// Share a contact via zero-hop broadcast
// Usage: _mesh_share_contact("alice")
static int lua_mesh_share_contact(lua_State *L) {
  const char *name_prefix = luaL_checkstring(L, 1);

  MESH_LOCK();
  ContactInfo *c = the_mesh->searchContactsByPrefix(name_prefix);
  if (!c) {
    MESH_UNLOCK();
    lua_pushboolean(L, 0);
    lua_pushstring(L, "Contact not found");
    return 2;
  }

  bool ok = the_mesh->shareContactZeroHop(*c);
  MESH_UNLOCK();
  lua_pushboolean(L, ok ? 1 : 0);
  return 1;
}

// Login to a room server or repeater (sendLogin handles both types; rooms get
// their sync_since cursor in the request, repeaters just the password).
// Usage: local ok, route, est_timeout = _mesh_login("myroom", "password123")
// The result arrives later via messages.__dispatch_login (LOGIN_RESULT event);
// est_timeout (ms) is how long the UI should wait before declaring no response.
static int lua_mesh_login_room(lua_State *L) {
  const char *name_prefix = luaL_checkstring(L, 1);
  const char *password = luaL_checkstring(L, 2);

  MESH_LOCK();
  ContactInfo *c = the_mesh->searchContactsByPrefix(name_prefix);
  if (!c) {
    MESH_UNLOCK();
    lua_pushboolean(L, 0);
    lua_pushstring(L, "Contact not found");
    return 2;
  }

  uint32_t est_timeout = 0;
  int result = the_mesh->sendLogin(*c, password, est_timeout);
  if (result != MSG_SEND_FAILED) {
    memcpy(&the_mesh->pending_login_prefix, c->id.pub_key, 4);
  }
  MESH_UNLOCK();

  if (result == MSG_SEND_FAILED) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, "Login send failed");
    return 2;
  }

  lua_pushboolean(L, 1);
  lua_pushstring(L, result == MSG_SEND_SENT_DIRECT ? "direct" : "flood");
  lua_pushinteger(L, est_timeout);
  return 3;
}

// Drop the keep-alive connection to a logged-in server (local only — MeshCore
// has no logout packet; the server just stops hearing our keep-alives).
// Usage: _mesh_logout("myroom")
static int lua_mesh_logout(lua_State *L) {
  const char *name_prefix = luaL_checkstring(L, 1);

  MESH_LOCK();
  ContactInfo *c = the_mesh->searchContactsByPrefix(name_prefix);
  if (c) the_mesh->stopConnectionToContact(c->id.pub_key);
  MESH_UNLOCK();

  lua_pushboolean(L, c != nullptr);
  return 1;
}

// True while a keep-alive connection to this server is live (only servers
// that returned a keep-alive interval at login appear here).
// Usage: local up = _mesh_is_connected("myroom")
static int lua_mesh_is_connected(lua_State *L) {
  const char *name_prefix = luaL_checkstring(L, 1);

  MESH_LOCK();
  ContactInfo *c = the_mesh->searchContactsByPrefix(name_prefix);
  bool up = c && the_mesh->hasConnectionToContact(c->id.pub_key);
  MESH_UNLOCK();

  lua_pushboolean(L, up ? 1 : 0);
  return 1;
}

// Send a CLI command to a logged-in repeater (TXT_TYPE_CLI_DATA — no ack on
// the reply). The command is persisted into the repeater's thread first so
// the console history reads like a chat.
// Usage: local ok, route = _mesh_send_command("repeater1", "ver")
static int lua_mesh_send_command(lua_State *L) {
  const char *name_prefix = luaL_checkstring(L, 1);
  const char *raw = luaL_checkstring(L, 2);
  char text[160];
  prepare_outgoing_text(raw, text, sizeof(text));

  MESH_LOCK();
  ContactInfo *c = the_mesh->searchContactsByPrefix(name_prefix);
  if (!c) {
    MESH_UNLOCK();
    lua_pushboolean(L, 0);
    lua_pushstring(L, "Contact not found");
    return 2;
  }

  uint32_t est_timeout = 0;
  uint32_t timestamp = the_mesh->getRTCClock()->getCurrentTime();
  int result = the_mesh->sendCommandTracked(*c, timestamp, 0, text, est_timeout);
  char hash_hex[MAX_HASH_SIZE * 2 + 1] = {0};
  if (result != MSG_SEND_FAILED) {
    // Both routes set _last_tx_hash (sendFloodScoped / sendDirectTracked), so
    // the console echo gets the repeat-until-heard indicator like DMs do.
    the_mesh->appendDMMessage(c->name, the_mesh->_prefs.node_name, text, timestamp,
                              0, 0, 0, result == MSG_SEND_SENT_DIRECT,
                              0, nullptr, the_mesh->_last_tx_hash);
    the_mesh->preRegisterSentHash(the_mesh->_last_tx_hash, true, -1, c->name);
    mesh::Utils::toHex(hash_hex, the_mesh->_last_tx_hash, MAX_HASH_SIZE);
  }
  MESH_UNLOCK();

  if (result == MSG_SEND_FAILED) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, "Command send failed");
    return 2;
  }

  lua_pushboolean(L, 1);
  lua_pushstring(L, result == MSG_SEND_SENT_DIRECT ? "direct" : "flood");
  lua_pushstring(L, hash_hex);
  return 3;
}

// Send a request to a contact (e.g. get stats from repeater/room)
// Usage: local ok, route = _mesh_send_request("repeater1", 1)  -- 1=GET_STATUS
// A GET_STATUS response comes back decoded via messages.__dispatch_status.
static int lua_mesh_send_request(lua_State *L) {
  const char *name_prefix = luaL_checkstring(L, 1);
  int req_type = luaL_checkinteger(L, 2);

  MESH_LOCK();
  ContactInfo *c = the_mesh->searchContactsByPrefix(name_prefix);
  if (!c) {
    MESH_UNLOCK();
    lua_pushboolean(L, 0);
    lua_pushstring(L, "Contact not found");
    return 2;
  }

  uint32_t tag = 0;
  uint32_t est_timeout = 0;
  int result = the_mesh->sendRequest(*c, (uint8_t)req_type, tag, est_timeout);
  if (result != MSG_SEND_FAILED && req_type == REQ_TYPE_GET_STATUS) {
    memcpy(&the_mesh->pending_status_prefix, c->id.pub_key, 4);
  }
  MESH_UNLOCK();

  if (result == MSG_SEND_FAILED) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, "Request send failed");
    return 2;
  }

  lua_pushboolean(L, 1);
  lua_pushstring(L, result == MSG_SEND_SENT_DIRECT ? "direct" : "flood");
  return 2;
}

// Get last RX radio info (SNR/RSSI from most recent received packet)
// Usage: local info = _mesh_get_rx_info()
static int lua_mesh_get_rx_info(lua_State *L) {
  lua_newtable(L);

  MESH_LOCK();
  float snr  = the_mesh->last_rx_snr;
  float rssi = the_mesh->last_rx_rssi;
  MESH_UNLOCK();

  lua_pushnumber(L, snr);
  lua_setfield(L, -2, "snr");

  lua_pushnumber(L, rssi);
  lua_setfield(L, -2, "rssi");

  return 1;
}

// ── Raw packet capture (Packets monitor app) ─────────────────────────
// Arm/disarm the capture ring. Allocating on arm and freeing on disarm keeps
// the ~14KB out of PSRAM whenever nothing is watching.
// Usage: local ok = _mesh_pkt_capture(true)
static int lua_mesh_pkt_capture(lua_State *L) {
  bool on = lua_toboolean(L, 1);
  MESH_LOCK();
  bool ok = true;
  if (on) ok = rcap::start(); else rcap::stop();
  MESH_UNLOCK();
  lua_pushboolean(L, ok);
  return 1;
}

// Drain up to `max` captured frames, oldest first.
// Usage: local pkts, dropped = _mesh_pkt_poll(32)
// Each entry: { seq, ts, ms, dir, parsed, snr, rssi, score, len, hash, raw }
// dir is "rx" / "tx" / "txfail"; score is nil when the hook had none; hash is
// nil on an unparsed frame; raw is the full wire frame as a hex string.
static int lua_mesh_pkt_poll(lua_State *L) {
  int max = (int)luaL_optinteger(L, 1, 32);
  if (max < 1) max = 1;
  if (max > PKT_CAP_RING_SIZE) max = PKT_CAP_RING_SIZE;

  lua_newtable(L);
  int out = 0;

  MESH_LOCK();
  uint32_t dropped = rcap::take_dropped();

  PktCapture e;
  // Copy out of the ring before touching Lua: a lua_* call can longjmp on
  // OOM, and the radio core must never find a half-consumed ring.
  while (out < max && rcap::pop_oldest(&e)) {
    MESH_UNLOCK();

    char raw_hex[MAX_TRANS_UNIT * 2 + 1];
    mesh::Utils::toHex(raw_hex, e.raw, e.len);   // toHex null-terminates

    lua_newtable(L);
    lua_pushinteger(L, e.seq);           lua_setfield(L, -2, "seq");
    lua_pushinteger(L, e.ts);            lua_setfield(L, -2, "ts");
    lua_pushinteger(L, e.ms);            lua_setfield(L, -2, "ms");
    lua_pushstring(L, e.dir == PKT_CAP_DIR_TX ? "tx"
                    : e.dir == PKT_CAP_DIR_TX_FAIL ? "txfail" : "rx");
                                         lua_setfield(L, -2, "dir");
    lua_pushboolean(L, e.parsed);        lua_setfield(L, -2, "parsed");
    lua_pushinteger(L, e.len);           lua_setfield(L, -2, "len");
    lua_pushstring(L, raw_hex);          lua_setfield(L, -2, "raw");

    if (e.dir == PKT_CAP_DIR_RX) {
      lua_pushnumber(L, e.snr_q4 / 4.0f); lua_setfield(L, -2, "snr");
      lua_pushinteger(L, e.rssi);         lua_setfield(L, -2, "rssi");
    }
    if (e.score_q10 >= 0) {
      lua_pushnumber(L, e.score_q10 / 1000.0f); lua_setfield(L, -2, "score");
    }
    if (e.parsed) {
      char hash_hex[MAX_HASH_SIZE * 2 + 1];
      mesh::Utils::toHex(hash_hex, e.hash, MAX_HASH_SIZE);
      lua_pushstring(L, hash_hex);        lua_setfield(L, -2, "hash");
    }

    lua_rawseti(L, -2, ++out);
    MESH_LOCK();
  }
  MESH_UNLOCK();

  lua_pushinteger(L, dropped);
  return 2;
}

// Node positions per protocol, read from FILES — so the Map can draw both
// protocols at once, no matter which protocol is running this boot.
//   _map_nodes("meshcore") -> contacts.bin + contacts_arch.bin (143-byte
//     records, last record per pubkey wins; gps stored in 1e-6 degrees)
//   _map_nodes("<proto>")  -> <prefix>/<proto>/peers text records
//     (lat/lon in 1e-7 degrees, ptime = our clock when heard)
// Each entry: { name, lat, lon, heard } — only nodes WITH a position.
// (A named function, not a registration lambda: lua_register is a macro and
// the brace-initializers/multi-declarations here would split its arguments.)
static int lua_map_nodes(lua_State *L) {
  const char* proto = luaL_checkstring(L, 1);
  lua_newtable(L);
  fs::FS* fs = mstore::storage();
  if (!fs || !proto[0]) return 1;
  for (const char* c = proto; *c; c++) {
    if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '_'))
      return 1;   // path-safe ids only
  }
  bool is_sd = (fs != &LittleFS);
  int out = 0;

  if (strcmp(proto, "meshcore") == 0) {
    // pubkey32 | name32 | type1 flags1 plen1 | path64 | advert u32 |
    // lat i32 | lon i32  (see serialize_contact in punkmesh.cpp)
    struct Rec { uint64_t k; char name[32]; int32_t lat; int32_t lon; uint32_t ts; };
    const int MAXN = 400;
    Rec* recs = (Rec*)heap_caps_malloc(sizeof(Rec) * MAXN,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!recs) return 1;
    int n = 0;
    const char* files[2] = { "/contacts.bin", "/contacts_arch.bin" };
    if (is_sd) sd_spi_take();
    for (int fi = 0; fi < 2; fi++) {
      String path = mstore::prefix() + files[fi];
      File f = fs->open(path.c_str(), "r");
      if (!f) continue;
      uint8_t rec[143];
      int iter = 0;
      while (f.available() >= 143) {
        if (f.read(rec, 143) != 143) break;
        uint64_t k; memcpy(&k, rec, 8);
        int slot = -1;
        for (int i = 0; i < n; i++) if (recs[i].k == k) { slot = i; break; }
        if (slot < 0) { if (n >= MAXN) continue; slot = n++; recs[slot].k = k; }
        memcpy(recs[slot].name, rec + 32, 31); recs[slot].name[31] = 0;
        memcpy(&recs[slot].ts,  rec + 131, 4);
        memcpy(&recs[slot].lat, rec + 135, 4);
        memcpy(&recs[slot].lon, rec + 139, 4);
        if (is_sd && ++iter % 64 == 0) { sd_spi_release(); vTaskDelay(1); sd_spi_take(); }
      }
      f.close();
    }
    if (is_sd) sd_spi_release();
    for (int i = 0; i < n; i++) {
      if (recs[i].lat == 0 && recs[i].lon == 0) continue;
      lua_newtable(L);
      lua_pushstring(L, recs[i].name);       lua_setfield(L, -2, "name");
      lua_pushnumber(L, recs[i].lat / 1e6);  lua_setfield(L, -2, "lat");
      lua_pushnumber(L, recs[i].lon / 1e6);  lua_setfield(L, -2, "lon");
      lua_pushinteger(L, recs[i].ts);        lua_setfield(L, -2, "heard");
      lua_rawseti(L, -2, ++out);
    }
    heap_caps_free(recs);
    return 1;
  }

  // Protocol-module peers file (text records, "---" terminated).
  String path = mstore::prefix() + "/" + proto + "/peers";
  if (is_sd) sd_spi_take();
  File f = fs->open(path.c_str(), "r");
  if (f) {
    char name[40] = {0};
    char id[16] = {0};
    long lat = 0;
    long lon = 0;
    uint32_t heard = 0;
    uint32_t ptime = 0;
    uint32_t prec = 0;
    char line[96];
    while (f.available()) {
      int len = 0;
      while (f.available() && len < (int)sizeof(line) - 1) {
        char ch = f.read();
        if (ch == '\n' || ch == '\r') break;
        line[len++] = ch;
      }
      line[len] = '\0';
      if (len == 0) continue;
      if (!strcmp(line, "---")) {
        if (lat || lon) {
          lua_newtable(L);
          lua_pushstring(L, name[0] ? name : id);    lua_setfield(L, -2, "name");
          lua_pushnumber(L, lat / 1e7);              lua_setfield(L, -2, "lat");
          lua_pushnumber(L, lon / 1e7);              lua_setfield(L, -2, "lon");
          lua_pushinteger(L, ptime ? ptime : heard); lua_setfield(L, -2, "heard");
          if (prec >= 1 && prec <= 31) {
            lua_pushinteger(L, prec);                lua_setfield(L, -2, "prec");
          }
          lua_rawseti(L, -2, ++out);
        }
        name[0] = 0; id[0] = 0; lat = 0; lon = 0; heard = 0; ptime = 0; prec = 0;
        continue;
      }
      char* eq = strchr(line, '=');
      if (!eq) continue;
      *eq = '\0';
      const char* k = line;
      const char* v = eq + 1;
      if      (!strcmp(k, "num"))   snprintf(id, sizeof(id), "!%s", v);
      else if (!strcmp(k, "long"))  { strncpy(name, v, sizeof(name) - 1); name[sizeof(name) - 1] = 0; }
      else if (!strcmp(k, "lat"))   lat = strtol(v, nullptr, 10);
      else if (!strcmp(k, "lon"))   lon = strtol(v, nullptr, 10);
      else if (!strcmp(k, "heard")) heard = (uint32_t)strtoul(v, nullptr, 10);
      else if (!strcmp(k, "ptime")) ptime = (uint32_t)strtoul(v, nullptr, 10);
      else if (!strcmp(k, "prec"))  prec = (uint32_t)strtoul(v, nullptr, 10);
    }
    f.close();
  }
  if (is_sd) sd_spi_release();
  return 1;
}

// Usage: local enabled = _mesh_get_rx_boost()
static int lua_mesh_get_rx_boost(lua_State *L) {
  // MC-PKG divergence: the pref is the truth (start() and the setter below
  // keep the chip in step); the firmware version read the driver's tracking.
  lua_pushboolean(L, the_mesh && the_mesh->_prefs.rx_boost);
  return 1;
}

// Usage: _mesh_set_rx_boost(true)
// Applies the setting to the radio and persists it.
static int lua_mesh_set_rx_boost(lua_State *L) {
  bool en = lua_toboolean(L, 1);
  MCH->radio_set_rx_boost(en);   // MC-PKG divergence: host op (locks inside)

  the_mesh->_prefs.rx_boost = en ? 1 : 0;
  the_mesh->savePrefs();
  SLog.printf("[RADIO] RX Boost preference saved: %d\n", en ? 1 : 0);

  return 0;
}

// ── Auto-add contact config (matches the BLE companion model) ─────
// _mesh_get_autoadd() → selected_mode, chat, repeater, room, sensor.
//   selected_mode false = "auto-add all"; true = "auto-add selected" (the four
//   type booleans say which types are added). Type bits map to the MeshCore
//   spec: chat 0x02 / repeater 0x04 / room 0x08 / sensor 0x10.
static int lua_mesh_get_autoadd(lua_State *L) {
  MESH_LOCK();
  uint8_t mode = the_mesh->_prefs.manual_add_contacts;
  uint8_t cfg  = the_mesh->_prefs.autoadd_config;
  MESH_UNLOCK();
  lua_pushboolean(L, (mode & 0x01) != 0);
  lua_pushboolean(L, (cfg & 0x02) != 0);
  lua_pushboolean(L, (cfg & 0x04) != 0);
  lua_pushboolean(L, (cfg & 0x08) != 0);
  lua_pushboolean(L, (cfg & 0x10) != 0);
  return 5;
}

// _mesh_set_autoadd(selected_mode, chat, repeater, room, sensor)
static int lua_mesh_set_autoadd(lua_State *L) {
  uint8_t mode = lua_toboolean(L, 1) ? 0x01 : 0x00;
  uint8_t cfg = 0;
  if (lua_toboolean(L, 2)) cfg |= 0x02;
  if (lua_toboolean(L, 3)) cfg |= 0x04;
  if (lua_toboolean(L, 4)) cfg |= 0x08;
  if (lua_toboolean(L, 5)) cfg |= 0x10;
  MESH_LOCK();
  the_mesh->_prefs.manual_add_contacts = mode;
  the_mesh->_prefs.autoadd_config = cfg;
  the_mesh->savePrefs();
  MESH_UNLOCK();
  SLog.printf("[MESH] autoadd mode=%s cfg=0x%02X\n", mode ? "selected" : "all", cfg);
  return 0;
}

// ── Message repeat settings bridge ───────────────────────────────

static int lua_mesh_get_msg_repeat(lua_State *L) {
  lua_newtable(L);
  lua_pushboolean(L, the_mesh->_prefs.msg_repeat_enabled);
  lua_setfield(L, -2, "enabled");
  lua_pushinteger(L, the_mesh->_prefs.msg_repeat_max);
  lua_setfield(L, -2, "max_repeats");
  lua_pushinteger(L, the_mesh->_prefs.msg_repeat_interval_secs);
  lua_setfield(L, -2, "interval");
  return 1;
}

static int lua_mesh_set_msg_repeat(lua_State *L) {
  bool en = lua_toboolean(L, 1);
  int max_rep = luaL_optinteger(L, 2, 3);
  int interval = luaL_optinteger(L, 3, 30);
  if (max_rep < 1) max_rep = 1;
  if (max_rep > 10) max_rep = 10;
  if (interval < 5) interval = 5;
  if (interval > 60) interval = 60;

  the_mesh->_prefs.msg_repeat_enabled = en ? 1 : 0;
  the_mesh->_prefs.msg_repeat_max = (uint8_t)max_rep;
  the_mesh->_prefs.msg_repeat_interval_secs = (uint8_t)interval;
  the_mesh->savePrefs();
  return 0;
}

static int lua_mesh_get_repeat_status(lua_State *L) {
  const char *hex = luaL_checkstring(L, 1);
  uint8_t hash[MAX_HASH_SIZE];
  memset(hash, 0, MAX_HASH_SIZE);
  size_t hlen = strlen(hex);
  for (size_t i = 0; i < hlen / 2 && i < MAX_HASH_SIZE; i++) {
    char hb[3] = { hex[i*2], hex[i*2+1], 0 };
    hash[i] = (uint8_t)strtoul(hb, NULL, 16);
  }
  int remaining = 0, total = 0;
  MESH_LOCK();
  int status = the_mesh->getRepeatStatus(hash, &remaining, &total);
  MESH_UNLOCK();
  // (status, remaining, total): remaining = re-airs still to go, total =
  // configured max; both 0 unless status==1 (actively repeating). The Messenger
  // renders "repeating N/M" from these (N = total - remaining + 1).
  lua_pushinteger(L, status);
  lua_pushinteger(L, remaining);
  lua_pushinteger(L, total);
  return 3;
}

// ── Persistent message history bridge ────────────────────────────

// Read stored messages for a channel slot.
// Usage: local msgs = _mesh_get_channel_messages(0 [, max_records])
// max_records omitted/0 = whole log; N = only the newest N records.
// Returns array of { from, peer, text, timestamp, hops, snr, rssi, direct, is_dm, channel_idx }
// No MESH_LOCK here: the pusher locks internally just for its channel-name
// snapshot; the file read + Lua pushes must run unlocked (see punkmesh.h).
static int lua_mesh_get_channel_messages(lua_State *L) {
  int ch_idx = luaL_checkinteger(L, 1);
  int max_records = (int)luaL_optinteger(L, 2, 0);
  return the_mesh->pushChannelMessagesToLua(L, ch_idx, max_records);
}

// _mesh_routing_query(sender_or_nil, since_ts, until_ts) -> array of
// { from, timestamp, lat, lon, path } from the routing store. sender nil/"" = all.
// No MESH_LOCK: only touches the routing files (sd_spi serialized inside).
static int lua_mesh_routing_query(lua_State *L) {
  const char *sender = lua_isnoneornil(L, 1) ? nullptr : luaL_checkstring(L, 1);
  uint32_t since = (uint32_t)luaL_optinteger(L, 2, 0);
  uint32_t until = (uint32_t)luaL_optinteger(L, 3, 0);
  return the_mesh->pushRoutingQuery(L, sender, since, until);
}

// _mesh_routing_senders(query_or_nil, max) -> array of distinct sender names from
// the routing index matching the (case-insensitive substring) query. Streams the
// .idx files in C — no message bodies loaded into Lua.
static int lua_mesh_routing_senders(lua_State *L) {
  const char *query = lua_isnoneornil(L, 1) ? nullptr : luaL_checkstring(L, 1);
  int max = (int)luaL_optinteger(L, 2, 64);
  return the_mesh->pushRoutingSenders(L, query, max);
}

// Read stored messages for a DM thread.
// Usage: local msgs = _mesh_get_dm_messages("alice" [, max_records])
// max_records omitted/0 = whole log; N = only the newest N records.
// No MESH_LOCK: the DM pusher touches no mesh state and the read + Lua
// pushes must run unlocked (see punkmesh.h).
static int lua_mesh_get_dm_messages(lua_State *L) {
  const char *peer = luaL_checkstring(L, 1);
  int max_records = (int)luaL_optinteger(L, 2, 0);
  return the_mesh->pushDMMessagesToLua(L, peer, max_records);
}

// Chat pager for the Messenger's windowed scroll (see punkmesh.h).
// _mesh_chat_page_channel(idx, mode, cursor, count) -> { list = {...}, size = N }
// _mesh_chat_page_dm(peer, mode, cursor, count)     -> { list = {...}, size = N }
// mode: 0 tail (newest count) / 1 older (before byte cursor) / 2 newer (from cursor).
// No MESH_LOCK here: the pushers lock internally only for the channel-name snapshot.
static int lua_mesh_chat_page_channel(lua_State *L) {
  int idx = luaL_checkinteger(L, 1);
  int mode = (int)luaL_optinteger(L, 2, 0);
  uint32_t cursor = (uint32_t)luaL_optinteger(L, 3, 0);
  int count = (int)luaL_optinteger(L, 4, 20);
  return the_mesh->pushChatPageChannel(L, idx, mode, cursor, count);
}

static int lua_mesh_chat_page_dm(lua_State *L) {
  const char *peer = luaL_checkstring(L, 1);
  int mode = (int)luaL_optinteger(L, 2, 0);
  uint32_t cursor = (uint32_t)luaL_optinteger(L, 3, 0);
  int count = (int)luaL_optinteger(L, 4, 20);
  return the_mesh->pushChatPageDM(L, peer, mode, cursor, count);
}

// Enumerate all DM thread peer names that have stored messages.
// Usage: local names = _mesh_get_dm_threads()
static int lua_mesh_get_dm_threads(lua_State *L) {
  MESH_LOCK();
  int n = the_mesh->pushDMThreadNamesToLua(L);
  MESH_UNLOCK();
  return n;
}

// One summary entry per stored conversation for the Messenger inbox:
// { kind="channel", idx, name, count, last } / { kind="dm", name, count, last }.
// Usage: local sums = _mesh_get_msg_summaries()
// No MESH_LOCK here — pushMsgSummariesToLua takes it internally just for the
// channel-table snapshot and does all file I/O outside it.
static int lua_mesh_get_msg_summaries(lua_State *L) {
  return the_mesh->pushMsgSummariesToLua(L);
}

// Per-channel notification modes (the Settings surface; RX consults the same
// table inside punkmesh.cpp). Overrides the firmware bindings, whose
// no-the_mesh fallback reads MENTION and drops writes.
static int lua_notify_channel_get(lua_State *L) {
  const char* name = luaL_checkstring(L, 1);
  MESH_LOCK();
  uint8_t mode = the_mesh->getChannelNotifyMode(name);
  MESH_UNLOCK();
  lua_pushinteger(L, mode);
  return 1;
}
static int lua_notify_channel_set(lua_State *L) {
  const char* name = luaL_checkstring(L, 1);
  int mode = (int)luaL_checkinteger(L, 2);
  if (mode < 0 || mode > NOTIFY_CHAN_ALL) mode = NOTIFY_CHAN_MENTION;
  MESH_LOCK();
  the_mesh->setChannelNotifyMode(name, (uint8_t)mode);
  MESH_UNLOCK();
  lua_pushinteger(L, mode);
  return 1;
}

// ── Unread counters (C-side, survive Lua teardown during ELF runs) ──
// The mesh task bumps these at RX (punkmesh.cpp); Lua only reads/clears.
// messages.lua wraps them so the topbar/Messenger API is unchanged.

// Usage: local n = _mesh_unread_total()
static int lua_mesh_unread_total(lua_State *L) {
  // mstore-direct (not through PunkMesh): the topbar polls this under every
  // protocol, including ones where the_mesh doesn't exist.
  MESH_LOCK();
  uint32_t n = mstore::unread_total();
  MESH_UNLOCK();
  lua_pushinteger(L, (lua_Integer)n);
  return 1;
}

// Usage: local n = _mesh_unread_channel(idx)
static int lua_mesh_unread_channel(lua_State *L) {
  int idx = luaL_checkinteger(L, 1);
  MESH_LOCK();
  uint16_t n = the_mesh->unreadChannel(idx);
  MESH_UNLOCK();
  lua_pushinteger(L, n);
  return 1;
}

// Usage: local n = _mesh_unread_dm(name)
static int lua_mesh_unread_dm(lua_State *L) {
  const char *name = luaL_checkstring(L, 1);
  MESH_LOCK();
  uint16_t n = the_mesh->unreadDM(name);
  MESH_UNLOCK();
  lua_pushinteger(L, n);
  return 1;
}

// Usage: _mesh_unread_clear_channel(idx)
static int lua_mesh_unread_clear_channel(lua_State *L) {
  int idx = luaL_checkinteger(L, 1);
  MESH_LOCK();
  the_mesh->unreadClearChannel(idx);
  MESH_UNLOCK();
  return 0;
}

// Usage: _mesh_unread_clear_dm(name)
static int lua_mesh_unread_clear_dm(lua_State *L) {
  const char *name = luaL_checkstring(L, 1);
  MESH_LOCK();
  the_mesh->unreadClearDM(name);
  MESH_UNLOCK();
  return 0;
}

// ── main.cpp 4338-4345: retention cap ──────────────────────────────────────
static int lua_mesh_set_max_messages(lua_State *L) {
  int n = luaL_checkinteger(L, 1);
  MESH_LOCK();
  mstore::set_max_messages(n);
  MESH_UNLOCK();
  lua_pushboolean(L, 1);
  return 1;
}

// Registration + drain (same TU: the bodies above are file-static).
#include "mclua_reg.inc"

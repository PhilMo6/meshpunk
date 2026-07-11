#include "ble_msg_sync.h"

#if BLE_COMPANION_ENABLED

#include "punkmesh.h"
#include "meshpunk_sync.h"
#include "usb_manager.h"   // UsbFlashGuardIf — pause USB audio around flash writes
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <helpers/BaseSerialInterface.h>
#include <helpers/TxtDataHelpers.h>

static_assert(BLE_SYNC_FRAME_MAX == MAX_FRAME_SIZE,
              "BleSyncFrame sized against the BLE frame limit");

// sd_spi_take() is inline in meshpunk_sync.h; the release half lives in main.cpp.
extern void sd_spi_release();

// Sidecar layout (v1): 'B' 'S' ver count, then per entry:
//   u8 name_len, name bytes, u32 offset, u32 last_ts, u8 flags(bit0=rescan)
static const uint8_t SIDECAR_MAGIC0 = 'B';
static const uint8_t SIDECAR_MAGIC1 = 'S';
static const uint8_t SIDECAR_VER    = 1;

static const char* basename_of(const char* path) {
  const char* slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

// Wire codes for the sync frames this engine emits (protocol constants,
// mirrored from ble_companion.h to avoid a circular include).
#define SYNC_RESP_CONTACT_MSG_V3  16
#define SYNC_RESP_CHANNEL_MSG_V3  17

// Build the V3 wire frame for one stored record. Returns 0 to skip (our own
// sends, corrupt-future timestamps, DMs without a sender key) — skipped
// records still advance the ledger offset via the batch tail.
static int buildSyncFrameFor(PunkMesh& mesh, const StoredMsg& m, uint8_t* frame) {
  if (strcmp(m.from, mesh._prefs.node_name) == 0) return 0;

  // Skip absurdly future timestamps (corrupt data). Only when the RTC has
  // been set (now > ~1 day past epoch).
  uint32_t now = mesh.getRTCClock()->getCurrentTime();
  if (now > 86400 && m.timestamp > now + 86400) return 0;

  int i = 0;
  bool is_dm = (m.flags & 0x02) != 0;
  int8_t snr4 = (int8_t)(m.snr * 4);

  if (is_dm) {
    if (!m.has_pub_key) return 0;
    frame[i++] = SYNC_RESP_CONTACT_MSG_V3;
    frame[i++] = (uint8_t)snr4;
    frame[i++] = 0;
    frame[i++] = 0;
    memcpy(&frame[i], m.sender_pub_key, 6); i += 6;
    frame[i++] = 0xFF;
    frame[i++] = TXT_TYPE_PLAIN;
    memcpy(&frame[i], &m.timestamp, 4); i += 4;
  } else {
    frame[i++] = SYNC_RESP_CHANNEL_MSG_V3;
    frame[i++] = (uint8_t)snr4;
    frame[i++] = 0;
    frame[i++] = 0;
    frame[i++] = (uint8_t)m.channel_idx;
    frame[i++] = 0xFF;
    frame[i++] = TXT_TYPE_PLAIN;
    memcpy(&frame[i], &m.timestamp, 4); i += 4;
  }

  if (!is_dm && m.from[0]) {
    int nlen = strlen(m.from);
    if (nlen + 2 > MAX_FRAME_SIZE - i) return 0;
    memcpy(&frame[i], m.from, nlen); i += nlen;
    frame[i++] = ':'; frame[i++] = ' ';
  }

  int tlen = strlen(m.text);
  if (tlen > MAX_FRAME_SIZE - i) tlen = MAX_FRAME_SIZE - i;
  memcpy(&frame[i], m.text, tlen); i += tlen;
  return i;
}

// ── Lifecycle ────────────────────────────────────────────────────

void BleMsgSync::begin(PunkMesh* mesh) {
  _mesh = mesh;

  _ledger   = (LedgerEntry*)heap_caps_calloc(BLE_SYNC_MAX_FILES, sizeof(LedgerEntry), MALLOC_CAP_SPIRAM);
  _frames   = (BleSyncFrame*)heap_caps_malloc(BLE_SYNC_BATCH * sizeof(BleSyncFrame), MALLOC_CAP_SPIRAM);
  _records  = (StoredMsg*)heap_caps_malloc(BLE_SYNC_BATCH * sizeof(StoredMsg), MALLOC_CAP_SPIRAM);
  _rec_ends = (uint32_t*)heap_caps_malloc(BLE_SYNC_BATCH * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
  if (!_ledger || !_frames || !_records || !_rec_ends) {
    SLog.println("[BLE SYNC] FATAL: buffer alloc failed; sync disabled");
    end();
    return;
  }

  _ledger_count = 0;
  _frame_count = _frame_idx = 0;
  _cur = -1;
  _tickle = false;
  _sidecar_dirty = false;

  loadSidecar();
  _state = ST_RECONCILE;   // always reconcile once at startup
}

void BleMsgSync::end() {
  if (_ledger)   { heap_caps_free(_ledger);   _ledger = nullptr; }
  if (_frames)   { heap_caps_free(_frames);   _frames = nullptr; }
  if (_records)  { heap_caps_free(_records);  _records = nullptr; }
  if (_rec_ends) { heap_caps_free(_rec_ends); _rec_ends = nullptr; }
  _ledger_count = 0;
  _frame_count = _frame_idx = 0;
  _state = ST_INIT;
}

// ── Ledger primitives ────────────────────────────────────────────

BleMsgSync::LedgerEntry* BleMsgSync::findEntry(const char* name) {
  for (int i = 0; i < _ledger_count; i++) {
    if (strcmp(_ledger[i].name, name) == 0) return &_ledger[i];
  }
  return nullptr;
}

BleMsgSync::LedgerEntry* BleMsgSync::addEntry(const char* name) {
  if (!_ledger || _ledger_count >= BLE_SYNC_MAX_FILES) return nullptr;
  LedgerEntry* e = &_ledger[_ledger_count++];
  memset(e, 0, sizeof(*e));
  strncpy(e->name, name, sizeof(e->name) - 1);
  return e;
}

void BleMsgSync::commit(LedgerEntry* e, uint32_t offset, uint32_t ts) {
  e->offset = offset;
  if (ts > e->last_ts) e->last_ts = ts;
  _sidecar_dirty = true;
  _sidecar_touch_ms = millis();
}

void BleMsgSync::commitBatchTail() {
  if (_cur < 0) return;
  commit(&_ledger[_cur], _batch_tail_offset, _batch_max_ts);
}

String BleMsgSync::sidecarPath() {
  return _mesh->messagesDirPath() + "/ble_sync.idx";
}

String BleMsgSync::fullPath(const char* name) {
  return _mesh->messagesDirPath() + "/" + name;
}

// ── RX glue ──────────────────────────────────────────────────────

void BleMsgSync::markDirty(const char* path) {
  if (!_ledger) return;
  const char* base = basename_of(path);
  LedgerEntry* e = findEntry(base);
  if (!e) e = addEntry(base);       // new conversation: offset 0 = all new
  if (!e) {
    SLog.printf("[BLE SYNC] ledger full, cannot track %s\n", base);
    return;
  }
  e->dirty = true;
}

// ── Serving ──────────────────────────────────────────────────────

int BleMsgSync::nextFrame(uint8_t* out) {
  if (!hasFrame()) return 0;
  // The app's pulls are strictly sequential: asking for frame N implies
  // frame N-1 arrived — commit it.
  if (_frame_idx > 0 && _cur >= 0) {
    commit(&_ledger[_cur], _frames[_frame_idx - 1].end_offset, _frames[_frame_idx - 1].ts);
  }
  BleSyncFrame& f = _frames[_frame_idx++];
  memcpy(out, f.data, f.len);
  return f.len;
}

int BleMsgSync::pickDirty() {
  for (int i = 0; i < _ledger_count; i++) {
    if (_ledger[i].dirty) return i;
  }
  return -1;
}

void BleMsgSync::tryLoadMore() {
  if (!_ledger || hasFrame()) return;

  // A fully-served previous batch is confirmed by this very request
  // (sequential pulls) — commit through its tail before reading on.
  if (_cur >= 0 && _frame_count > 0 && _frame_idx >= _frame_count) {
    commitBatchTail();
    _frame_count = _frame_idx = 0;
  }

  // Bounded walk: each dirty file is visited at most once per call.
  for (int guard = 0; guard <= BLE_SYNC_MAX_FILES; guard++) {
    if (_cur >= 0) {
      if (!_cur_exhausted) {
        if (loadBatch()) return;      // got frames to serve
      }
      if (_cur_exhausted) {
        // File fully consumed: commit through its tail, clear flags.
        commitBatchTail();
        _ledger[_cur].dirty = false;
        _ledger[_cur].rescan = false;
        _cur = -1;
      }
      // !exhausted but zero frames (all records filtered): loop back into
      // loadBatch to keep advancing through the file.
      if (_cur >= 0) continue;
    }

    int idx = pickDirty();
    if (idx < 0) return;              // nothing to sync — zero file opens
    _cur = idx;
    _cur_next_offset  = _ledger[idx].offset;
    _batch_tail_offset = _ledger[idx].offset;
    _batch_max_ts      = _ledger[idx].last_ts;
    _cur_exhausted = false;
  }
}

bool BleMsgSync::loadBatch() {
  LedgerEntry* e = &_ledger[_cur];
  uint32_t min_ts = e->rescan ? e->last_ts : 0;
  uint32_t next = _cur_next_offset, fsize = 0;

  int n = _mesh->readStoredMsgsFrom(fullPath(e->name).c_str(), _cur_next_offset,
                                    min_ts, _records, _rec_ends, BLE_SYNC_BATCH,
                                    &next, &fsize);

  if (_cur_next_offset > fsize) {
    // File shrank under us (retention compaction): restart with ts filter.
    SLog.printf("[BLE SYNC] %s compacted (off=%u > size=%u), rescanning\n",
                e->name, _cur_next_offset, fsize);
    e->offset = 0;
    e->rescan = (e->last_ts > 0);
    _cur_next_offset = 0;
    _batch_tail_offset = 0;
    _sidecar_dirty = true;
    _sidecar_touch_ms = millis();
    return false;                     // caller loops back in
  }

  _frame_count = _frame_idx = 0;
  for (int i = 0; i < n; i++) {
    int flen = buildSyncFrameFor(*_mesh, _records[i], _frames[_frame_count].data);
    if (_records[i].timestamp > _batch_max_ts) _batch_max_ts = _records[i].timestamp;
    if (flen > 0) {
      _frames[_frame_count].len = (uint8_t)flen;
      _frames[_frame_count].end_offset = _rec_ends[i];
      _frames[_frame_count].ts = _records[i].timestamp;
      _frame_count++;
    }
  }

  bool progressed = (next != _cur_next_offset);
  uint32_t start = _cur_next_offset;
  _batch_tail_offset = next;
  _cur_next_offset = next;
  _cur_exhausted = (next >= fsize) || !progressed;

  // Zero-record batches (rescan filtering) stay silent — one line per batch
  // that actually serves messages.
  if (n > 0) {
    SLog.printf("[BLE SYNC] %s off=%u->%u size=%u records=%d frames=%d\n",
                e->name, start, next, fsize, n, _frame_count);
  }
  return _frame_count > 0;
}

void BleMsgSync::onSyncSessionEnd() {
  if (_cur >= 0 && !hasFrame()) {
    commitBatchTail();
    if (_cur_exhausted) {
      _ledger[_cur].dirty = false;
      _ledger[_cur].rescan = false;
    }
    _cur = -1;
  }
}

void BleMsgSync::onAppStart() {
  // New app session: drop unserved frames. Offsets were only committed for
  // confirmed frames, so the un-acked tail re-serves (duplicates over loss).
  _frame_count = _frame_idx = 0;
  if (_cur >= 0) {
    _ledger[_cur].dirty = true;
    _cur = -1;
  }
}

// ── Background work ──────────────────────────────────────────────

bool BleMsgSync::step() {
  if (!_ledger) return false;
  if (_state == ST_RECONCILE) {
    reconcile();
    return true;
  }
  if (_sidecar_dirty && millis() - _sidecar_touch_ms > BLE_SYNC_PERSIST_MS) {
    persistSidecar();
    return true;
  }
  return false;
}

bool BleMsgSync::takeTickle() {
  bool t = _tickle;
  _tickle = false;
  return t;
}

void BleMsgSync::reconcile() {
  // Entry removal compacts the ledger array, which would dangle _cur —
  // defer until the in-flight file finishes (step() retries us).
  if (_cur >= 0) return;

  // Metadata-only pass: directory sizes vs ledger offsets. No content reads.
  PunkMesh::MsgFileInfo* infos = (PunkMesh::MsgFileInfo*)heap_caps_malloc(
      BLE_SYNC_MAX_FILES * sizeof(PunkMesh::MsgFileInfo), MALLOC_CAP_SPIRAM);
  if (!infos) { _state = ST_READY; return; }

  int n = _mesh->enumerateMessageFiles(infos, BLE_SYNC_MAX_FILES);

  for (int i = 0; i < _ledger_count; i++) _ledger[i].seen = false;

  int dirty_count = 0;
  for (int i = 0; i < n; i++) {
    const char* base = basename_of(infos[i].path);
    LedgerEntry* e = findEntry(base);
    if (!e) {
      e = addEntry(base);
      if (!e) continue;               // ledger full
      if (_have_legacy) {
        // Migrating from the old global watermark: everything up to the
        // watermark was synced; rescan with the ts filter finds the rest.
        e->offset = 0;
        e->last_ts = _legacy_ts;
        e->rescan = (_legacy_ts > 0);
      } else if (_seed_mode) {
        // Fresh install, no prior state: serve only what arrives from now on.
        e->offset = infos[i].size;
      }
      // else: file appeared while running — offset 0, all of it is new.
    }
    e->seen = true;

    if (infos[i].size < e->offset) {          // compacted while we were away
      e->offset = 0;
      e->rescan = (e->last_ts > 0);
      e->dirty = true;
    } else if (infos[i].size > e->offset) {
      e->dirty = true;
    } else {
      e->dirty = false;
    }
    if (e->dirty) dirty_count++;
  }
  heap_caps_free(infos);

  // Drop ledger entries whose files are gone (retention deleted them).
  int w = 0;
  for (int i = 0; i < _ledger_count; i++) {
    if (_ledger[i].seen) {
      if (w != i) _ledger[w] = _ledger[i];
      w++;
    }
  }
  if (w != _ledger_count) { _ledger_count = w; _sidecar_dirty = true; _sidecar_touch_ms = millis(); }

  if (_have_legacy || _seed_mode) {
    persistSidecar();
    if (_have_legacy) {
      bool is_sd = (_mesh->_storage != &LittleFS);
      if (is_sd) sd_spi_take();
      _mesh->_storage->remove((_mesh->messagesDirPath() + "/ble_sync_ts").c_str());
      if (is_sd) sd_spi_release();
      SLog.printf("[BLE SYNC] migrated legacy watermark=%u\n", _legacy_ts);
    }
    _have_legacy = false;
    _seed_mode = false;
  }

  if (dirty_count > 0) _tickle = true;
  _state = ST_READY;
  SLog.printf("[BLE SYNC] reconcile: files=%d ledger=%d dirty=%d\n",
              n, _ledger_count, dirty_count);
}

// ── Sidecar persistence ──────────────────────────────────────────

void BleMsgSync::loadSidecar() {
  _have_legacy = false;
  _seed_mode = false;
  if (!_mesh->_storage) return;

  bool is_sd = (_mesh->_storage != &LittleFS);
  if (is_sd) sd_spi_take();

  File f = _mesh->_storage->open(sidecarPath().c_str(), "r");
  if (f) {
    uint8_t hdr[4];
    bool ok = (f.read(hdr, 4) == 4 && hdr[0] == SIDECAR_MAGIC0 &&
               hdr[1] == SIDECAR_MAGIC1 && hdr[2] == SIDECAR_VER);
    int count = ok ? hdr[3] : 0;
    if (count > BLE_SYNC_MAX_FILES) { ok = false; count = 0; }
    for (int i = 0; ok && i < count; i++) {
      uint8_t nlen;
      if (f.read(&nlen, 1) != 1 || nlen == 0 || nlen >= BLE_SYNC_NAME_LEN) { ok = false; break; }
      LedgerEntry* e = &_ledger[_ledger_count];
      memset(e, 0, sizeof(*e));
      uint8_t flags;
      if (f.read((uint8_t*)e->name, nlen) != nlen ||
          f.read((uint8_t*)&e->offset, 4) != 4 ||
          f.read((uint8_t*)&e->last_ts, 4) != 4 ||
          f.read(&flags, 1) != 1) { ok = false; break; }
      e->rescan = (flags & 0x01) != 0;
      _ledger_count++;
    }
    if (!ok) {
      SLog.println("[BLE SYNC] sidecar corrupt, discarding");
      _ledger_count = 0;
    }
    f.close();
  }

  if (_ledger_count == 0) {
    // No (usable) sidecar: try the legacy single-watermark file once.
    File lf = _mesh->_storage->open((_mesh->messagesDirPath() + "/ble_sync_ts").c_str(), "r");
    if (lf) {
      uint32_t ts = 0;
      if (lf.read((uint8_t*)&ts, 4) == 4 && ts > 0) {
        _legacy_ts = ts;
        _have_legacy = true;
      }
      lf.close();
    }
    if (!_have_legacy) _seed_mode = true;   // fresh install: sync from now on
  }

  if (is_sd) sd_spi_release();
  SLog.printf("[BLE SYNC] ledger loaded: %d entries%s%s\n", _ledger_count,
              _have_legacy ? " (legacy migration pending)" : "",
              _seed_mode ? " (fresh, seeding to current sizes)" : "");
}

void BleMsgSync::persistSidecar() {
  if (!_mesh->_storage) return;

  bool is_sd = (_mesh->_storage != &LittleFS);
  UsbFlashGuardIf _g(!is_sd);   // LittleFS backend: internal-flash write
  if (is_sd) sd_spi_take();

  File f = _mesh->_storage->open(sidecarPath().c_str(), "w", true);
  if (f) {
    uint8_t hdr[4] = { SIDECAR_MAGIC0, SIDECAR_MAGIC1, SIDECAR_VER, (uint8_t)_ledger_count };
    f.write(hdr, 4);
    for (int i = 0; i < _ledger_count; i++) {
      LedgerEntry* e = &_ledger[i];
      uint8_t nlen = (uint8_t)strlen(e->name);
      uint8_t flags = e->rescan ? 0x01 : 0x00;
      f.write(&nlen, 1);
      f.write((const uint8_t*)e->name, nlen);
      f.write((const uint8_t*)&e->offset, 4);
      f.write((const uint8_t*)&e->last_ts, 4);
      f.write(&flags, 1);
    }
    f.close();
  }
  if (is_sd) sd_spi_release();

  _sidecar_dirty = false;
  SLog.printf("[BLE SYNC] ledger saved (%d entries)\n", _ledger_count);
}

#endif // BLE_COMPANION_ENABLED

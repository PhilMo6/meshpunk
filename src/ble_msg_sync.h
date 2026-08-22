#pragma once

#if BLE_COMPANION_ENABLED

#include <Arduino.h>

// ── BLE message-sync engine ──────────────────────────────────────
//
// Replaces the global-watermark + full-store-scan model with a per-file
// ledger so sync work is proportional to NEW data, never store size:
//
//   * Ledger: one entry per message log {basename, synced_offset}.
//     Logs are append-only, so "new records" always live at >= synced_offset
//     and a sync is seek(offset) + bounded forward read — never a whole-file
//     parse. Persisted to <messages>/ble_sync.idx (RAM authoritative, lazy
//     write).
//
//   * Dirty flags: the RX path marks exactly the file it appended
//     (markDirty). An idle CMD_SYNC_NEXT_MESSAGE answers NO_MORE with zero
//     file opens. Reconciliation (boot / sidecar loss) compares directory
//     sizes against ledger offsets — metadata only, no content reads.
//
//   * One serving path: whenever a file is picked, PunkMesh::_ble_sync_max_
//     per_channel (0 = no limit) moves the offset to the newest N records.
//     A compaction (file shrank) leaves the stored offset pointing at nothing,
//     so it resets to 0 and the cap trims it forward again on the next pick —
//     at most N already-delivered records repeat.
//
//   * Delivery-safe commits: serving frame N commits frame N-1's offset (the
//     app's strictly-sequential pulls imply receipt); crash or disconnect
//     re-serves at most the tail of one batch. Duplicates over loss.
//
// Threading: every method runs on the Core-1 mesh task (companion loop, cmd
// handlers, RX push glue) — no internal locking needed. File I/O bounds each
// call to one batch of one file, serialized against other SD users by the
// sd_spi lock inside the PunkMesh read helpers.

class PunkMesh;
struct StoredMsg;

#define BLE_SYNC_MAX_FILES   40
#define BLE_SYNC_NAME_LEN    48
#define BLE_SYNC_BATCH       64     // frames per load unit (bounds handler time)
#define BLE_SYNC_FRAME_MAX   172    // == MAX_FRAME_SIZE (static_assert in .cpp)
#define BLE_SYNC_PERSIST_MS  5000   // lazy sidecar write delay

struct BleSyncFrame {
  uint8_t  len;
  uint8_t  data[BLE_SYNC_FRAME_MAX];
  uint32_t end_offset;   // file offset just past this frame's record
};

class BleMsgSync {
public:
  // Allocates PSRAM buffers, loads the sidecar (or arms migration).
  // Call once from the companion handler ctor; end() from the dtor.
  void begin(PunkMesh* mesh);
  void end();

  // RX glue (mesh task): a message was just appended to this log file.
  void markDirty(const char* path);

  // CMD_SYNC_NEXT_MESSAGE serving. tryLoadMore() is bounded: at most one
  // file open + one batch read per call.
  bool hasFrame() const { return _frames && _frame_idx < _frame_count; }
  int  nextFrame(uint8_t* out);     // copies frame data, returns len
  void tryLoadMore();
  void onSyncSessionEnd();          // about to send NO_MORE: final commit
  void onAppStart();                // new app session: abort in-flight batch

  // Companion-loop idle work: one bounded unit (reconcile step or sidecar
  // persist). Returns true if it did something.
  bool step();

  // True once when background work discovered unsynced data — caller sends
  // PUSH_CODE_MSG_WAITING so the app knows to pull.
  bool takeTickle();

private:
  enum State : uint8_t { ST_INIT, ST_RECONCILE, ST_READY };

  struct LedgerEntry {
    char     name[BLE_SYNC_NAME_LEN];  // basename, e.g. "dm_Alice.log"
    uint32_t offset;                   // synced-through byte offset
    bool     dirty;                    // has (or may have) unsynced records
    bool     seen;                     // scratch flag for reconciliation
  };

  LedgerEntry* findEntry(const char* name);
  LedgerEntry* addEntry(const char* name);
  void   commit(LedgerEntry* e, uint32_t offset);
  void   commitBatchTail();
  bool   loadBatch();                // read next batch of current file
  int    pickDirty();                // ledger idx of next dirty entry, or -1
  void   reconcile();
  void   loadSidecar();
  void   persistSidecar();
  String sidecarPath();
  String fullPath(const char* name);

  PunkMesh*     _mesh = nullptr;
  LedgerEntry*  _ledger = nullptr;
  int           _ledger_count = 0;

  BleSyncFrame* _frames = nullptr;
  int           _frame_count = 0;
  int           _frame_idx = 0;

  StoredMsg*    _records = nullptr;    // scratch batch (PSRAM)
  uint32_t*     _rec_ends = nullptr;

  int           _cur = -1;             // ledger idx being served
  uint32_t      _cur_next_offset = 0;  // resume point in current file
  uint32_t      _batch_tail_offset = 0;
  bool          _cur_exhausted = false;

  State         _state = ST_INIT;
  bool          _tickle = false;
  bool          _sidecar_dirty = false;
  uint32_t      _sidecar_touch_ms = 0;
};

#endif // BLE_COMPANION_ENABLED

#pragma once

// Board tags: how an image says which board it was built for.
//
// The main firmware carries "MESHPUNK-BOARD:<slug>" (kBoardTag in
// ota_update.cpp) and the updater carries "MESHPUNK-UPDATER:<slug>"
// (kUpdaterTag in updater/main_updater.cpp), each as one contiguous string.
// Both verifiers look for the first board tag in an image file and compare
// its slug with MESHPUNK_BOARD_NAME; the main firmware also looks for the
// updater tag in the mapped updater partition. Every scanner assembles its
// search prefix at run time from two separate literals, so no program
// contains a prefix it did not declare as its own tag.
//
// Header-only, included by the main firmware and by the updater (which
// compiles only src/updater/ but has src/ on its include path).

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define OTA_TAG_SLUG_MAX 31

struct OtaTagScan {
  char    prefix[24];
  size_t  prefix_len;
  char    slug[OTA_TAG_SLUG_MAX + 1];
  bool    found;
  uint8_t carry[64];   // tail of the previous chunk, for tags that straddle chunks
  size_t  carry_len;
};

static inline void ota_tag_scan_init(OtaTagScan& s, bool updater_tag) {
  static const char head[]  = "MESHPUNK-";
  static const char board[] = "BOARD:";
  static const char upd[]   = "UPDATER:";
  snprintf(s.prefix, sizeof s.prefix, "%s%s", head, updater_tag ? upd : board);
  s.prefix_len = strlen(s.prefix);
  s.slug[0] = 0;
  s.found = false;
  s.carry_len = 0;
}

// Searches buf for "<prefix><slug>\0". True when a complete tag was read. A
// prefix whose slug runs past the end of buf is left for a later call that
// sees more bytes; a prefix with no terminator within the slug limit is not
// a tag.
static inline bool ota_tag_search(OtaTagScan& s, const uint8_t* buf, size_t len) {
  if (len < s.prefix_len) return false;
  for (size_t i = 0; i + s.prefix_len <= len; i++) {
    if (buf[i] != (uint8_t)s.prefix[0]) continue;
    if (memcmp(buf + i, s.prefix, s.prefix_len) != 0) continue;
    const uint8_t* p = buf + i + s.prefix_len;
    size_t avail = len - (i + s.prefix_len);
    size_t window = avail < OTA_TAG_SLUG_MAX + 1 ? avail : OTA_TAG_SLUG_MAX + 1;
    const uint8_t* nul = (const uint8_t*)memchr(p, 0, window);
    if (!nul) {
      if (avail < OTA_TAG_SLUG_MAX + 1) return false;
      continue;
    }
    if (nul == p) continue;
    memcpy(s.slug, p, nul - p);
    s.slug[nul - p] = 0;
    s.found = true;
    return true;
  }
  return false;
}

// Feeds one chunk of the image. The last 63 bytes of each chunk are kept and
// searched together with the first 63 bytes of the next, which covers a tag
// of at most 17 + 31 + 1 bytes crossing a chunk boundary.
static inline void ota_tag_scan_feed(OtaTagScan& s, const uint8_t* data, size_t len) {
  if (s.found || len == 0) return;
  const size_t keep_max = sizeof s.carry - 1;
  if (s.carry_len) {
    uint8_t win[sizeof s.carry * 2];
    size_t take = len < keep_max ? len : keep_max;
    memcpy(win, s.carry, s.carry_len);
    memcpy(win + s.carry_len, data, take);
    if (ota_tag_search(s, win, s.carry_len + take)) return;
  }
  if (ota_tag_search(s, data, len)) return;
  size_t keep = len < keep_max ? len : keep_max;
  memcpy(s.carry, data + len - keep, keep);
  s.carry_len = keep;
}

static inline void ota_tag_scan_finish(OtaTagScan& s) {
  if (!s.found && s.carry_len) ota_tag_search(s, s.carry, s.carry_len);
}

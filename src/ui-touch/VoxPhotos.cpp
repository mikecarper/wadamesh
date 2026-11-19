// SPDX-License-Identifier: GPL-3.0-or-later
#include "VoxPhotos.h"

#include <esp_heap_caps.h>
#include <new>

#include <Arduino.h>
#include <FS.h>
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "../MyMesh.h"
#include "ChannelSenderSplit.h"
#include "VoxAv1Core.h"
#include "VoxServe.h"
#include "VoxVoice.h"
#include "device_caps.h"
#include "../helpers/PsramStatic.h"

namespace VoxPhotos {
namespace {

fs::FS* g_fs = nullptr;
// Folder the files go in: "" (the root) on internal flash, "/meshcomod" when
// they follow the data store onto the SD card.
char g_root[24] = "";
// The overflow store: the SD card, when internal flash has no room left for a
// photo or clip and none of ours is left to remove. Other data (settings, chat
// history, OTP pads) can fill internal flash on its own; a Super Sharp photo
// (~38 KB) then finished downloading and failed with "couldn't store it" on a
// ThinkNode M9 with a card in it. Files are read from internal flash first,
// then from here.
fs::FS* g_fs2 = nullptr;
char g_root2[24] = "";
fs::FS* (*g_overflow_fn)() = nullptr;   // mounts the card on demand; nullptr: no card
uint32_t g_overflow_try_ms = 0;
uint32_t g_version = 0;
// Photos and talks whose fetch changed since stateVersion() last looked, and the
// status each was last seen in: a packet arriving changes only the progress, a
// new status (requesting, complete, failed) is what the chat must show soon.
uint32_t g_state_version = 0;
uint32_t g_touched[8];
int g_touched_n = 0;
struct LastSt { uint32_t sid; uint8_t st; };
LastSt g_last_st[16];
int g_last_st_next = 0;
void touched(uint32_t sid) {
  g_version++;
  for (int i = 0; i < g_touched_n; i++) if (g_touched[i] == sid) return;
  if (g_touched_n < 8) g_touched[g_touched_n++] = sid;
  else g_state_version++;   // too many at once: treat it as a real change
}
void bumpState() { g_version++; g_state_version++; }

// Photos whose thumbnail is to be made again from the photo: it was the early
// one from the first packet, or the photo has just completed.
uint32_t g_thumb_redo[16];
uint8_t g_thumb_redo_next = 0;
void thumbRedoSet(uint32_t sid, bool on) {
  for (auto& r : g_thumb_redo) if (r == sid) { if (!on) r = 0; return; }
  if (on) g_thumb_redo[g_thumb_redo_next++ % 16] = sid;
}
bool thumbRedo(uint32_t sid) {
  for (uint32_t r : g_thumb_redo) if (sid && r == sid) return true;
  return false;
}
// Photos whose early thumbnail was asked for, so it's asked for once.
uint32_t g_early_asked[16];
uint8_t g_early_asked_next = 0;

// Photos that want a thumbnail made (a download just finished, or a bubble
// showed one without). The UI takes them one at a time (nextThumbWanted).
uint32_t g_thumb_want[8];
int g_thumb_want_n = 0;
void wantThumb(uint32_t sid) {
  if (!sid) return;
  for (int i = 0; i < g_thumb_want_n; i++) if (g_thumb_want[i] == sid) return;
  if (g_thumb_want_n == 8) {   // full: the oldest goes
    memmove(g_thumb_want, g_thumb_want + 1, sizeof g_thumb_want[0] * 7);
    g_thumb_want_n = 7;
  }
  g_thumb_want[g_thumb_want_n++] = sid;
}

// What each recent envelope was, so a finished clip is stored the right way:
// a photo as its payload (/vox_<sid>.vi1, opens in Files), a voice clip as
// [wire mode][codec frames] (/vox_<sid>.ve3).
struct Seen { uint32_t sid; VoxWire::Kind kind; uint8_t mode; };
Seen g_seen[16];
int g_seen_next = 0;

void remember(const VoxWire::Envelope& env) {
  for (auto& e : g_seen) if (e.sid == env.sid && (e.sid || e.mode)) { e.kind = env.kind; e.mode = env.mode; return; }
  g_seen[g_seen_next] = Seen{env.sid, env.kind, env.mode};
  g_seen_next = (g_seen_next + 1) % (int)(sizeof g_seen / sizeof g_seen[0]);
}
const Seen* seen(uint32_t sid) {
  for (auto& e : g_seen) if (e.sid == sid && (e.sid || e.mode)) return &e;
  return nullptr;
}

// Kinds: 0 the photo (.vi1), 1 a voice clip (.ve3), 2 a photo's thumbnail (.vt1).
void kindPath(const char* root, uint32_t sid, int kind, char* out, size_t cap) {
  static const char* const kExt[3] = {".vi1", ".ve3", ".vt1"};
  snprintf(out, cap, "%s/vox_%08lx%s", root, (unsigned long)sid, kExt[kind < 0 || kind > 2 ? 0 : kind]);
}

// Opens a stored file for reading: from internal flash, else the overflow.
File openVox(uint32_t sid, int kind) {
  char path[48];
  if (g_fs) {
    kindPath(g_root, sid, kind, path, sizeof path);
    // With an overflow, ask first: opening a missing file logs an error.
    if (!g_fs2 || g_fs->exists(path)) {
      File f = g_fs->open(path, "r");
      if (f) return f;
    }
  }
  if (g_fs2) {
    kindPath(g_root2, sid, kind, path, sizeof path);
    return g_fs2->open(path, "r");
  }
  return File();
}

bool existsVox(uint32_t sid, int kind) {
  char path[48];
  kindPath(g_root, sid, kind, path, sizeof path);
  if (g_fs && g_fs->exists(path)) return true;
  if (!g_fs2) return false;
  kindPath(g_root2, sid, kind, path, sizeof path);
  return g_fs2->exists(path);
}

// Removes it from both stores; true if either had it.
bool removeVox(uint32_t sid, int kind) {
  char path[48];
  bool ok = false;
  kindPath(g_root, sid, kind, path, sizeof path);
  if (g_fs && g_fs->remove(path)) ok = true;
  if (g_fs2) {
    kindPath(g_root2, sid, kind, path, sizeof path);
    if (g_fs2->exists(path) && g_fs2->remove(path)) ok = true;
  }
  return ok;
}

// Photos and clips live on internal flash and nothing else ever removes them,
// so a radio that has been on the air long enough fills it: the download
// completes and then "couldn't store it", and a new recording can't be kept.
// When a write doesn't fit, the oldest stored photo or clip (other than the
// one being written) makes room, and the write is tried again. Only our own
// /vox_<sid>.vi1|.ve3 files are touched: OTP pads, settings and the rest of
// internal storage are never removed for this.
// Whether a photo or clip is stored, remembered. Every photo and voice bubble
// asks (download state, "tap to play", how it got here), several times each,
// on every layout of the open chat: all of the channel's bubbles, not just the
// visible ones, and once a second while a transfer runs. Each ask was a
// filesystem exists(), which SPIFFS answers by scanning its page tables (slow,
// and slower as it fills) and the SD card over the bus the T-Deck's screen and
// radio share: opening a channel with many photos and clips took many seconds.
// The answers live here; every write and remove below keeps them true.
// Kinds: 0 the photo (.vi1), 1 a voice clip (.ve3), 2 a photo's thumbnail (.vt1).
// bits: (1 << 2k) known, (2 << 2k) present. A thumbnail's size rides along, so a
// chat layout can make room for it without reading the file.
struct HaveEntry { uint32_t sid; uint8_t bits; uint8_t tw, th; };
static auto& g_have = psramStatic<HaveEntry[128]>();   // PSRAM: not internal RAM
static uint32_t g_fs_checks = 0;   // exists() calls made (About: chat open timing)
static uint8_t g_have_next = 0;

static void haveClear() { memset(g_have, 0, sizeof g_have); g_have_next = 0; }

static HaveEntry* haveFind(uint32_t sid, bool add) {
  for (auto& h : g_have) if (h.bits && h.sid == sid) return &h;
  if (!add) return nullptr;
  HaveEntry& h = g_have[g_have_next];
  g_have_next = (uint8_t)((g_have_next + 1) % (sizeof g_have / sizeof g_have[0]));
  h.sid = sid;
  h.bits = 0;
  h.tw = h.th = 0;
  return &h;
}

static void idxNote(uint32_t sid, int kind, bool present, uint8_t tw = 0, uint8_t th = 0);
static void haveSetKind(uint32_t sid, int kind, bool present) {
  idxNote(sid, kind, present);   // every fact learned or made keeps the index true too
  HaveEntry* h = haveFind(sid, true);
  const uint8_t known = (uint8_t)(1u << (2 * kind)), has = (uint8_t)(2u << (2 * kind));
  h->bits = (uint8_t)((h->bits & ~has) | known | (present ? has : 0));
}
static void haveSet(uint32_t sid, bool voice, bool present) { haveSetKind(sid, voice ? 1 : 0, present); }

// From a store path ("<root>/vox_<sid>.vi1|.ve3|.vt1"): its sid and kind.
static bool havePathKey(const char* path, uint32_t* sid, int* kind) {
  const char* b = strrchr(path, '/');
  b = b ? b + 1 : path;
  if (strlen(b) != 16 || strncmp(b, "vox_", 4)) return false;
  char hex[9];
  memcpy(hex, b + 4, 8);
  hex[8] = '\0';
  *sid = (uint32_t)strtoul(hex, nullptr, 16);
  *kind = !strcmp(b + 12, ".ve3") ? 1 : !strcmp(b + 12, ".vt1") ? 2 : 0;
  return true;
}

static void haveSetPath(const char* path, bool present) {
  uint32_t sid; int kind;
  if (havePathKey(path, &sid, &kind)) haveSetKind(sid, kind, present);
}

// ---- the store's index ------------------------------------------------------------------
// One background pass lists the store folder once (and reads each thumbnail's
// two-byte size): which sids have a photo, a clip, a thumbnail. From then on
// every "is it stored" answer comes from RAM, kept true by every write and
// remove below. On a T-Deck where one lookup took ~300 ms (the SD card behind
// the bus the screen and radio share, or a full SPIFFS), a private channel full
// of photos and voice opened in 20 s: its layout asked 63 times. Until the
// index is ready, the chat asks in "quick" mode (setQuick): unknown counts as
// not stored, without touching the filesystem, and the chat is laid out again
// when the index lands (layoutVersion).
struct IdxEntry { uint32_t sid; uint32_t mtime; uint8_t kinds; uint8_t tw, th; };   // kinds: 1 photo, 2 voice, 4 thumb
static IdxEntry* g_idx = nullptr;
static int g_idx_n = 0, g_idx_cap = 0;
static bool g_indexed = false;            // g_idx is complete
static bool g_idx_started = false;
static uint32_t g_idx_gen = 0;            // a new store discards a pass in flight
static volatile bool g_idx_ready = false; // the pass finished: g_idx_new
static IdxEntry* g_idx_new = nullptr;
static int g_idx_new_n = 0, g_idx_new_cap = 0;
static uint32_t g_idx_new_gen = 0;
struct IdxOp { uint32_t sid; uint8_t kind; bool present; uint8_t tw, th; };
static auto& g_idx_ops = psramStatic<IdxOp[48]>();   // changes made while the pass ran, applied after it
static int g_idx_ops_n = 0;
static bool g_idx_ops_lost = false;
static uint32_t g_layout_ver = 0;         // bubble heights may differ: lay the chat out again
static bool g_quick = false;

static IdxEntry* idxFindIn(IdxEntry* a, int n, uint32_t sid) {
  for (int i = 0; i < n; i++) if (a[i].sid == sid) return &a[i];
  return nullptr;
}
static IdxEntry* idxAdd(IdxEntry** a, int* n, int* cap, uint32_t sid) {
  if (IdxEntry* e = idxFindIn(*a, *n, sid)) return e;
  if (*n == *cap) {
    const int nc = *cap ? *cap * 2 : 128;
    IdxEntry* g = (IdxEntry*)heap_caps_realloc(*a, sizeof(IdxEntry) * (size_t)nc, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!g) return nullptr;
    *a = g;
    *cap = nc;
  }
  IdxEntry& e = (*a)[(*n)++];
  e = IdxEntry{sid, 0, 0, 0, 0};
  return &e;
}
static void idxApply(uint32_t sid, int kind, bool present, uint8_t tw, uint8_t th) {
  IdxEntry* e = present ? idxAdd(&g_idx, &g_idx_n, &g_idx_cap, sid) : idxFindIn(g_idx, g_idx_n, sid);
  if (!e) return;
  const uint8_t bit = (uint8_t)(1u << kind);
  e->kinds = present ? (uint8_t)(e->kinds | bit) : (uint8_t)(e->kinds & ~bit);
  if (kind == 2) { e->tw = present ? tw : 0; e->th = present ? th : 0; }
  if (kind == 0 && present && !e->mtime) e->mtime = 0xFFFFFFFFu;   // just stored: the newest
}
static void idxNote(uint32_t sid, int kind, bool present, uint8_t tw, uint8_t th) {
  if (g_indexed) { idxApply(sid, kind, present, tw, th); return; }
  if (!g_idx_started) return;   // the pass, when it starts, sees it on disk
  if (g_idx_ops_n < (int)(sizeof g_idx_ops / sizeof g_idx_ops[0]))
    g_idx_ops[g_idx_ops_n++] = IdxOp{sid, (uint8_t)kind, present, tw, th};
  else
    g_idx_ops_lost = true;
}

static void idxList(fs::FS* fs, const char* root, IdxEntry** pa, int* pn, int* pcap);
static void idxTask(void* arg) {
  const uint32_t gen = (uint32_t)(uintptr_t)arg;
  IdxEntry* a = nullptr;
  int n = 0, cap = 0;
  char root[sizeof g_root], root2[sizeof g_root2];
  fs::FS* fs = g_fs;
  fs::FS* fs2 = g_fs2;
  snprintf(root, sizeof root, "%s", g_root);
  snprintf(root2, sizeof root2, "%s", g_root2);
  idxList(fs, root, &a, &n, &cap);
  if (fs2) idxList(fs2, root2, &a, &n, &cap);   // the overflow on the card
  g_idx_new = a;
  g_idx_new_n = n;
  g_idx_new_cap = cap;
  g_idx_new_gen = gen;
  g_idx_ready = true;
  vTaskDelete(nullptr);
}

static void idxList(fs::FS* fs, const char* root, IdxEntry** pa, int* pn, int* pcap) {
  IdxEntry*& a = *pa;
  int& n = *pn;
  int& cap = *pcap;
  File dir = fs ? fs->open(root[0] ? root : "/") : File();
  if (dir && dir.isDirectory()) {
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
      const char* nm = f.name();
      const char* b = strrchr(nm, '/');
      b = b ? b + 1 : nm;
      if (!f.isDirectory() && strlen(b) == 16 && !strncmp(b, "vox_", 4)) {
        const int kind = !strcmp(b + 12, ".vi1") ? 0 : !strcmp(b + 12, ".ve3") ? 1 : !strcmp(b + 12, ".vt1") ? 2 : -1;
        if (kind >= 0) {
          char hex[9];
          memcpy(hex, b + 4, 8);
          hex[8] = 0;
          IdxEntry* e = idxAdd(&a, &n, &cap, (uint32_t)strtoul(hex, nullptr, 16));
          if (e && (e->kinds & (1u << kind))) e = nullptr;   // internal flash's copy, listed first, is the one read
          if (e) {
            e->kinds |= (uint8_t)(1u << kind);
            if (kind == 0) e->mtime = (uint32_t)f.getLastWrite();
            if (kind == 2) {   // the thumbnail's size: its first two bytes
              uint8_t hdr[2] = {0, 0};
              if (f.read(hdr, 2) == 2 && hdr[0] && hdr[1] && hdr[0] <= kThumbMax && hdr[1] <= kThumbMax &&
                  f.size() == 2u + (size_t)hdr[0] * hdr[1] * 2u) {
                e->tw = hdr[0];
                e->th = hdr[1];
              } else {
                e->kinds &= (uint8_t)~4u;   // a broken thumbnail: as none (it is made again)
              }
            }
          }
        }
      }
      f.close();
      vTaskDelay(1);   // the loop and the radio come first
    }
    dir.close();
  }
}

// From tick(): start the pass once a store is set; take its result when done.
static void idxTick() {
  if (!g_fs) return;
  if (!g_idx_started) {
    g_idx_started = true;
    g_idx_ops_n = 0;
    g_idx_ops_lost = false;
    if (xTaskCreatePinnedToCore(idxTask, "vox_idx", 6144, (void*)(uintptr_t)g_idx_gen, 1, nullptr, 0) != pdPASS)
      g_idx_started = false;   // try again on a later tick
    return;
  }
  if (!g_idx_ready) return;
  g_idx_ready = false;
  if (g_idx_new_gen != g_idx_gen) {   // for a store no longer in use
    heap_caps_free(g_idx_new);
    g_idx_new = nullptr;
    g_idx_started = false;
    return;
  }
  heap_caps_free(g_idx);
  g_idx = g_idx_new;
  g_idx_n = g_idx_new_n;
  g_idx_cap = g_idx_new_cap;
  g_idx_new = nullptr;
  if (g_idx_ops_lost) {   // too much changed meanwhile: list again
    g_idx_started = false;
    return;
  }
  for (int i = 0; i < g_idx_ops_n; i++)
    idxApply(g_idx_ops[i].sid, g_idx_ops[i].kind, g_idx_ops[i].present, g_idx_ops[i].tw, g_idx_ops[i].th);
  g_idx_ops_n = 0;
  g_indexed = true;
  ++g_layout_ver;
  bumpState();
  Serial.printf("[VOX] store indexed: %d photos/clips\n", g_idx_n);
}

static bool storeHas(uint32_t sid, bool voice) {
  if (!g_fs) return false;
  if (g_indexed) {
    const IdxEntry* e = idxFindIn(g_idx, g_idx_n, sid);
    return e && (e->kinds & (voice ? 2 : 1));
  }
  const uint8_t known = voice ? 4 : 1, has = voice ? 8 : 2;
  if (const HaveEntry* h = haveFind(sid, false)) {
    if (h->bits & known) return (h->bits & has) != 0;
  }
  if (g_quick) return false;   // a chat being laid out never waits on the filesystem
  ++g_fs_checks;
  const bool present = existsVox(sid, voice ? 1 : 0);
  haveSet(sid, voice, present);
  return present;
}

static bool isVoxFile(const char* n) {
  const size_t l = strlen(n);
  return l == 16 && !strncmp(n, "vox_", 4) &&
         (!strcmp(n + 12, ".ve3") || !strcmp(n + 12, ".vi1") || !strcmp(n + 12, ".vt1"));
}

static const char* baseName(const char* p) {
  const char* b = strrchr(p, '/');
  return b ? b + 1 : p;
}

static bool pruneOldestVox(fs::FS* fs, const char* root, const char* keep) {
  File dir = fs->open(root[0] ? root : "/");
  if (!dir || !dir.isDirectory()) return false;
  char oldest[48] = "";
  time_t oldest_t = 0;
  int n = 0;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    const char* nm = baseName(f.name());
    if (!f.isDirectory() && isVoxFile(nm) && strcmp(nm, baseName(keep))) {
      ++n;
      const time_t t = f.getLastWrite();   // 0 without timestamps: the first listed goes
      if (!oldest[0] || t < oldest_t) { snprintf(oldest, sizeof oldest, "%s/%s", root, nm); oldest_t = t; }
    }
    f.close();
  }
  dir.close();
  if (!oldest[0]) return false;
  const bool ok = fs->remove(oldest);
  haveSetPath(oldest, false);
  Serial.printf("[VOX] storage full: removed %s to make room (%d stored)\n", oldest, n);
  return ok;
}

// [lead byte][data] to `path` on `fs`; false (and nothing left behind) if it
// doesn't fit.
static bool writeTo(fs::FS* fs, const char* path, const uint8_t* lead, const uint8_t* data, size_t len) {
  File f = fs->open(path, "w");
  bool ok = (bool)f;
  if (ok) {
    ok = (!lead || f.write(lead, 1) == 1) && f.write(data, len) == len;
    f.close();
  }
  if (!ok) fs->remove(path);
  return ok;
}

// Writes it to `fs`, making room from the oldest photos and clips there when it
// doesn't fit. It used to give up after 16 removals, and with the store full of
// small thumbnails (~300 B each) that freed too little for a 30 KB photo.
static bool writePruning(fs::FS* fs, const char* root, const char* path, const uint8_t* lead, const uint8_t* data,
                         size_t len) {
  for (int tries = 0; tries < 1024; ++tries) {
    if (!pruneOldestVox(fs, root, path)) return false;
    if (writeTo(fs, path, lead, data, len)) return true;
  }
  return false;
}

static void useOverflow(fs::FS* fs) {
  if (!fs || fs == g_fs) return;   // the store is on the card already
  if (!fs->exists(g_root2)) fs->mkdir(g_root2);
  g_fs2 = fs;
  haveClear();   // "not stored" answers may now be on the card
  g_indexed = false;
  g_idx_started = false;
  ++g_idx_gen;
  Serial.printf("[VOX] SD card for photos and clips that don't fit internal storage (%s)\n", g_root2);
}

// The card, mounted when first needed (a card put in after boot); at most a try
// every 30 s while there's none.
static fs::FS* overflowStore() {
  if (g_fs2 || !g_overflow_fn) return g_fs2;
  if (g_overflow_try_ms && millis() - g_overflow_try_ms < 30000) return nullptr;
  g_overflow_try_ms = millis() | 1;
  useOverflow(g_overflow_fn());
  return g_fs2;
}

// Writes [lead byte][data] as photo, clip or thumbnail `kind` of `sid`: to
// internal flash; when that is full, to the SD card if there is one (nothing is
// removed while the card has room); then making room from the oldest photos
// and clips, internal flash first. False only if it still can't be written:
// "couldn't store it. Tap to retry." after a full download.
static bool writeStore(uint32_t sid, int kind, const uint8_t* lead, const uint8_t* data, size_t len) {
  char path[48], path2[48];
  kindPath(g_root, sid, kind, path, sizeof path);
  bool ok = writeTo(g_fs, path, lead, data, len);   // a failure removed any older copy here
  fs::FS* ov = ok ? nullptr : overflowStore();
  if (ov) kindPath(g_root2, sid, kind, path2, sizeof path2);
  if (!ok && ov && writeTo(ov, path2, lead, data, len)) {
    Serial.printf("[VOX] internal storage full: %s on the SD card (%s)\n", baseName(path2), g_root2);
    ok = true;
  }
  if (!ok) ok = writePruning(g_fs, g_root, path, lead, data, len);
  if (!ok && ov) ok = writePruning(ov, g_root2, path2, lead, data, len);
  haveSetKind(sid, kind, ok);
  if (!ok)
    Serial.printf("[VOX] can't store %s (%u bytes): full%s, no older photos or clips left to remove\n",
                  baseName(path), (unsigned)len + (lead ? 1u : 0u), ov ? " here and on the SD card" : " (no SD card)");
  return ok;
}

// Case-insensitive name match that ignores emoji variation selectors (U+FE0E,
// U+FE0F), as the app does when it matches channel senders to contacts.
void normName(const char* in, char* out, size_t cap) {
  size_t o = 0;
  for (const unsigned char* p = (const unsigned char*)in; *p && o + 1 < cap;) {
    if (p[0] == 0xEF && p[1] == 0xB8 && (p[2] == 0x8E || p[2] == 0x8F)) { p += 3; continue; }
    out[o++] = (char)tolower(*p++);
  }
  out[o] = '\0';
  while (o && out[o - 1] == ' ') out[--o] = '\0';
}

void earlyThumbCheck(uint32_t sid);

class MeshIo : public VoxFetchIo {
 public:
  uint32_t nowMs() override { return millis(); }
  void log(const char* line) override { Serial.println(line); }
  uint32_t random32() override { return esp_random(); }

  void selfKey6(uint8_t out[6]) override { memcpy(out, the_mesh.getSelfPubKey(), 6); }

  bool contactRoute(const uint8_t pub[32], uint8_t* path, uint8_t* packed) override {
    return the_mesh.voxContactRoute(pub, path, packed);
  }
  int contactHops(const uint8_t pub[32]) override { return the_mesh.voxContactHops(pub); }

  bool contactByName(const char* name, uint8_t pub_out[32]) override {
    char want[40], have[40];
    normName(name, want, sizeof want);
    if (!want[0]) return false;
    const int n = the_mesh.getNumContacts();
    for (int i = 0; i < n; i++) {
      ContactInfo c;
      if (!the_mesh.getContactByIdx((uint32_t)i, c)) continue;
      normName(c.name, have, sizeof have);
      if (strcmp(want, have) == 0) {
        memcpy(pub_out, c.id.pub_key, 32);
        return true;
      }
    }
    return false;
  }

  bool sendRaw(const uint8_t* path, uint8_t packed, const uint8_t* data, size_t len) override {
    if ((packed >> 6) != 0) return false;   // raw data takes 1-byte hashes only
    return the_mesh.voxSendRaw(path, packed, data, len);
  }

  bool sendDmFlood(const uint8_t pub[32], const char* text) override {
    // Flood it even when we have a (possibly one-way) route, so the sender's
    // radio learns a route back to us from the path exchange.
    the_mesh.voxForgetRoute(pub);
    ContactInfo c;
    if (!the_mesh.voxContact(pub, c)) return false;
    uint32_t expected_ack = 0, est_timeout = 0;
    const uint32_t ts = the_mesh.getRTCClock()->getCurrentTimeUnique();
    return the_mesh.sendMessage(c, ts, 0, text, expected_ack, est_timeout) != MSG_SEND_FAILED;
  }

  bool sendChannelData(int ch, uint16_t type, const uint8_t* data, size_t len) override {
    return the_mesh.voxSendChannelData(ch, type, data, len);
  }

  // One full photo packet on air at the radio's current settings (the app's
  // voxEstimateAirtime, with no path bytes).
  uint32_t packetAirMs() override {
    const NodePrefs* p = the_mesh.getNodePrefs();
    const int sf = p && p->sf ? p->sf : 8;
    const double bw = p && p->bw > 0 ? p->bw : 62.5;
    const int cr = p && p->cr >= 5 && p->cr <= 8 ? p->cr : 5;
    const int payload = 2 + VoxWire::kPacketHeaderLen + VoxWire::kBytesPerPacket;
    const double t_sym = pow(2.0, sf) / (bw * 1000.0);
    const int de = (sf >= 11 && bw <= 125) ? 1 : 0;
    const double num = 8.0 * payload - 4.0 * sf + 28 + 16;
    const double den = 4.0 * (sf - 2 * de);
    const double n_payload = 8 + fmax(ceil(num / den) * (cr - 4 + 4), 0);
    return (uint32_t)((8 + 4.25 + n_payload) * t_sym * 1000.0);
  }

  uint32_t radio2PacketAirMs() override {
    return mediamesh::radio2AirMs(2 + VoxWire::kPacketHeaderLen + VoxWire::kBytesPerPacket);
  }

  bool hasPayload(uint32_t sid) override {
    return storeHas(sid, false) || storeHas(sid, true);
  }

  bool savePayload(uint32_t sid, const uint8_t* data, size_t len) override {
    if (!g_fs) return false;
    const Seen* e = seen(sid);
    const bool voice = e && e->kind != VoxWire::kImage;
    if (!writeStore(sid, voice ? 1 : 0, voice ? &e->mode : nullptr, data, len)) return false;
    if (!voice) {   // the UI decodes it in the background for the bubble
      thumbRedoSet(sid, true);   // over the early one from the first packet, if any
      wantThumb(sid);
    }
    Serial.printf("[VOX] %s %08lx complete, %u bytes\n", voice ? "voice" : "photo", (unsigned long)sid,
                  (unsigned)len);
    return true;
  }

  void changed(uint32_t sid) override {
    touched(sid);
    earlyThumbCheck(sid);
  }
};

MeshIo g_io;
// The fetch and serve engines (~4 KB together) live in PSRAM: they are only ever
// used from the loop task, and internal RAM is what Wi-Fi, BLE and TLS need.
template <typename T, typename Io>
T& inPsram(Io& io) {
  void* p = heap_caps_malloc(sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) p = malloc(sizeof(T));
  return *new (p) T(io);
}
VoxFetch& g_fetch = inPsram<VoxFetch>(g_io);

// A photo's packet just came in (asked for, or pushed on a channel): once its
// first packets carry the whole thumbnail, have that decoded for the bubble,
// long before the rest of the photo is in.
void earlyThumbCheck(uint32_t sid) {
  const Seen* e = seen(sid);
  if (!e || e->kind != VoxWire::kImage) return;
  for (uint32_t a : g_early_asked) if (a == sid) return;
  const uint8_t* first = nullptr;
  size_t n = 0, tl = 0;
  if (!g_fetch.firstPackets(sid, &first, &n) || !VoxWire::imageThumbObus(first, n, &tl)) return;
  g_early_asked[g_early_asked_next++ % 16] = sid;
  if (!thumbSize(sid, nullptr, nullptr)) wantThumb(sid);
}

// Answers downloads of the clips this radio holds (VoxServe).
class ServeIo : public VoxServeIo {
 public:
  uint32_t nowMs() override { return millis(); }
  uint32_t random32() override { return esp_random(); }
  void selfKey6(uint8_t out[6]) override { memcpy(out, the_mesh.getSelfPubKey(), 6); }

  bool routeToKey6(const uint8_t key6[6], uint8_t* path, int* len) override {
    ContactInfo* c = the_mesh.lookupContactByPubKey(key6, 6);
    if (!c) return false;
    uint8_t packed = 0;
    uint8_t buf[MAX_PATH_SIZE];
    if (!the_mesh.voxContactRoute(c->id.pub_key, buf, &packed)) return false;
    if ((packed >> 6) != 0) return false;   // raw data takes 1-byte hashes only
    const int n = packed & 0x3F;
    memcpy(path, buf, (size_t)n);
    *len = n;
    return true;
  }

  bool sendRaw(const uint8_t* path, int len, const uint8_t* data, size_t n) override {
    if (len < 0 || len > 63) return false;
    return the_mesh.voxSendRaw(path, (uint8_t)len, data, n);
  }

  bool sendChannelData(int ch, uint16_t type, const uint8_t* data, size_t n, bool zero_hop) override {
    return the_mesh.voxSendChannelData(ch, type, data, n, zero_hop);
  }

  // Clips go out on radio2 alone (tx.media radio2): space them by its airtime,
  // not the main channel's, or a faster radio2 sends them no faster.
  uint32_t packetAirMs() override {
    const uint32_t r2 = mediamesh::mediaRadio2AirMs(2 + VoxWire::kPacketHeaderLen + VoxWire::kBytesPerPacket);
    return r2 ? r2 : g_io.packetAirMs();
  }

  bool loadClip(uint32_t sid, uint8_t* mode, uint8_t** bits, size_t* len) override {
    int m = 0;
    if (loadVoice(sid, &m, bits, len)) {
      *mode = (uint8_t)m;
      return true;
    }
    // A photo this radio sent (sendPhoto), or one it downloaded: served the same
    // way, in 156-byte packets (wire mode 40).
    if (!loadFile(sid, false, bits, len)) return false;
    *mode = (uint8_t)VoxWire::kImageModeId;
    return true;
  }
  void freeClip(uint8_t* bits) override { free(bits); }
  void log(const char* line) override { Serial.println(line); }
};

ServeIo g_serve_io;
VoxServe& g_serve = inPsram<VoxServe>(g_serve_io);

bool parseChannelPost(const char* text, char* name, size_t name_cap, VoxWire::Envelope* env) {
  const char* body = ChannelSenderSplit::split(text, name, name_cap);
  return VoxWire::parseEnvelope(body, env);
}

}  // namespace

bool canFetch(const VoxWire::Envelope& env) {
  return env.kind == VoxWire::kImage || canPlay(env);
}

bool canPlay(const VoxWire::Envelope& env) {
#if CAP_LUA_AUDIO
  return env.kind == VoxWire::kCodec2 && voxCodec2CanDecode(env.mode);
#else
  (void)env;
  return false;
#endif
}

void setStore(fs::FS* fs, const char* root) {
  g_fs = fs;
  haveClear();   // another store: nothing known about it yet
  g_indexed = false;
  g_idx_started = false;
  ++g_idx_gen;
  snprintf(g_root, sizeof g_root, "%s", root ? root : "");
}

void setOverflow(fs::FS* (*mount)(), const char* root) {
  g_overflow_fn = mount;
  snprintf(g_root2, sizeof g_root2, "%s", root ? root : "/meshcomod");
  g_overflow_try_ms = millis() | 1;
  if (mount) useOverflow(mount());   // at boot, so the index lists what is already there
}

static VoxPhotos::Latest g_latest[2];   // [0] photo, [1] voice
static void noteLatest(const char* text, const VoxWire::Envelope& env, const uint8_t* pub, const char* name,
                       int channel) {
  if (env.kind != VoxWire::kImage && env.kind != VoxWire::kCodec2) return;
  VoxPhotos::Latest& l = g_latest[env.kind == VoxWire::kImage ? 0 : 1];
  l = VoxPhotos::Latest{};
  l.env = env;
  size_t cl = 0;
  const char* c = VoxWire::envelopeCaption(text, &cl);
  l.talk = env.kind == VoxWire::kCodec2 && c && cl == 3 && !strncmp(c, "PTT", 3);
  l.at_ms = millis();
  if (name) snprintf(l.name, sizeof l.name, "%s", name);
  if (pub) { memcpy(l.pub, pub, 32); l.has_pub = true; }
  l.channel = channel;
  bumpState();
}

bool latest(bool voice, Latest* out) {
  const Latest& l = g_latest[voice ? 1 : 0];
  if (!l.env.sid || !out) return false;
  *out = l;
  return true;
}

// Mimi clips this radio has seen announced, and when a packet of each was last
// heard: a Codec2 copy's sender waits for its Mimi transfer to finish first,
// and the bubble says so (mimiBusy).
struct MimiHeard { uint32_t sid; uint32_t at; };
static MimiHeard g_mimi[8];
static int g_mimi_next = 0;
static void noteMimi(uint32_t sid) {
  for (auto& m : g_mimi) if (m.sid == sid) return;
  g_mimi[g_mimi_next] = MimiHeard{sid, 0};
  g_mimi_next = (g_mimi_next + 1) % (int)(sizeof g_mimi / sizeof g_mimi[0]);
}
static void heardMimiPacket(const uint8_t* data, size_t len) {
  VoxWire::Packet p;
  if (!data || len == 0 || VoxWire::isFetchRequest(data, len) || !VoxWire::parsePacket(data, len, &p)) return;
  for (auto& m : g_mimi) if (m.sid && m.sid == p.sid) { m.at = millis() | 1; return; }
}

bool mimiBusy(uint32_t mimi_sid) {
  for (auto& m : g_mimi)
    if (m.sid == mimi_sid && m.at) return millis() - m.at < kMimiQuietMs;
  return false;
}

// A Mimi envelope: what this radio does with it is fetch its Codec2 copy
// (VoxWire::codec2Copy). False when it can't play that either.
static bool mimiCopy(const VoxWire::Envelope& env, VoxWire::Envelope* copy) {
  return env.kind == VoxWire::kMimi && VoxWire::codec2Copy(env, copy) && canPlay(*copy);
}

bool onDirectText(const char* text, const uint8_t sender_pub[32]) {
  if (g_serve.onDirectText(text)) return true;   // VR3: someone wants a clip of ours
  VoxWire::Envelope env;
  if (!VoxWire::parseEnvelope(text, &env)) return false;
  VoxWire::Envelope copy;
  if (mimiCopy(env, &copy)) {
    // A Mimi voice message to us: its Codec2 copy is asked for at once, from
    // the sender alone. The bubble stays the Mimi message's.
    noteMimi(env.sid);
    remember(copy);
    g_fetch.onEnvelope(copy, sender_pub, nullptr, -1, true);
    return false;
  }
  if (!canFetch(env)) return false;
  remember(env);
  noteLatest(text, env, sender_pub, nullptr, -1);
  // A photo or voice message sent to us is fetched as it arrives, as the app
  // does (its auto-play setting, on by default): the photo's thumbnail then
  // shows in the chat by itself, and a tap opens or plays it at once. Only
  // its sender is asked. A channel's post is pushed to the radios in range,
  // which keep it the same way (VoxFetch's passive packets). A build that
  // can't show photos (no AV1 decoder) leaves them to a tap.
  g_fetch.onEnvelope(env, sender_pub, nullptr, -1, env.kind != VoxWire::kImage || VOX_AV1_AVAILABLE);
  return false;
}

void onChannelText(const char* text, int channel_idx) {
  char name[ChannelSenderSplit::kMaxWireName + 1];
  VoxWire::Envelope env;
  if (!parseChannelPost(text, name, sizeof name, &env)) return;
  VoxWire::Envelope copy;
  if (mimiCopy(env, &copy)) {
    // On a channel the copy is fetched on a tap (Get as Codec2): every
    // standalone asking for every clip would double the channel's airtime.
    // Known meanwhile, so a copy someone else asked for, answered on the
    // channel, is kept as it goes by.
    noteMimi(env.sid);
    remember(copy);
    g_fetch.onEnvelope(copy, nullptr, name, channel_idx, false);
    return;
  }
  if (!canFetch(env)) return;
  remember(env);
  const char* body = strstr(text, ": ");
  noteLatest(body ? body + 2 : text, env, nullptr, name, channel_idx);
  g_fetch.onEnvelope(env, nullptr, name, channel_idx, false);
  // Its thumbnail before anyone taps: from the sender's push, or, when that
  // doesn't reach here, by asking the channel for its first packets alone.
  if (env.kind == VoxWire::kImage && VOX_AV1_AVAILABLE)
    g_fetch.probeFirst(env.sid, VoxFetch::kThumbProbeChannelWaitMs + esp_random() % VoxFetch::kThumbProbeSpreadMs);
}

void onPayload(const uint8_t* data, size_t len, int channel_idx, uint8_t via, int hops, const uint8_t* last_hop,
               uint8_t last_hop_len) {
  g_fetch.onPayload(data, len, channel_idx, via, hops, last_hop, last_hop_len);
  g_serve.onPayload(data, len, channel_idx, hops);
  heardMimiPacket(data, len);
}

void tick() {
  idxTick();
  g_fetch.tick();
  g_serve.tick();
}

void setQuick(bool on) { g_quick = on; }
void setFast(bool on) {
  g_fetch.setFast(on);
  g_serve.setFast(on);
}
bool fast() { return g_serve.fast(); }
void setAdaptive(bool on) { g_serve.setAdaptive(on); }
bool adaptive() { return g_serve.adaptive(); }
uint32_t layoutVersion() { return g_layout_ver; }

bool parse(const char* text, VoxWire::Envelope* env) { return VoxWire::parseEnvelope(text, env); }

void request(const VoxWire::Envelope& env, const uint8_t* dm_pub, const char* sender_name,
             int channel_idx) {
  if (!canFetch(env)) return;
  remember(env);
  g_fetch.onEnvelope(env, dm_pub, sender_name, channel_idx, false);
  g_fetch.request(env.sid);
}

VoxFetch::Status status(uint32_t sid, int* received, int* total) {
  return g_fetch.status(sid, received, total);
}

const char* statusText(uint32_t sid, char* buf, size_t cap) { return g_fetch.statusText(sid, buf, cap); }

bool viaRepeater(uint32_t sid, char* hex, size_t cap) {
  uint8_t h[3];
  uint8_t n = 0;
  if (!hex || cap < 7 || g_fetch.status(sid) != VoxFetch::kComplete || !g_fetch.radio2Hop(sid, h, &n)) return false;
  for (uint8_t i = 0; i < n; i++) snprintf(hex + 2 * i, cap - 2 * i, "%02x", h[i]);
  return true;
}

const char* viaText(uint32_t sid) {
  if (g_fetch.status(sid) != VoxFetch::kComplete) return nullptr;
  return VoxFetch::viaText(g_fetch.via(sid));
}

bool load(uint32_t sid, uint8_t** data, size_t* len) { return loadFile(sid, false, data, len); }

bool loadVoice(uint32_t sid, int* mode, uint8_t** frames, size_t* len) {
  uint8_t* raw = nullptr;
  size_t n = 0;
  if (!mode || !frames || !len || !loadFile(sid, true, &raw, &n)) return false;
  if (n < 2) { free(raw); return false; }
  *mode = raw[0];
  memmove(raw, raw + 1, n - 1);
  *frames = raw;
  *len = n - 1;
  return true;
}

bool loadFile(uint32_t sid, bool voice, uint8_t** data, size_t* len) {
  if (!g_fs || !data || !len) return false;
  File f = openVox(sid, voice ? 1 : 0);
  if (!f) return false;
  const size_t n = f.size();
  if (n < 2 || n > 64 * 1024) { f.close(); return false; }   // largest clip: 96 x 156 B
  uint8_t* b = (uint8_t*)malloc(n);
  if (!b) { f.close(); return false; }
  const size_t r = f.read(b, n);
  f.close();
  if (r != n) { free(b); return false; }
  *data = b;
  *len = n;
  return true;
}

uint32_t version() { return g_version; }
uint32_t fsChecks() { return g_fs_checks; }

uint32_t stateVersion() {
  for (int i = 0; i < g_touched_n; i++) {
    const uint32_t sid = g_touched[i];
    const uint8_t st = (uint8_t)g_fetch.status(sid);
    int at = -1;
    for (int j = 0; j < 16; j++) if (g_last_st[j].sid == sid) { at = j; break; }
    if (at < 0) {
      at = g_last_st_next;
      g_last_st_next = (g_last_st_next + 1) % 16;
      g_last_st[at].sid = sid;
      g_last_st[at].st = 0xFF;
    }
    if (g_last_st[at].st != st) { g_last_st[at].st = st; g_state_version++; }
  }
  g_touched_n = 0;
  return g_state_version;
}

bool addRecording(uint8_t wire_mode, uint8_t* bits, size_t len, uint32_t ms, char* envelope, size_t cap,
                  uint32_t* sid_out, uint32_t want_sid) {
  const int pb = VoxWire::packetBytes(wire_mode);
  if (!g_fs || !bits || len == 0 || pb <= 0) { free(bits); return false; }
  const int total = (int)((len + (size_t)pb - 1) / (size_t)pb);
  const int seconds = (int)((ms + 999) / 1000);
  uint32_t sid = want_sid;
  while (!sid) sid = esp_random();
  if (total > VoxWire::kMaxClipPackets || !VoxWire::encodeVoiceEnvelope(sid, wire_mode, total, seconds, envelope, cap)) {
    free(bits);
    return false;
  }
  // Stored like a downloaded clip ([wire mode][frames]), so the bubble plays it
  // and VoxServe can answer for it after a reboot too.
  const bool ok = writeStore(sid, 1, &wire_mode, bits, len);
  free(bits);
  if (!ok) return false;
  VoxWire::Envelope env;
  VoxWire::parseEnvelope(envelope, &env);
  remember(env);
  g_serve.markOwn(sid);
  bumpState();
  if (sid_out) *sid_out = sid;
  Serial.printf("[VOX] recorded clip %08lx: %u bytes, %d packets, %d s\n", (unsigned long)sid, (unsigned)len, total,
                seconds);
  return true;
}

bool storeHeard(uint32_t sid, uint8_t wire_mode, const uint8_t* bits, size_t len) {
  if (!g_fs || !sid || !bits || len == 0) return false;
  if (storeHas(sid, true)) return true;
  if (!writeStore(sid, 1, &wire_mode, bits, len)) return false;
  VoxWire::Envelope env;
  env.sid = sid;
  env.kind = VoxWire::kCodec2;
  env.mode = wire_mode;
  remember(env);
  bumpState();
  Serial.printf("[VOX] talk %08lx heard live, kept: %u bytes\n", (unsigned long)sid, (unsigned)len);
  return true;
}

void discardRecording(uint32_t sid) {
  if (!g_fs || !sid) return;
  removeVox(sid, 1);
  haveSet(sid, true, false);
  bumpState();
  Serial.printf("[VOX] discarded clip %08lx\n", (unsigned long)sid);
}

bool haveVoice(uint32_t sid) { return storeHas(sid, true); }

// ---- thumbnails: /vox_<sid>.vt1 = [w][h][w*h RGB565], at most kThumbMax a side ----

void requestThumb(uint32_t sid) { wantThumb(sid); }

int listPhotos(StoredPhoto* out, int cap) {
  if (!g_fs || !out || cap <= 0) return 0;
  if (g_indexed) {   // from the index: no folder listing (seconds on a slow store)
    int n = 0;
    for (int i = 0; i < g_idx_n; i++) {
      if (!(g_idx[i].kinds & 1)) continue;
      const StoredPhoto p{g_idx[i].sid, g_idx[i].mtime, 0};
      if (n < cap) {
        out[n++] = p;
      } else {
        int o = 0;
        for (int k = 1; k < n; k++) if (out[k].mtime < out[o].mtime) o = k;
        if (p.mtime > out[o].mtime) out[o] = p;
      }
    }
    for (int i = 1; i < n; i++) {
      const StoredPhoto x = out[i];
      int j = i - 1;
      while (j >= 0 && out[j].mtime < x.mtime) { out[j + 1] = out[j]; --j; }
      out[j + 1] = x;
    }
    return n;
  }
  int n = 0;
  for (int store = 0; store < 2; ++store) {   // internal flash, then the overflow on the card
  fs::FS* fs = store ? g_fs2 : g_fs;
  const char* root = store ? g_root2 : g_root;
  File dir = fs ? fs->open(root[0] ? root : "/") : File();
  if (!dir || !dir.isDirectory()) continue;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    const char* nm = f.name();
    const char* b = strrchr(nm, '/');
    b = b ? b + 1 : nm;
    if (!f.isDirectory() && strlen(b) == 16 && !strncmp(b, "vox_", 4) && !strcmp(b + 12, ".vi1")) {
      char hex[9];
      memcpy(hex, b + 4, 8);
      hex[8] = 0;
      const StoredPhoto p{(uint32_t)strtoul(hex, nullptr, 16), (uint32_t)f.getLastWrite(), (uint32_t)f.size()};
      haveSet(p.sid, false, true);
      bool dup = false;
      for (int k = 0; k < n && !dup; k++) dup = out[k].sid == p.sid;
      if (dup) {
      } else if (n < cap) {
        out[n++] = p;
      } else {   // full: keep the newest (replace the oldest kept)
        int o = 0;
        for (int i = 1; i < n; i++) if (out[i].mtime < out[o].mtime) o = i;
        if (p.mtime > out[o].mtime) out[o] = p;
      }
    }
    f.close();
  }
  dir.close();
  }
  // Newest first (the order the directory lists them in when there are no times).
  for (int i = 1; i < n; i++) {
    const StoredPhoto x = out[i];
    int j = i - 1;
    while (j >= 0 && out[j].mtime < x.mtime) { out[j + 1] = out[j]; --j; }
    out[j + 1] = x;
  }
  return n;
}

bool photoEnvelope(uint32_t sid, char* out, size_t cap) {
  if (!g_fs || !sid || !out) return false;
  File f = openVox(sid, 0);
  if (!f) return false;
  const size_t len = f.size();
  uint8_t ver = 0;
  const bool ok = len > 1 && f.read(&ver, 1) == 1 && VoxWire::isImagePayloadVersion(ver);
  f.close();
  if (!ok) return false;
  const int pb = VoxWire::packetBytes(VoxWire::kImageModeId);
  const int total = (int)((len + (size_t)pb - 1) / (size_t)pb);
  if (total < 1 || total > VoxWire::kMaxPackets) return false;
  // width * 100 / height, from the thumbnail (made when the photo was first
  // decoded); square when there is none yet.
  int tw = 0, th = 0;
  const uint32_t aspect = thumbSize(sid, &tw, &th) && th > 0 ? (uint32_t)(tw * 100 / th) : 100u;
  char buf[48];
  int n = snprintf(buf, sizeof buf, "VI1:");
  n += VoxWire::toBase36(sid, buf + n);
  buf[n++] = ':';
  n += VoxWire::toBase36((uint32_t)VoxWire::kImageModeId, buf + n);
  buf[n++] = ':';
  n += VoxWire::toBase36((uint32_t)total, buf + n);
  buf[n++] = ':';
  n += VoxWire::toBase36(aspect, buf + n);
  n += snprintf(buf + n, sizeof buf - (size_t)n, " %s", VoxWire::kindLabel(VoxWire::kImage));   // plain word (VoxWire.h)
  if ((size_t)n + 1 > cap) return false;
  memcpy(out, buf, (size_t)n + 1);
  // Ours to serve from now on (VoxServe answers for it as its author), and
  // known as a photo so the bubble reads "Photo sent".
  VoxWire::Envelope env;
  if (VoxWire::parseEnvelope(out, &env)) remember(env);
  g_serve.markOwn(sid);
  return true;
}

bool importPhoto(const uint8_t* data, size_t len, uint32_t* sid_out) {
  if (!g_fs || !data || len < 2 || !VoxWire::isImagePayloadVersion(data[0])) return false;
  const int pb = VoxWire::packetBytes(VoxWire::kImageModeId);
  if ((len + (size_t)pb - 1) / (size_t)pb > (size_t)VoxWire::kMaxPackets) return false;
  uint32_t sid = 0;
  while (!sid) sid = esp_random();
  if (!writeStore(sid, 0, nullptr, data, len)) return false;
  wantThumb(sid);
  bumpState();
  if (sid_out) *sid_out = sid;
  return true;
}

static void thumbCacheDrop(uint32_t sid);
bool deletePhoto(uint32_t sid) {
  if (!g_fs || !sid) return false;
  const bool ok = removeVox(sid, 0);
  haveSet(sid, false, false);
  if (removeVox(sid, 2)) haveSetKind(sid, 2, false);
  thumbCacheDrop(sid);
  bumpState();
  return ok;
}

bool earlyThumbPayload(uint32_t sid, uint8_t* out, size_t cap, size_t* len) {
  const uint8_t* first = nullptr;
  size_t n = 0, tl = 0;
  if (!out || !g_fetch.firstPackets(sid, &first, &n)) return false;
  const uint8_t* obus = VoxWire::imageThumbObus(first, n, &tl);
  if (!obus || tl + 1 > cap) return false;
  out[0] = VoxWire::kImagePayloadVersion;
  memcpy(out + 1, obus, tl);
  *len = tl + 1;
  return true;
}

bool saveThumbScaledUp(uint32_t sid, const uint16_t* src, int sw, int sh);

bool saveEarlyThumb(uint32_t sid, const uint16_t* src, int sw, int sh) {
  if (!src || sw <= 0 || sh <= 0) return false;
  if (!storeHas(sid, false) && saveThumbScaledUp(sid, src, sw, sh)) {
    thumbRedoSet(sid, true);   // the photo's own replaces it once the photo is in
    return true;
  }
  return false;
}

bool thumbFinal(uint32_t sid) { return thumbSize(sid, nullptr, nullptr) && !thumbRedo(sid); }

uint32_t nextThumbWanted() {
  if (!g_thumb_want_n) return 0;
  const uint32_t sid = g_thumb_want[0];
  memmove(g_thumb_want, g_thumb_want + 1, sizeof g_thumb_want[0] * (size_t)(g_thumb_want_n - 1));
  --g_thumb_want_n;
  return sid;
}

// Thumbnail pixels kept in PSRAM (24 x up to 18 KB), so redrawing a chat or
// the Photos grid doesn't read the files again.
struct ThumbPx { uint32_t sid; uint8_t w, h; uint16_t* px; };
static auto& g_tpx = psramStatic<ThumbPx[24]>();
static uint8_t g_tpx_next = 0;
static void thumbCacheDrop(uint32_t sid) {
  for (auto& t : g_tpx) if (t.px && t.sid == sid) { heap_caps_free(t.px); t.px = nullptr; t.sid = 0; }
}
static void thumbCachePut(uint32_t sid, int w, int h, const uint16_t* px) {
  thumbCacheDrop(sid);
  ThumbPx& t = g_tpx[g_tpx_next];
  g_tpx_next = (uint8_t)((g_tpx_next + 1) % (sizeof g_tpx / sizeof g_tpx[0]));
  if (t.px) heap_caps_free(t.px);
  t.px = (uint16_t*)heap_caps_malloc((size_t)w * h * 2u, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  t.sid = t.px ? sid : 0;
  if (!t.px) return;
  t.w = (uint8_t)w;
  t.h = (uint8_t)h;
  memcpy(t.px, px, (size_t)w * h * 2u);
}

bool thumbSize(uint32_t sid, int* w, int* h) {
  if (!g_fs || !sid) return false;
  if (g_indexed) {
    const IdxEntry* x = idxFindIn(g_idx, g_idx_n, sid);
    if (!x || !(x->kinds & 4) || !x->tw || !x->th) return false;
    if (w) *w = x->tw;
    if (h) *h = x->th;
    return true;
  }
  HaveEntry* e = haveFind(sid, false);
  if ((!e || !(e->bits & 16)) && g_quick) return false;   // unknown: no thumbnail for now
  if (!e || !(e->bits & 16)) {
    ++g_fs_checks;
    uint8_t hdr[2] = {0, 0};
    bool ok = false;
    if (existsVox(sid, 2)) {
      File f = openVox(sid, 2);
      ok = f && f.read(hdr, 2) == 2 && hdr[0] && hdr[1] && hdr[0] <= kThumbMax && hdr[1] <= kThumbMax &&
           f.size() == 2u + (size_t)hdr[0] * hdr[1] * 2u;
      if (f) f.close();
    }
    haveSetKind(sid, 2, ok);
    e = haveFind(sid, false);
    if (e) { e->tw = ok ? hdr[0] : 0; e->th = ok ? hdr[1] : 0; }
  }
  if (!e || !(e->bits & 32) || !e->tw || !e->th) return false;
  if (w) *w = e->tw;
  if (h) *h = e->th;
  return true;
}

bool loadThumb(uint32_t sid, int* w, int* h, uint16_t** px) {
  int tw = 0, th = 0;
  if (!px || !thumbSize(sid, &tw, &th)) return false;
  for (const auto& t : g_tpx) {
    if (!t.px || t.sid != sid) continue;
    const size_t n = (size_t)t.w * t.h * 2u;
    uint16_t* b = (uint16_t*)heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!b) return false;
    memcpy(b, t.px, n);
    *px = b;
    if (w) *w = t.w;
    if (h) *h = t.h;
    return true;
  }
  File f = openVox(sid, 2);
  if (!f) { haveSetKind(sid, 2, false); return false; }
  const size_t n = (size_t)tw * th * 2u;
  uint16_t* b = (uint16_t*)heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!b) b = (uint16_t*)malloc(n);
  bool ok = b && f.seek(2) && f.read((uint8_t*)b, n) == n;
  f.close();
  if (!ok) { free(b); return false; }
  thumbCachePut(sid, tw, th, b);
  *px = b;
  if (w) *w = tw;
  if (h) *h = th;
  return true;
}

bool saveThumb(uint32_t sid, const uint16_t* src, int sw, int sh) {
  if (!g_fs || !sid || !src || sw <= 0 || sh <= 0) return false;
  // Fit inside kThumbMax x kThumbMax, keeping the shape; each pixel the average
  // of the box of source pixels it covers.
  int tw = sw, th = sh;
  if (tw > kThumbMax || th > kThumbMax) {
    if (sw >= sh) { tw = kThumbMax; th = (int)((long)sh * kThumbMax / sw); }
    else { th = kThumbMax; tw = (int)((long)sw * kThumbMax / sh); }
  }
  if (tw < 1) tw = 1;
  if (th < 1) th = 1;
  const size_t n = 2u + (size_t)tw * th * 2u;
  uint8_t* buf = (uint8_t*)heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buf) buf = (uint8_t*)malloc(n);
  if (!buf) return false;
  buf[0] = (uint8_t)tw;
  buf[1] = (uint8_t)th;
  uint16_t* out = (uint16_t*)(buf + 2);
  for (int y = 0; y < th; y++) {
    const int y0 = (int)((long)y * sh / th);
    int y1 = (int)((long)(y + 1) * sh / th);
    if (y1 <= y0) y1 = y0 + 1;
    for (int x = 0; x < tw; x++) {
      const int x0 = (int)((long)x * sw / tw);
      int x1 = (int)((long)(x + 1) * sw / tw);
      if (x1 <= x0) x1 = x0 + 1;
      uint32_t r = 0, g = 0, bl = 0, c = 0;
      for (int yy = y0; yy < y1; yy++) {
        const uint16_t* row = src + (size_t)yy * sw;
        for (int xx = x0; xx < x1; xx++) {
          const uint16_t p = row[xx];
          r += p >> 11; g += (p >> 5) & 0x3F; bl += p & 0x1F; ++c;
        }
      }
      out[y * tw + x] = (uint16_t)(((r / c) << 11) | ((g / c) << 5) | (bl / c));
    }
  }
  const bool ok = writeStore(sid, 2, nullptr, buf, n);
  if (ok) {
    if (HaveEntry* e = haveFind(sid, true)) { e->tw = (uint8_t)tw; e->th = (uint8_t)th; }
    idxNote(sid, 2, true, (uint8_t)tw, (uint8_t)th);   // writeStore noted it without its size
    thumbCachePut(sid, tw, th, out);
    thumbRedoSet(sid, false);
    bumpState();
  }
  free(buf);
  return ok;
}

// A thumbnail smaller than the bubble's (the early one, 64 px at most) scaled
// up to fit kThumbMax bilinearly, so the bubble keeps its size when the
// photo's own replaces it. Stored as saveThumb does.
bool saveThumbScaledUp(uint32_t sid, const uint16_t* src, int sw, int sh) {
  int tw = sw, th = sh;
  if (sw >= sh) { tw = kThumbMax; th = (int)((long)sh * kThumbMax / sw); }
  else { th = kThumbMax; tw = (int)((long)sw * kThumbMax / sh); }
  if (tw <= sw || th < 1 || tw < 1) return saveThumb(sid, src, sw, sh);
  uint16_t* big = (uint16_t*)heap_caps_malloc((size_t)tw * th * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!big) return saveThumb(sid, src, sw, sh);
  for (int y = 0; y < th; y++) {
    // Source position of this pixel's centre, in 1/256ths.
    int fy = (int)(((2L * y + 1) * sh * 128) / th) - 128;
    if (fy < 0) fy = 0;
    int y0 = fy >> 8, wy = fy & 255;
    if (y0 >= sh - 1) { y0 = sh - 1; wy = 0; }
    const int y1 = y0 + 1 < sh ? y0 + 1 : y0;
    for (int x = 0; x < tw; x++) {
      int fx = (int)(((2L * x + 1) * sw * 128) / tw) - 128;
      if (fx < 0) fx = 0;
      int x0 = fx >> 8, wx = fx & 255;
      if (x0 >= sw - 1) { x0 = sw - 1; wx = 0; }
      const int x1 = x0 + 1 < sw ? x0 + 1 : x0;
      const uint16_t p00 = src[y0 * sw + x0], p01 = src[y0 * sw + x1];
      const uint16_t p10 = src[y1 * sw + x0], p11 = src[y1 * sw + x1];
      auto mix = [&](int shift, int mask) {
        const int a = (p00 >> shift) & mask, b = (p01 >> shift) & mask;
        const int c = (p10 >> shift) & mask, d = (p11 >> shift) & mask;
        const int top = a * (256 - wx) + b * wx, bot = c * (256 - wx) + d * wx;
        return ((top * (256 - wy) + bot * wy) + 32768) >> 16;
      };
      big[y * tw + x] = (uint16_t)((mix(11, 31) << 11) | (mix(5, 63) << 5) | mix(0, 31));
    }
  }
  const bool ok = saveThumb(sid, big, tw, th);
  heap_caps_free(big);
  return ok;
}

}  // namespace VoxPhotos

// SPDX-License-Identifier: GPL-3.0-or-later
//
// MediaMesh coverage (Settings > MediaMesh > Map coverage): where the MediaMesh
// repeaters can be reached, recorded as you walk or drive, the same as
// MeshCore Open TAK's coverage map (MEDIAMESH.md, "Coverage map").
//
// Every 150 m, at most every 40 s, the radio asks the repeaters in direct range
// as Find MediaMesh does (DISCOVER_REQ with the radio2 flag) and keeps only the
// ones answering with radio2 on. Each sample is a dot on the map coloured by the
// best repeater's estimated radio2 margin (helpers/MediaMeshCoverage.h), and a
// line per repeater in /meshcomod/mediamesh_coverage.csv on the card.
//
// Included in UITask.cpp before the map markers; the recorder runs from the UI
// loop, so it keeps going with the screen off (GPS stays awake while it does).
// Tapping a dot on the map (mapTap) shows the MediaMesh repeaters heard there.
//
// Share coverage points (off by default) sends the file's points to a
// CoreScope analyzer, analyzer.cascadiamesh.org unless another is entered,
// signed by this standalone's key (helpers/MediaMeshCoverageShare.h; the net
// worker does the request). Where it got to in the file is kept, so each
// point goes once.

#include "helpers/MediaMeshCoverage.h"
#include "helpers/MediaMeshCoverageShare.h"

// The sheet a tapped dot opens (Team's sheet: defined after TeamUI.inc).
static void mmcovShowSheet(const char* title, const char* body);

namespace mmcovui {

static constexpr int kRing = 500;                    // samples kept for the map (PSRAM, 4 KB)
static constexpr const char* kCsv = "/meshcomod/mediamesh_coverage.csv";
static mmcov::Sample* s_ring = nullptr;
static int s_n = 0;                                  // samples ever (ring slot = n % kRing)
static uint32_t s_counts[4] = {0, 0, 0, 0};          // per reach, of the samples in the ring
static bool s_loaded = false, s_rec = false, s_show = true;
// Sharing (see the top): on, where to, and how far into the file has gone.
static bool s_share = false;
static char s_url[128] = "";
static uint32_t s_sent_off = 0;
static bool s_have_last = false;
static int32_t s_last_lat = 0, s_last_lon = 0;
static uint32_t s_last_ms = 0;
static uint32_t s_ask_ms = 0;                        // the ask's millis() (0: not asking)
static uint32_t s_ask_scan = 0;                      // the engine's scan stamp for our ask
static int32_t s_ask_lat = 0, s_ask_lon = 0;
static uint32_t s_tick_ms = 0, s_hold_until = 0;
static lv_obj_t* s_layer = nullptr;                  // the map's dots (one object, drawn by hand)
static double s_cwx = 0, s_cwy = 0;
static lv_obj_t* s_legend = nullptr;                 // Settings: the counts
// What each sample in the ring heard, for a tapped dot: when (epoch, 0 if not
// known) and its repeaters, strongest first (PSRAM, ~59 KB; without it a dot
// shows only its count and best margin).
struct Rep { char name[16]; int8_t snr_q4, remote_q4, margin_q4; };
static uint32_t* s_epoch = nullptr;
static Rep* s_reps = nullptr;                        // kRing x mmcov::kMaxRepeaters

static bool ensureRing() {
  if (!s_ring) s_ring = (mmcov::Sample*)psAlloc(sizeof(mmcov::Sample) * kRing);
  if (s_ring && !s_epoch) s_epoch = (uint32_t*)psAlloc(sizeof(uint32_t) * kRing);
  if (s_ring && !s_reps) s_reps = (Rep*)psAlloc(sizeof(Rep) * kRing * mmcov::kMaxRepeaters);
  return s_ring != nullptr;
}

static void push(const mmcov::Sample& s, uint32_t epoch = 0) {
  if (!ensureRing()) return;
  const int at = s_n % kRing;
  mmcov::Sample& slot = s_ring[at];
  if (s_n >= kRing) { uint8_t r = slot.reach(); if (s_counts[r]) s_counts[r]--; }
  slot = s;
  if (s_epoch) s_epoch[at] = epoch;
  if (s_reps) memset(&s_reps[at * mmcov::kMaxRepeaters], 0, sizeof(Rep) * mmcov::kMaxRepeaters);
  s_counts[s.reach()]++;
  s_n++;
}

// Repeater k of the newest sample.
static void setRep(int k, const char* name, float snr, float remote, float margin) {
  if (!s_reps || !s_n || k < 0 || k >= mmcov::kMaxRepeaters) return;
  Rep& r = s_reps[((s_n - 1) % kRing) * mmcov::kMaxRepeaters + k];
  snprintf(r.name, sizeof r.name, "%s", name ? name : "");
  r.snr_q4 = mmcov::toQ4(snr);
  r.remote_q4 = mmcov::toQ4(remote);
  r.margin_q4 = mmcov::toQ4(margin);
}

// A CSV line's repeater fields (after best_margin): name, snr, remote, margin.
static bool parseRep(const char* c5, char* name, size_t cap, float* snr, float* remote, float* margin) {
  const char* n = c5 ? c5 + 1 : nullptr;
  const char* c6 = n ? strchr(n, ',') : nullptr;
  if (!c6 || c6 == n) return false;
  snprintf(name, cap, "%.*s", (int)(c6 - n), n);
  char* end = nullptr;
  *snr = strtof(c6 + 1, &end);
  if (!end || *end != ',') return false;
  *remote = strtof(end + 1, &end);
  if (!end || *end != ',') return false;
  *margin = strtof(end + 1, nullptr);
  return true;
}

// The last part of the card's file back into the ring, once: the map shows
// earlier walks too. Lines of one sample share epoch, lat and lon.
static void loadFromCard() {
#if CAP_SD || defined(TLORA_PAGER)
  if (SD.cardType() == CARD_NONE) return;
  markSdIo();
  File f = SD.open(kCsv, FILE_READ);
  if (!f) return;
  const size_t size = f.size(), tail = 48 * 1024;
  if (size > tail) { f.seek(size - tail); f.readStringUntil('\n'); }   // skip the cut line
  char prev[48] = "";
  while (f.available()) {
    String ln = f.readStringUntil('\n');
    const char* p = ln.c_str();
    if (!*p || *p == 'e') continue;   // blank or the header
    // epoch,lat,lon,reach,best_margin,...
    const char* c1 = strchr(p, ',');
    const char* c2 = c1 ? strchr(c1 + 1, ',') : nullptr;
    const char* c3 = c2 ? strchr(c2 + 1, ',') : nullptr;
    const char* c4 = c3 ? strchr(c3 + 1, ',') : nullptr;
    if (!c4) continue;
    char key[48];
    snprintf(key, sizeof key, "%.*s", (int)(c3 - p), p);
    const char* c5 = strchr(c4 + 1, ',');
    char rname[24];
    float rsnr = 0, rremote = 0, rmargin = 0;
    const bool has_rep = parseRep(c5, rname, sizeof rname, &rsnr, &rremote, &rmargin);
    if (strcmp(key, prev) == 0) {   // another repeater of the same sample
      if (s_n && s_ring) {
        mmcov::Sample& last = s_ring[(s_n - 1) % kRing];
        if (has_rep) setRep(last.count, rname, rsnr, rremote, rmargin);
        if (last.count < 255) last.count++;
      }
      continue;
    }
    snprintf(prev, sizeof prev, "%s", key);
    mmcov::Sample s;
    s.lat_e6 = (int32_t)lround(strtod(c1 + 1, nullptr) * 1e6);
    s.lon_e6 = (int32_t)lround(strtod(c2 + 1, nullptr) * 1e6);
    const bool none = strncmp(c3 + 1, "none", 4) == 0;
    s.count = none ? 0 : 1;
    s.best_q4 = none ? 0 : mmcov::toQ4((float)strtod(c4 + 1, nullptr));
    push(s, (uint32_t)strtoul(p, nullptr, 10));
    if (!none && has_rep) setRep(0, rname, rsnr, rremote, rmargin);
  }
  f.close();
#endif
}

static void prefsLoad() {
  if (s_loaded) return;
  s_loaded = true;
#if defined(ESP32)
  SdNvsPrefs p;
  if (p.begin("mmcov", true)) {
    s_rec = p.getBool("rec", false);
    s_show = p.getBool("show", true);
    s_share = p.getBool("share", false);
    if (!p.getString("url", s_url, sizeof s_url)) snprintf(s_url, sizeof s_url, "%s", mmshare::kDefaultAnalyzer);
    s_sent_off = p.getUInt("off", 0);
    p.end();
  }
#endif
  if (!s_url[0]) snprintf(s_url, sizeof s_url, "%s", mmshare::kDefaultAnalyzer);
  loadFromCard();
}

static void prefsSave() {
#if defined(ESP32)
  SdNvsPrefs p;
  if (p.begin("mmcov", false)) {
    p.putBool("rec", s_rec);
    p.putBool("show", s_show);
    p.putBool("share", s_share);
    p.putString("url", s_url);
    p.putUInt("off", s_sent_off);
    p.end();
  }
#endif
}

struct Heard { char name[24]; char key[9]; float snr, remote, margin; };

static void writeSample(uint32_t epoch, const mmcov::Sample& s, const Heard* h, int n) {
#if CAP_SD || defined(TLORA_PAGER)
  if (SD.cardType() == CARD_NONE) return;
  markSdIo();
  SD.mkdir("/meshcomod");
  File f = SD.open(kCsv, FILE_APPEND);
  if (!f) { sdNoteIoFailure(); return; }
  if (f.size() == 0) f.print(mmcov::kCsvHeader);
  char line[160];
  const float best = s.best_q4 / 4.0f;
  if (!n) {
    mmcov::csvLine(line, sizeof line, epoch, s.lat_e6, s.lon_e6, mmcov::None, 0, nullptr, 0, 0, 0);
    f.print(line);
  }
  for (int i = 0; i < n; i++) {
    mmcov::csvLine(line, sizeof line, epoch, s.lat_e6, s.lon_e6, s.reach(), best, h[i].name, h[i].snr, h[i].remote,
                   h[i].margin, h[i].key);
    f.print(line);
  }
  f.close();
#else
  (void)epoch; (void)s; (void)h; (void)n;
#endif
}

static void legendRefresh() {
  if (!s_legend || !lv_obj_is_valid(s_legend)) { s_legend = nullptr; return; }
  char buf[160];
  snprintf(buf, sizeof buf, "%s: %lu %s, %lu %s, %lu %s, %lu %s", TR("Coverage"), (unsigned long)s_counts[mmcov::Good], TR("good"),
           (unsigned long)s_counts[mmcov::Marginal], TR("marginal"), (unsigned long)s_counts[mmcov::Out], TR("out"),
           (unsigned long)s_counts[mmcov::None], TR("none"));
  lv_label_set_text(s_legend, buf);
}

// Another scan (Discover, Find MediaMesh) owns the engine's table now.
static bool busy() {
  if (s_mm_find_timer) return true;
  return s_discover_root != nullptr && s_discover_scanning;
}

// The answers to our ask: MediaMesh repeaters only, heard directly.
static void collect(uint32_t now) {
  s_ask_ms = 0;
  if (the_mesh.discoverScanMs() != s_ask_scan) { s_hold_until = now + mmcov::kMinGapMs; return; }   // someone else asked
  NodePrefs* np = the_mesh.getNodePrefs();
  const uint32_t main_bw = np ? (uint32_t)lroundf(np->bw * 1000.0f) : 62500;
  Heard heard[mmcov::kMaxRepeaters];
  int n = 0;
  for (int i = 0; i < the_mesh.discoverCount(); i++) {
    MyMesh::DiscoverHit h;
    if (!the_mesh.discoverGet((uint8_t)i, h) || h.node_type != ADV_TYPE_REPEATER || h.path_len != 0) continue;
    if ((int32_t)(h.last_ms - s_ask_scan) < 0 || !h.r2_len) continue;   // an older answer
    mediamesh::DiscoverRadio2 p;
    if (!mediamesh::parseDiscoverRadio2(h.r2, h.r2_len, p) || !p.active()) continue;
    Heard x;
    x.snr = h.our_snr_q4 / 4.0f;
    x.remote = h.their_snr_q4 / 4.0f;
    x.margin = mmcov::margin(mmcov::linkSnr(h.our_snr_q4, h.their_snr_q4), main_bw, p.bw_hz, p.sf);
    snprintf(x.key, sizeof x.key, "%02x%02x%02x%02x", h.pubkey[0], h.pubkey[1], h.pubkey[2], h.pubkey[3]);
    ContactInfo* c = the_mesh.lookupContactByPubKey(h.pubkey, PUB_KEY_SIZE);
    if (c) snprintf(x.name, sizeof x.name, "%s", c->name);
    else snprintf(x.name, sizeof x.name, "%02X%02X%02X%02X", h.pubkey[0], h.pubkey[1], h.pubkey[2], h.pubkey[3]);
    for (char* q = x.name; *q; q++) if (*q == ',' || *q == '\n' || *q == '"') *q = ' ';   // one CSV field
    // Strongest first, the best few kept.
    int at = n < mmcov::kMaxRepeaters ? n : mmcov::kMaxRepeaters;
    while (at > 0 && heard[at - 1].margin < x.margin) {
      if (at < mmcov::kMaxRepeaters) heard[at] = heard[at - 1];
      at--;
    }
    if (at < mmcov::kMaxRepeaters) { heard[at] = x; if (n < mmcov::kMaxRepeaters) n++; }
  }
  mmcov::Sample s;
  s.lat_e6 = s_ask_lat;
  s.lon_e6 = s_ask_lon;
  s.count = (uint8_t)n;
  s.best_q4 = n ? mmcov::toQ4(heard[0].margin) : 0;
  const uint32_t epoch = (uint32_t)rtc_clock.getCurrentTime();
  push(s, epoch);
  for (int i = 0; i < n; i++) setRep(i, heard[i].name, heard[i].snr, heard[i].remote, heard[i].margin);
  writeSample(epoch, s, heard, n);
  s_have_last = true;
  s_last_lat = s_ask_lat;
  s_last_lon = s_ask_lon;
  s_last_ms = s_ask_scan;
  legendRefresh();
  if (s_show) renderMapMarkers();
}

static void shareTick(uint32_t now);

static void loopTick(uint32_t now) {
  if (!s_loaded) prefsLoad();
  shareTick(now);
  if (!s_rec) return;
  if (s_ask_ms) {
    if ((uint32_t)(now - s_ask_ms) >= mediamesh::kAskListenMs) collect(now);
    return;
  }
  if ((uint32_t)(now - s_tick_ms) < mmcov::kTickMs) return;
  s_tick_ms = now;
  if ((int32_t)(now - s_hold_until) < 0 || busy() || !g_lv.task || !g_lv.task->getGpsFix()) return;
  const double lat = g_lv.task->getNodeLat(), lon = g_lv.task->getNodeLon();
  if (lat == 0.0 && lon == 0.0) return;
  const int32_t lat_e6 = (int32_t)lround(lat * 1e6), lon_e6 = (int32_t)lround(lon * 1e6);
  if (!mmcov::due(s_have_last, s_last_lat, s_last_lon, s_last_ms, lat_e6, lon_e6, now)) return;
  if (the_mesh.getRemainingTxBudget() < 300) return;   // airtime cushion, as Find keeps
  if (the_mesh.uiStartDiscoverScan((uint8_t)(1u << ADV_TYPE_REPEATER), true) == 0) {
    s_hold_until = now + mmcov::kMinGapMs;
    return;
  }
  s_ask_scan = the_mesh.discoverScanMs();
  s_ask_ms = now ? now : 1;
  s_ask_lat = lat_e6;
  s_ask_lon = lon_e6;
}

// ---- sharing with an analyzer -----------------------------------------------------------
// One request at a time: the UI builds and signs it (shareTick), the net worker
// sends it (mmcovPostRun), the UI reads the answer.
static lv_obj_t* s_share_note = nullptr;             // Settings: what the last send did
static char s_share_msg[120] = "";
#if MMCOV_SHARE
struct Post {
  char url[200];
  char method[8];
  char sig[129];
  uint8_t* body;                                     // PSRAM, kBodyCap
  size_t len;
  char resp[256];
  char msg[96];
  int code;
  uint32_t next_off;                                 // the file offset once this batch is in
  int points;
  bool del;
};
static constexpr size_t kBodyCap = 64 * 1024;
static Post* s_post = nullptr;                       // PSRAM
static volatile uint8_t s_post_state = 0;            // 0 idle, 1 with the worker, 2 answered
static uint32_t s_share_next_ms = 0;
static bool s_send_now = false;

static void shareNoteRefresh() {
  if (!s_share_note || !lv_obj_is_valid(s_share_note)) { s_share_note = nullptr; return; }
  lv_label_set_text(s_share_note, s_share_msg[0] ? s_share_msg
                                                 : TR("Off: nothing leaves the standalone. On: what it records goes to the analyzer below, signed by its key. The analyzer shows it as 200 m squares, leaves out where each trip starts and ends, and never says whose it is."));
}

static void shareSay(const char* m) {
  snprintf(s_share_msg, sizeof s_share_msg, "%s", m);
  shareNoteRefresh();
}

static bool wifiUp() { return WiFi.status() == WL_CONNECTED && (uint32_t)WiFi.localIP() != 0; }

static void selfHex(char out[65]) {
  const uint8_t* pk = the_mesh.getSelfPubKey();
  for (int i = 0; i < 32; i++) snprintf(out + i * 2, 3, "%02x", pk[i]);
}

// Signs the body in s_post with prefix, into s_post->sig.
static void signPost(const char* prefix) {
  uint8_t digest[32];
  mesh::Utils::sha256(digest, sizeof digest, s_post->body, (int)s_post->len);
  char text[96];
  const int n = mmshare::signedMessage(text, sizeof text, prefix, digest);
  uint8_t sig[64];
  the_mesh.uiSign(sig, (const uint8_t*)text, n);
  for (int i = 0; i < 64; i++) snprintf(s_post->sig + i * 2, 3, "%02x", sig[i]);
}

static bool ensurePost() {
  if (!s_post) s_post = (Post*)psAlloc(sizeof(Post));
  if (s_post && !s_post->body) s_post->body = (uint8_t*)psAlloc(kBodyCap);
  return s_post && s_post->body;
}

static void startPost(const char* method) {
  snprintf(s_post->method, sizeof s_post->method, "%s", method);
  s_post->resp[0] = 0;
  s_post->msg[0] = 0;
  s_post->code = -1;
  s_post_state = 1;
  ensureTileFetchTaskRunning();
  s_mmcov_post_request = true;
}

// The next batch from the file: false when there is nothing new (or no card).
static bool buildBatch() {
#if CAP_SD || defined(TLORA_PAGER)
  if (SD.cardType() == CARD_NONE) return false;
  markSdIo();
  File f = SD.open(kCsv, FILE_READ);
  if (!f) return false;
  const uint32_t size = (uint32_t)f.size();
  if (s_sent_off > size) s_sent_off = 0;   // a new file
  if (s_sent_off >= size) { f.close(); return false; }
  f.seek(s_sent_off);
  char radio[65];
  selfHex(radio);
  NodePrefs* np = the_mesh.getNodePrefs();
  const uint32_t main_bw = np ? (uint32_t)lroundf(np->bw * 1000.0f) : 62500;
  mmshare::Batch b((char*)s_post->body, kBodyCap);
  b.begin(radio, (uint32_t)rtc_clock.getCurrentTime(), "Wadamesh TAK", main_bw, 250000, 7);
  uint32_t next = s_sent_off;
  char line[200];
  while (f.available()) {
    const uint32_t at = (uint32_t)f.position();
    const size_t n = f.readBytesUntil('\n', line, sizeof line - 1);
    line[n] = 0;
    mmshare::Row r;
    if (!mmshare::parseRow(line, r)) { next = (uint32_t)f.position(); continue; }
    if (!b.add(r)) { next = at; break; }
    next = (uint32_t)f.position();
  }
  f.close();
  if (!b.finish()) return false;
  s_post->len = b.len();
  s_post->points = b.samples();
  s_post->next_off = next;
  if (!b.samples()) {   // only lines that can't be sent (from before keys were written)
    s_sent_off = next;
    prefsSave();
    return false;
  }
  return true;
#else
  return false;
#endif
}

static void shareTick(uint32_t now) {
  if (s_post_state == 2) {   // answered
    s_post_state = 0;
    char m[120], err[96];
    mmshare::replyError(s_post->resp, err, sizeof err);
    if (s_post->code == 200) {
      if (s_post->del) {
        const char* r = strstr(s_post->resp, "\"removed\":");
        snprintf(m, sizeof m, "%s: %ld", TR("Points removed from the analyzer"), r ? strtol(r + 10, nullptr, 10) : 0L);
        s_sent_off = 0;   // sharing again sends them all again
        s_share = false;  // and it's off until switched on again
      } else {
        snprintf(m, sizeof m, "%s: %d", TR("Points sent"), s_post->points);
        s_sent_off = s_post->next_off;
        s_share_next_ms = now + 3000;   // there may be more
      }
      prefsSave();
    } else {
      snprintf(m, sizeof m, "%s: %s", TR("Couldn't send"), err[0] ? err : s_post->msg[0] ? s_post->msg : "no answer");
      s_share_next_ms = now + 10UL * 60UL * 1000UL;
    }
    shareSay(m);
    return;
  }
  if (!s_share || s_post_state || (!s_send_now && (int32_t)(now - s_share_next_ms) < 0)) return;
  s_share_next_ms = now + 5UL * 60UL * 1000UL;
  const bool asked = s_send_now;
  s_send_now = false;
  if (!wifiUp()) { if (asked) shareSay(TR("Couldn't send: not on Wi-Fi")); return; }
  if (!ensurePost()) return;
  if (!mmshare::endpointFor(s_url, s_post->url, sizeof s_post->url)) { shareSay(TR("Couldn't send: that isn't a web address")); return; }
  if (!buildBatch()) { if (asked) shareSay(TR("Nothing new to send")); return; }
  s_post->del = false;
  signPost(mmshare::kSignPrefix);
  shareSay(TR("Sending…"));
  startPost("POST");
}

static void removeMine() {
  if (s_post_state) return;
  if (!wifiUp()) { shareSay(TR("Couldn't send: not on Wi-Fi")); return; }
  if (!ensurePost() || !mmshare::endpointFor(s_url, s_post->url, sizeof s_post->url)) return;
  char radio[65];
  selfHex(radio);
  s_post->len = (size_t)mmshare::deleteBody((char*)s_post->body, kBodyCap, radio, (uint32_t)rtc_clock.getCurrentTime());
  s_post->del = true;
  signPost(mmshare::kDeletePrefix);
  shareSay(TR("Removing…"));
  startPost("DELETE");
}
#else
static void shareNoteRefresh() {
  if (!s_share_note || !lv_obj_is_valid(s_share_note)) { s_share_note = nullptr; return; }
  lv_label_set_text(s_share_note, TR("Not on this board: it can't reach the internet."));
}
static void shareTick(uint32_t) {}
static void removeMine() {}
#endif

static bool recording() { return s_rec; }

// ---- the map ----------------------------------------------------------------------------
static void layerDrawCb(lv_event_t* e) {
  if (!s_ring || !s_n) return;
  lv_obj_t* obj = lv_event_get_target(e);
  lv_draw_ctx_t* dc = lv_event_get_draw_ctx(e);
  lv_area_t a;
  lv_obj_get_coords(obj, &a);
  lv_draw_rect_dsc_t rd;
  lv_draw_rect_dsc_init(&rd);
  rd.radius = LV_RADIUS_CIRCLE;
  rd.bg_opa = LV_OPA_80;
  rd.border_color = lv_color_hex(0x101010);
  rd.border_width = 1;
  rd.border_opa = LV_OPA_COVER;
  const int r = s_map_zoom >= 15 ? 6 : s_map_zoom >= 13 ? 5 : 4;
  const int total = s_n < kRing ? s_n : kRing;
  for (int k = 0; k < total; k++) {   // oldest first, so the newest sits on top
    const mmcov::Sample& s = s_ring[(s_n - total + k) % kRing];
    double wx, wy;
    latLonToWorldPx(s.lat_e6 / 1e6, s.lon_e6 / 1e6, s_map_zoom, &wx, &wy);
    const int sx = (int)(wx - s_cwx + k_map_canvas_w / 2), sy = (int)(wy - s_cwy + k_map_canvas_h / 2);
    if (sx < -r || sx >= k_map_canvas_w + r || sy < -r || sy >= k_map_canvas_h + r) continue;
    rd.bg_color = lv_color_hex(mmcov::reachRgb(s.reach()));
    const lv_area_t d = {(lv_coord_t)(a.x1 + sx - r), (lv_coord_t)(a.y1 + sy - r), (lv_coord_t)(a.x1 + sx + r),
                         (lv_coord_t)(a.y1 + sy + r)};
    lv_draw_rect(dc, &rd, &d);
  }
}

// A tap on the map: the dot nearest it (within 16 px), if any, opens a sheet
// with when it was measured and the MediaMesh repeaters heard there.
static bool mapTap(lv_point_t p) {
  if (!s_layer || !lv_obj_is_valid(s_layer) || !s_ring || !s_n) return false;
  lv_area_t a;
  lv_obj_get_coords(s_layer, &a);
  const int total = s_n < kRing ? s_n : kRing;
  int best = -1, best_d = 16 * 16;
  for (int k = 0; k < total; k++) {   // newest wins a tie: it's drawn on top
    const int at = (s_n - total + k) % kRing;
    const mmcov::Sample& s = s_ring[at];
    double wx, wy;
    latLonToWorldPx(s.lat_e6 / 1e6, s.lon_e6 / 1e6, s_map_zoom, &wx, &wy);
    const int sx = (int)(wx - s_cwx + k_map_canvas_w / 2) + a.x1, sy = (int)(wy - s_cwy + k_map_canvas_h / 2) + a.y1;
    const int dx = sx - p.x, dy = sy - p.y, d = dx * dx + dy * dy;
    if (d <= best_d) { best = at; best_d = d; }
  }
  if (best < 0) return false;
  const mmcov::Sample& s = s_ring[best];
  char body[600];
  int o = 0;
  const uint32_t epoch = s_epoch ? s_epoch[best] : 0;
  const uint32_t now = (uint32_t)rtc_clock.getCurrentTime();
  if (epoch && now >= epoch) {
    const uint32_t ago = now - epoch;
    if (ago < 3600) o += snprintf(body + o, sizeof body - o, "%s %lu min %s\n", TR("Measured"), (unsigned long)(ago / 60), TR("ago"));
    else if (ago < 172800) o += snprintf(body + o, sizeof body - o, "%s %lu h %s\n", TR("Measured"), (unsigned long)(ago / 3600), TR("ago"));
    else o += snprintf(body + o, sizeof body - o, "%s %lu %s\n", TR("Measured"), (unsigned long)(ago / 86400), TR("days ago"));
  }
  if (!s.count) {
    o += snprintf(body + o, sizeof body - o, "%s", TR("No MediaMesh repeater answered here."));
  } else {
    bool listed = false;
    for (int k = 0; s_reps && k < s.count && k < mmcov::kMaxRepeaters && o < (int)sizeof body - 80; k++) {
      const Rep& r = s_reps[best * mmcov::kMaxRepeaters + k];
      if (!r.name[0]) continue;
      listed = true;
      const char* reach = mmcov::reachName(mmcov::reachFor(true, r.margin_q4 / 4.0f));
      o += snprintf(body + o, sizeof body - o, "%s (%s): %+.1f dB %s\n  SNR %.1f dB %s, %.1f dB %s\n", r.name, reach,
                    r.margin_q4 / 4.0f, TR("to spare on radio2"), r.snr_q4 / 4.0f, TR("heard here"),
                    r.remote_q4 / 4.0f, TR("at the repeater"));
    }
    if (!listed)
      o += snprintf(body + o, sizeof body - o, "%u %s, %s %+.1f dB\n", (unsigned)s.count, TR("MediaMesh repeaters"),
                    TR("best"), s.best_q4 / 4.0f);
  }
  (void)o;
  mmcovShowSheet(TR("MediaMesh coverage here"), body);
  return true;
}

static void freeMapObj() {
  if (s_layer) { lv_obj_del(s_layer); s_layer = nullptr; }
}

static void drawOnMap(lv_obj_t* parent, double cwx, double cwy) {
  freeMapObj();
  if (!s_show || !s_n || !s_ring) return;
  s_cwx = cwx;
  s_cwy = cwy;
  s_layer = lv_obj_create(parent);
  lv_obj_remove_style_all(s_layer);
  lv_obj_set_size(s_layer, k_map_canvas_w, k_map_canvas_h);
  lv_obj_set_pos(s_layer, 0, 0);
  lv_obj_clear_flag(s_layer, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(s_layer, layerDrawCb, LV_EVENT_DRAW_MAIN, nullptr);
}

// ---- Settings > MediaMesh -----------------------------------------------------------------
static void buildRows(lv_obj_t* c) {
  prefsLoad();
  setSwitchRow(c, "Map coverage",
               "While on, every 150 m it asks the repeaters in range which have MediaMesh on and maps how well "
               "radio2 reaches them. Needs a GPS fix; the GPS stays on while it records.",
               s_rec, [](lv_event_t* e) {
                 s_rec = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
                 s_have_last = false;
                 s_ask_ms = 0;
                 prefsSave();
               });
  setSwitchRow(c, "Show coverage on the map", nullptr, s_show, [](lv_event_t* e) {
    s_show = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    prefsSave();
    renderMapMarkers();
  });
  s_legend = setNote(c, "");
  legendRefresh();
  setNote(c, "Green: radio2 has 5 dB to spare. Amber: just reaches. Red: the repeater is heard on the main mesh but "
             "radio2 likely won't reach it. Grey: no MediaMesh repeater answered. An estimate from the main-mesh "
             "signal; every point is saved to mediamesh_coverage.csv on the card, the same format as MeshCore Open "
             "TAK exports.");
  // Sharing with an analyzer
  setSwitchRow(c, "Share coverage points",
               "Sends what it records to a CoreScope analyzer over Wi-Fi, so its MediaMesh coverage map is checked "
               "against real measurements.",
               s_share, [](lv_event_t* e) {
                 s_share = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
                 s_share_msg[0] = 0;
                 prefsSave();
                 shareNoteRefresh();
#if MMCOV_SHARE
                 if (s_share) s_send_now = true;
#endif
               });
  lv_obj_t* url = setField(c, "Analyzer", mmshare::kDefaultAnalyzer, sizeof s_url - 1);
  lv_textarea_set_text(url, s_url);
  auto onUrl = [](lv_event_t* e) {
    lv_obj_t* ta = lv_event_get_target(e);
    const char* v = lv_textarea_get_text(ta);
    char ep[200];
    char want[sizeof s_url];
    snprintf(want, sizeof want, "%s", (v && v[0]) ? v : mmshare::kDefaultAnalyzer);
    if (!mmshare::endpointFor(want, ep, sizeof ep)) {
      if (g_lv.task) g_lv.task->showAlert(TR("That isn't a web address"), 2000);
      lv_textarea_set_text(ta, s_url);
      return;
    }
    if (strcmp(want, s_url) != 0) {
      snprintf(s_url, sizeof s_url, "%s", want);
      s_sent_off = 0;   // a new analyzer gets everything
      s_share_msg[0] = 0;
      prefsSave();
      shareNoteRefresh();
    }
  };
  lv_obj_add_event_cb(url, onUrl, LV_EVENT_READY, nullptr);
  lv_obj_add_event_cb(url, onUrl, LV_EVENT_DEFOCUSED, nullptr);
  setNote(c, "Where the points go. Outside CascadiaMesh? Enter your area's analyzer: its upload address, or just its address.");
  s_share_note = setNote(c, "");
  shareNoteRefresh();
#if MMCOV_SHARE
  setButton(c, LV_SYMBOL_UPLOAD, "Send now", [](lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!s_share) { shareSay(TR("Switch on Share coverage points first.")); return; }
    s_send_now = true;
  });
  setButton(c, LV_SYMBOL_CLOSE, "Remove my points from the analyzer", [](lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    showConfirm(TR("Remove every coverage point this standalone sent to the analyzer? The file on the card is kept, and sharing switches off."),
                TR("Remove"), +[]() { removeMine(); });
  }, nullptr, SET_DANGER);
#endif
  setButton(c, LV_SYMBOL_TRASH, "Clear coverage map", [](lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    showConfirm(TR("Clear the coverage map? The file on the card is kept."), TR("Clear"), +[]() {
      s_n = 0;
      s_counts[0] = s_counts[1] = s_counts[2] = s_counts[3] = 0;
      s_have_last = false;
      legendRefresh();
      renderMapMarkers();
    });
  }, nullptr, SET_DANGER);
}

}  // namespace mmcovui

static void mmcovBuildRows(lv_obj_t* card) { mmcovui::buildRows(card); }

#if MMCOV_SHARE
// On the net worker: the request shareTick built.
static void mmcovPostRun() {
  using namespace mmcovui;
  if (!s_post || s_post_state != 1) return;
  s_post->code = forkPostJson(s_post->url, s_post->method, s_post->sig, s_post->body, s_post->len, s_post->resp,
                              sizeof s_post->resp, s_post->msg, sizeof s_post->msg);
  s_post_state = 2;
}
#endif

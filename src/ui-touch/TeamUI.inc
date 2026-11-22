// SPDX-License-Identifier: GPL-3.0-or-later
// Team (MeshCore Open TAK team awareness) on this radio: the glue to the mesh,
// GPS and storage, the map overlay, the alarm, Bloodhound, and the Team page.
// Included by UITask.cpp; the rules live in TeamCore.cpp, the bytes in
// TeamWire.h.

#include <new>

#include "QuickReply.h"
#include "TeamCore.h"
#include "TeamSetup.h"
#include "WatchFeed.h"
#include "TransitFeed.h"
#include "MapHighlight.h"

#if defined(HAS_GPS_MOTION)
extern bool wadaGpsMotion(float* speed_kmh, float* course_deg);   // board target.cpp
#endif

namespace teamui {

#if CAP_TAK_EXTRAS
// Quick replies heard and sent (QuickReply.h), one answer per person per item.
static quickreply::Book* s_book = nullptr;
static quickreply::Book* book() {
  if (!s_book) {
    void* m = heap_caps_malloc(sizeof(quickreply::Book), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (m) s_book = new (m) quickreply::Book();
  }
  return s_book;
}
static char s_reply_toast[96];
static bool s_reply_toast_pending = false;
#endif

// ---- glue ---------------------------------------------------------------------------
class Io : public team::Io {
 public:
  uint32_t nowMs() override { return millis(); }
  uint32_t epoch() override {
    const uint32_t t = the_mesh.getRTCClock()->getCurrentTime();
    return t > 1700000000UL ? t : 0;
  }
  uint32_t random32() override { return esp_random(); }
  void selfKey4(uint8_t out[4]) override { memcpy(out, the_mesh.getSelfPubKey(), 4); }
  const char* nodeName() override { return the_mesh.getNodePrefs()->node_name; }
  bool fix(team::Fix& f) override {
    UITask* t = g_lv.task;
    if (!t) return false;
    const double la = t->getNodeLat(), lo = t->getNodeLon();
    if (la == 0.0 && lo == 0.0) return false;   // no GPS and no position set by hand
    f = team::Fix();
    f.lat = la;
    f.lon = lo;
    f.gps = t->getGPSState() && t->getGpsFix();
    if (f.gps) {
      f.has_alt = true;
      f.alt_m = t->getGpsAltitude();
      f.accuracy_m = 10;
#if defined(HAS_GPS_MOTION)
      float spd = NAN, crs = NAN;
      if (wadaGpsMotion(&spd, &crs) && !isnan(spd) && !isnan(crs)) {
        f.has_motion = true;
        f.speed_mps = spd / 3.6f;
        f.heading_deg = crs;
      }
#endif
    }
    return true;
  }
  int batteryPct() override {
    const uint16_t mv = batteryMvSmoothed();
    return mv ? batteryPercentFromMv(mv) : -1;
  }
  bool channelOk(const char* name) override { return channelIdx(name) >= 0; }
  bool send(const char* name, const uint8_t* d, size_t n) override {
    const int idx = channelIdx(name);
    return idx >= 0 && the_mesh.voxSendChannelData(idx, TeamWire::kDataType, d, n);
  }
  bool readFile(const char* name, uint8_t* buf, size_t cap, size_t* len) override {
    fs::FS* fs = backupInternalFs();
    char path[24];
    snprintf(path, sizeof path, "/%s", name);
    if (!fs || !fs->exists(path)) return false;
    File f = fs->open(path, "r");
    if (!f) return false;
    const size_t n = f.size();
    if (n > cap) { f.close(); return false; }
    *len = f.read(buf, n);
    f.close();
    return *len == n;
  }
  bool writeFile(const char* name, const uint8_t* d, size_t n) override {
    fs::FS* fs = backupInternalFs();
    if (!fs) return false;
    char path[24];
    snprintf(path, sizeof path, "/%s", name);
    WdtHeavyGuard guard;
    File f = fs->open(path, "w");
    if (!f) return false;
    const size_t w = f.write(d, n);
    f.close();
    return w == n;
  }
  void changed() override { s_version++; }
  void alarm(const team::AlertRec&) override { s_alarm_pending = true; }
#if CAP_TAK_EXTRAS
  // A teammate's quick reply: kept for the item's sheet, and a toast when it
  // answers something of ours.
  void reply(const TeamWire::Reply& r) override {
    quickreply::Book* bk = book();
    if (!bk) return;
    char ref[28], who[21];
    quickreply::refTeam(r.ref, r.to4, r.ref_id, ref, sizeof ref);
    if (r.callsign[0]) snprintf(who, sizeof who, "%s", r.callsign);
    else snprintf(who, sizeof who, "%02X%02X%02X%02X", r.key4[0], r.key4[1], r.key4[2], r.key4[3]);
    bk->add(ref, who, r.code, r.arg, nullptr, millis(), false);
    uint8_t me[4];
    selfKey4(me);
    if (!memcmp(r.to4, me, 4)) {
      char w[24];
      quickreply::words(r.code, r.arg, w, sizeof w);
      snprintf(s_reply_toast, sizeof s_reply_toast, "%s: %s", who, w);
      s_reply_toast_pending = true;
    }
    s_version++;
  }
#endif
  // MeshCore TEAM interop (MESHCORE_TEAM.md).
  bool sendText(const char* name, const char* text) override {
    const int idx = channelIdx(name);
    ChannelDetails cd;
    if (idx < 0 || !the_mesh.getChannel(idx, cd)) return false;
    const uint32_t ts = the_mesh.getRTCClock()->getCurrentTimeUnique();
    const char* me = the_mesh.getNodePrefs()->node_name;
    return sendGroupMessageScoped(idx, cd, ts, me && *me ? me : "me", text);   // not into our chat
  }
  int batteryMv() override {
    const uint16_t mv = batteryMvSmoothed();
    return mv ? (int)mv : -1;
  }
  void advert() override { the_mesh.sendAdvert(true); }
  bool contactKey4(const char* name, uint8_t out[4]) override {
    const int nc = the_mesh.getNumContacts();
    for (int k = 0; k < nc; k++) {
      ContactInfo c;
      if (the_mesh.getContactByIdx((uint32_t)k, c) && !strcmp(c.name, name)) {
        memcpy(out, c.id.pub_key, 4);
        return true;
      }
    }
    return false;
  }
  bool selfKey6(uint8_t out[6]) override { memcpy(out, the_mesh.getSelfPubKey(), 6); return true; }

  static int channelIdx(const char* name) {
    if (!name || !*name) return -1;
    for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
      ChannelDetails cd;
      if (the_mesh.getChannel(i, cd) && cd.name[0] && !strcmp(cd.name, name)) return i;
    }
    return -1;
  }
  static uint32_t s_version;
  static bool s_alarm_pending;
};
uint32_t Io::s_version = 0;
bool Io::s_alarm_pending = false;

static Io s_io;
static team::Core* s_core = nullptr;

static team::Core* core() {
  if (!s_core) {
    void* m = heap_caps_malloc(sizeof(team::Core), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!m) return nullptr;
    s_core = new (m) team::Core(&s_io);
    s_core->begin();
  }
  return s_core;
}

// Watched channels: reports with a position on chosen channels (IceBot, a
// drone detector...) go on the map for a while. Independent of team mode.
static watchfeed::Store* s_watch = nullptr;

static watchfeed::Store* watch() {
  if (!s_watch) {
    void* m = heap_caps_malloc(sizeof(watchfeed::Store), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!m) return nullptr;
    s_watch = new (m) watchfeed::Store();
    char buf[300];
    size_t n = 0;
    if (s_io.readFile("teamwatch", (uint8_t*)buf, sizeof buf - 1, &n)) {
      buf[n] = 0;
      s_watch->load(buf);
    }
    // #icewatch is watched by default, to show what watching does (as the
    // app): added once, on new radios and existing ones alike, then it's an
    // ordinary choice that can be turned off. It only matters with that
    // channel added. The "once" is kept in "watchdef".
    uint8_t applied = 0;
    size_t an = 0;
    if (!s_io.readFile("watchdef", &applied, 1, &an) || an != 1 || applied != 1) {
      s_watch->setWatched("#icewatch", true);
      const size_t m = s_watch->save(buf, sizeof buf);
      s_io.writeFile("teamwatch", (const uint8_t*)buf, m);
      applied = 1;
      s_io.writeFile("watchdef", &applied, 1);
    }
  }
  return s_watch;
}

// Transit bots' replies on watched channels (TransitFeed.h): the joined text
// of each one on the map, by its watch item, to lay out its card; a reply's
// first part while the rest (with the stop's position) is on its way; and a
// parsed reply each for the mesh side and the UI. All in PSRAM, made on the
// first transit reply.
static char (*s_tx)[transitfeed::kTextCap] = nullptr;
struct TxPending {
  int8_t ch = -1;
  uint32_t ms = 0, sent = 0;
  char sender[40] = {0};
  char text[transitfeed::kTextCap] = {0};
};
static TxPending* s_txp = nullptr;
static transitfeed::Reply* s_txr = nullptr;      // the mesh side's
static transitfeed::Reply* s_txr_ui = nullptr;   // the Watch page's

static bool txAlloc() {
  if (!s_tx)
    s_tx = (char (*)[transitfeed::kTextCap])heap_caps_calloc(watchfeed::kMaxItems, transitfeed::kTextCap,
                                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!s_txp) {
    void* m = heap_caps_malloc(sizeof(TxPending), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (m) s_txp = new (m) TxPending();
  }
  if (!s_txr) {
    void* m = heap_caps_malloc(sizeof(transitfeed::Reply), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (m) s_txr = new (m) transitfeed::Reply();
  }
  if (!s_txr_ui) {
    void* m = heap_caps_malloc(sizeof(transitfeed::Reply), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (m) s_txr_ui = new (m) transitfeed::Reply();
  }
  return s_tx && s_txp && s_txr && s_txr_ui;
}

// Watch item [i]'s transit reply, or nullptr when it isn't one.
static const char* txText(int i) {
  return s_tx && i >= 0 && i < watchfeed::kMaxItems && s_tx[i][0] ? s_tx[i] : nullptr;
}
static void txSet(int i, const char* t) {
  if (!s_tx || i < 0 || i >= watchfeed::kMaxItems) return;
  watchfeed::copyN(s_tx[i], transitfeed::kTextCap, t ? t : "", t ? strlen(t) : 0);
}

// Watch item [i]'s card text: [full] for its sheet, else the list's few lines.
// Countdowns run from when the reply was sent. False when it isn't transit.
static bool txDescribe(const watchfeed::Item& it, int i, bool full, char* out, size_t cap) {
  const char* t = txText(i);
  if (!t || !s_txr_ui) return false;
  ChannelDetails cd;
  const bool posts = the_mesh.getChannel(it.channel, cd) && transitfeed::transitChannel(cd.name);
  if (!transitfeed::parse(t, *s_txr_ui, posts)) return false;
  const uint32_t elapsed = (millis() - it.at_ms) / 1000;
  const uint32_t now_epoch = s_io.epoch();
  transitfeed::describe(*s_txr_ui, elapsed, now_epoch > elapsed ? now_epoch - elapsed : 0, full, out, cap);
  return true;
}

// New points flash on the map (MapHighlight.h); the time is kept in "maphl".
static maphl::Table* s_hl = nullptr;
static uint32_t s_hl_until = 0;     // the next highlight to run out (millis)
static bool s_hl_active = false;

static maphl::Table* highlights() {
  if (!s_hl) {
    void* m = heap_caps_malloc(sizeof(maphl::Table), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!m) return nullptr;
    s_hl = new (m) maphl::Table();
    uint8_t v = 0;
    size_t n = 0;
    if (s_io.readFile("maphl", &v, 1, &n) && n == 1) s_hl->choice = (uint8_t)(v % maphl::kChoices);
  }
  return s_hl;
}

// Auto-zoom (as the app's map button, lib/team/watch_auto_zoom.dart): a watched
// report with a position that arrives live moves the map to it, zoomed in to at
// least street level, while the map is on screen. A channel's backlog or a
// report older than 30 minutes never moves it. On unless turned off; kept in
// "watchzoom".
static int8_t s_watch_zoom = -1;      // -1: not read yet
static bool s_zoom_pending = false;
static double s_zoom_lat = 0, s_zoom_lon = 0;
static constexpr uint8_t kWatchZoomLevel = 15;

static bool watchZoomOn() {
  if (s_watch_zoom < 0) {
    uint8_t v = 1;
    size_t n = 0;
    s_watch_zoom = (s_io.readFile("watchzoom", &v, 1, &n) && n == 1) ? (v ? 1 : 0) : 1;
  }
  return s_watch_zoom == 1;
}
static void watchZoomSet(bool on) {
  s_watch_zoom = on ? 1 : 0;
  const uint8_t v = on ? 1 : 0;
  s_io.writeFile("watchzoom", &v, 1);
}

static void highlightsSave() {
  if (!s_hl) return;
  const uint8_t v = s_hl->choice;
  s_io.writeFile("maphl", &v, 1);
}

static void watchSave() {
  if (!s_watch) return;
  char buf[300];
  const size_t n = s_watch->save(buf, sizeof buf);
  s_io.writeFile("teamwatch", (const uint8_t*)buf, n);
}

static const char* channelName(int idx) {
  static ChannelDetails cd;
  if (idx == 0xFF) return "this standalone";   // heard here (Drone Detector)
  return (idx >= 0 && the_mesh.getChannel(idx, cd) && cd.name[0]) ? cd.name : "?";
}

// The team's name for a role (a few rotating buffers, for use in one printf).
static const char* roleStr(int id) {
  static char b[4][TeamWire::kMaxRoleName + 4];
  static uint8_t k = 0;
  char* o = b[k++ & 3];
  if (s_core) s_core->roleName(id, o, sizeof b[0]);
  else snprintf(o, sizeof b[0], "%s", TeamWire::roleName(id));
  return o;
}

static void toast(const char* m, int ms = 1800) { if (g_lv.task) g_lv.task->showAlert(m, ms); }

// ---- colour: the firmware theme's accent instead of plain grey ---------------------------
// Buttons are tinted with the theme's accent, a card takes its own colour (a
// teammate's, a pin's kind, a report's level) or else the accent, and the chosen
// tab or channel is filled with it. E-paper and high contrast keep the plain look.
static bool plain() {
#if defined(HAS_TDECK_PRO)
  return true;
#else
  return s_theme_high_contrast;
#endif
}
static bool isCol(lv_color_t c, uint32_t hex) { return c.full == lv_color_hex(hex).full; }
// Readable text on a fill.
static uint32_t textOn(uint32_t c) {
  const uint32_t r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
  return (r * 299 + g * 587 + b * 114) / 1000 > 150 ? 0x14171C : 0xFFFFFF;
}
// A button filled with a colour of its own (Hound, Delete, the chosen tab).
static void fill(lv_obj_t* o, uint32_t c) {
  lv_obj_set_style_bg_color(o, lv_color_hex(c), LV_PART_MAIN);
  lv_obj_set_style_bg_color(o, lv_color_hex(rgbMix(c, 0x000000, 50)), LV_PART_MAIN | LV_STATE_PRESSED);
  lv_obj_set_style_text_color(o, lv_color_hex(textOn(c)), LV_PART_MAIN);
}
static void selected(lv_obj_t* o) { fill(o, plain() ? COLOR_CONTROL_PRESSED : COLOR_GLOW); }
// Popups, sheets and the alarm: solid, tinted with the accent (or a card's own colour).
static void tintSolid(lv_obj_t* o) {
  if (!o || plain()) return;
  const uint32_t a = COLOR_GLOW;
  if (lv_obj_get_style_bg_opa(o, LV_PART_MAIN) > LV_OPA_50) {
    const lv_color_t bg = lv_obj_get_style_bg_color(o, LV_PART_MAIN);
    if (isCol(bg, COLOR_CONTROL)) {
      lv_obj_set_style_bg_color(o, lv_color_hex(rgbMix(COLOR_CONTROL, a, 72)), LV_PART_MAIN);
      lv_obj_set_style_bg_color(o, lv_color_hex(rgbMix(COLOR_CONTROL_PRESSED, a, 120)), LV_PART_MAIN | LV_STATE_PRESSED);
      lv_obj_set_style_border_color(o, lv_color_hex(rgbMix(COLOR_BORDER, a, 150)), LV_PART_MAIN);
    } else if (isCol(bg, COLOR_PANEL)) {
      const lv_color_t bc = lv_obj_get_style_border_color(o, LV_PART_MAIN);
      const bool own = lv_obj_get_style_border_width(o, LV_PART_MAIN) >= 2 && !isCol(bc, COLOR_BORDER);
      const uint32_t c = own ? (lv_color_to32(bc) & 0xFFFFFF) : a;
      lv_obj_set_style_bg_color(o, lv_color_hex(rgbMix(COLOR_PANEL, c, own ? 44 : 26)), LV_PART_MAIN);
      if (!own) lv_obj_set_style_border_color(o, lv_color_hex(rgbMix(COLOR_BORDER, a, 110)), LV_PART_MAIN);
    }
  }
  const uint32_t n = lv_obj_get_child_cnt(o);
  for (uint32_t i = 0; i < n; i++) tintSolid(lv_obj_get_child(o, (int32_t)i));
}
// A page (Team, Flock You, Drones, Sound link): the fork's glass look (ForkLook.inc).
static void tint(lv_obj_t* o) { forklook::dress(o); }

static bool myFix(double* lat, double* lon) {
  team::Fix f;
  if (!s_io.fix(f)) return false;
  *lat = f.lat;
  *lon = f.lon;
  return true;
}

// Whether the channel named [name] is the Public channel (its well-known key).
static bool threadIsPublicChannel(const char* name) {
  static const uint8_t kPublic[16] = {0x8b, 0x33, 0x87, 0xe9, 0xc5, 0xcd, 0xea, 0x6a,
                                      0xc9, 0xe5, 0xed, 0xba, 0xa1, 0x15, 0xcd, 0x72};
  if (!name || !*name) return false;
  for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
    ChannelDetails cd;
    if (!the_mesh.getChannel(i, cd) || strcmp(cd.name, name) != 0) continue;
    return memcmp(cd.channel.secret, kPublic, sizeof kPublic) == 0;
  }
  return false;
}

static void rangeTo(double lat, double lon, char* out, size_t cap) {
  double ml, mo;
  if (!myFix(&ml, &mo)) { snprintf(out, cap, "%.5f, %.5f", lat, lon); return; }
  TeamWire::rangeBearing(TeamWire::distanceM(ml, mo, lat, lon), TeamWire::bearingDeg(ml, mo, lat, lon), out, cap);
}

// A teammate's range and bearing, or that they have no position (no GPS fix
// or location set: they're in the list, not on the map).
static void memberRange(const team::Member& m, char* out, size_t cap) {
  if (!m.pos.has_pos) { snprintf(out, cap, "%s", TR("no position")); return; }
  rangeTo(m.pos.lat, m.pos.lon, out, cap);
}

// A sheet's "Map" (teammates, pins, watched reports...): the map centred on the
// point, zoomed in to street level if it was further out (as the On map list
// does), so the point itself is what's in view.
static void showOnMap(double lat, double lon) { mapJumpTo(lat, lon, 14); }

#if CAP_TAK_EXTRAS
// ---- quick replies: what the answer buttons answer -----------------------------------------
// Set by the sheet (or the alarm) that shows the buttons; the buttons send.
static uint8_t s_rq_kind = 0;          // TeamWire::kRefAlert/Report/Pin, or kRqWatch
static constexpr uint8_t kRqWatch = 9;
static uint8_t s_rq_to4[4] = {0};
static uint32_t s_rq_id = 0;
static uint8_t s_rq_ch = 0;            // a watched report's channel
static uint16_t s_rq_hash = 0;         // and its hash
static void rqSend(uint8_t code, uint8_t arg);
static void rqEtaSheet();
#endif

// ---- alarm (a teammate's SOS) ------------------------------------------------------------
static lv_obj_t* s_alarm = nullptr;
static team::AlertRec* s_alarm_rec = nullptr;
static uint32_t s_alarm_beep_ms = 0;

static void alarmClose() {
  if (s_alarm) popupClose(&s_alarm);
  s_alarm_rec = nullptr;
}

static void alarmShow(team::AlertRec& r) {
  alarmClose();
  s_alarm_rec = &r;
  const lv_coord_t sw = lv_disp_get_hor_res(nullptr), sh = lv_disp_get_ver_res(nullptr);
  s_alarm = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(s_alarm);
  lv_obj_set_size(s_alarm, sw, sh);
  lv_obj_set_style_bg_color(s_alarm, lv_color_hex(0x7A0D12), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_alarm, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_add_flag(s_alarm, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(s_alarm, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(s_alarm, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(s_alarm, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_all(s_alarm, 10, LV_PART_MAIN);
  lv_obj_set_style_pad_row(s_alarm, 8, LV_PART_MAIN);
  char line[160];
  snprintf(line, sizeof line, LV_SYMBOL_WARNING " %s", TeamWire::alertName(r.a.type));
  lv_obj_t* t = lv_label_create(s_alarm);
  lv_label_set_text(t, line);
  lv_obj_set_style_text_font(t, &lv_font_montserrat_28, LV_PART_MAIN);
  lv_obj_set_style_text_color(t, lv_color_white(), LV_PART_MAIN);
  char who[64], where[48];
  snprintf(who, sizeof who, "%s", r.a.callsign[0] ? r.a.callsign : "Teammate");
  if (r.a.has_pos) rangeTo(r.a.lat, r.a.lon, where, sizeof where);
  else snprintf(where, sizeof where, "%s", TR("No position"));
  snprintf(line, sizeof line, "%s\n%s%s%s", who, where, r.a.note[0] ? "\n" : "", r.a.note);
  lv_obj_t* d = lv_label_create(s_alarm);
  lv_label_set_text(d, line);
  lv_obj_set_style_text_align(d, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_style_text_font(d, &g_font_16, LV_PART_MAIN);
  lv_obj_set_style_text_color(d, lv_color_white(), LV_PART_MAIN);
  useChainedFont(d);
  lv_obj_t* row = lv_obj_create(s_alarm);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(row, 8, LV_PART_MAIN);
  auto mk = [&](const char* txt, lv_event_cb_t cb) {
    lv_obj_t* b = lv_btn_create(row);
    styleButton(b);
    tintSolid(b);
    lv_obj_set_size(b, LV_SIZE_CONTENT, 36);
    lv_obj_set_style_pad_hor(b, 12, LV_PART_MAIN);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* l = lv_label_create(b);
    lv_label_set_text(l, txt);
    useChainedFont(l);
    lv_obj_center(l);
  };
  mk(TR("Acknowledge"), [](lv_event_t*) {
    if (s_alarm_rec && s_core) s_core->acknowledge(*s_alarm_rec);
    alarmClose();
  });
#if CAP_TAK_EXTRAS
  // One tap answers the teammate and acknowledges (QuickReply.h).
  {
    lv_obj_t* qrow = lv_obj_create(s_alarm);
    lv_obj_remove_style_all(qrow);
    lv_obj_set_size(qrow, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(qrow, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(qrow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(qrow, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_row(qrow, 6, LV_PART_MAIN);
    for (uint8_t code : quickreply::kAlertCodes) {
      lv_obj_t* b = lv_btn_create(qrow);
      styleButton(b);
      fill(b, 0xB71C1C);
      lv_obj_set_size(b, LV_SIZE_CONTENT, 32);
      lv_obj_set_style_pad_hor(b, 10, LV_PART_MAIN);
      lv_obj_set_style_border_color(b, lv_color_white(), LV_PART_MAIN);
      lv_obj_set_style_border_width(b, 1, LV_PART_MAIN);
      lv_obj_add_event_cb(b, [](lv_event_t* e) {
        if (!s_alarm_rec || !s_core) return;
        const uint8_t code = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
        s_rq_kind = TeamWire::kRefAlert;
        memcpy(s_rq_to4, s_alarm_rec->a.key4, 4);
        s_rq_id = s_alarm_rec->a.id;
        s_core->acknowledge(*s_alarm_rec);
        alarmClose();
        if (code == quickreply::kEta) rqEtaSheet();
        else rqSend(code, 0);
      }, LV_EVENT_CLICKED, (void*)(uintptr_t)code);
      lv_obj_t* l = lv_label_create(b);
      lv_label_set_text(l, code == quickreply::kEta ? TR("ETA...") : TR(quickreply::english(code)));
      lv_obj_set_style_text_color(l, lv_color_white(), LV_PART_MAIN);
      useChainedFont(l);
      lv_obj_center(l);
    }
  }
#endif
  if (r.a.has_pos) {
    mk(TR("Hound"), [](lv_event_t*) {
      if (s_alarm_rec && s_core) {
        s_core->houndPoint(s_alarm_rec->a.lat, s_alarm_rec->a.lon, s_alarm_rec->a.callsign);
        s_core->acknowledge(*s_alarm_rec);
      }
      alarmClose();
    });
    mk(TR("Map"), [](lv_event_t*) {
      if (!s_alarm_rec || !s_core) return;
      const double la = s_alarm_rec->a.lat, lo = s_alarm_rec->a.lon;
      s_core->acknowledge(*s_alarm_rec);
      alarmClose();
      showOnMap(la, lo);
    });
  }
  lv_obj_move_foreground(s_alarm);
  s_alarm_beep_ms = 0;
}

// ---- Bloodhound strip ----------------------------------------------------------------------
static lv_obj_t* s_hound = nullptr;
static lv_obj_t* s_hound_lbl = nullptr;
static lv_obj_t* s_hound_arrow = nullptr;
static lv_point_t s_hound_pts[5];

static void houndRefresh() {
  team::Core* c = s_core;
  double la, lo, ml, mo;
  char label[33];
  const bool on = c && c->hound(&la, &lo, label, sizeof label);
  if (!on) {
    if (s_hound) popupClose(&s_hound);
    s_hound_lbl = s_hound_arrow = nullptr;
    return;
  }
  if (!s_hound) {
    s_hound = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_hound);
    lv_obj_set_size(s_hound, 168, 40);
    lv_obj_align(s_hound, LV_ALIGN_BOTTOM_RIGHT, -4, -46);
    styleSurface(s_hound, 0x101418, 8);
    lv_obj_set_style_bg_opa(s_hound, LV_OPA_80, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_hound, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_hound, lv_color_hex(0xF2A33A), LV_PART_MAIN);
    lv_obj_add_flag(s_hound, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_hound, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_hound, [](lv_event_t*) {
      showConfirm(s_core && s_core->following() ? TR("Stop following the route?") : TR("Stop Bloodhound?"), TR("Stop"),
                  +[]() { if (s_core) s_core->stopHound(); houndRefresh(); renderMapMarkers(); });
    }, LV_EVENT_CLICKED, nullptr);
    s_hound_arrow = lv_line_create(s_hound);
    lv_obj_set_style_line_width(s_hound_arrow, 3, LV_PART_MAIN);
    lv_obj_set_style_line_color(s_hound_arrow, lv_color_hex(0xF2A33A), LV_PART_MAIN);
    lv_obj_set_style_line_rounded(s_hound_arrow, true, LV_PART_MAIN);
    lv_obj_set_pos(s_hound_arrow, 0, 0);
    s_hound_lbl = lv_label_create(s_hound);
    lv_obj_set_style_text_font(s_hound_lbl, &g_font_12, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_hound_lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_pos(s_hound_lbl, 40, 3);
    lv_obj_set_width(s_hound_lbl, 124);
    lv_label_set_long_mode(s_hound_lbl, LV_LABEL_LONG_DOT);
  }
  char txt[128];
  double rel = 0;
  // Following a route: taller, with the next turn under the name.
  const bool route = c->following();
  lv_obj_set_size(s_hound, route ? 200 : 168, route ? 54 : 40);
  lv_obj_set_width(s_hound_lbl, route ? 156 : 124);
  lv_label_set_long_mode(s_hound_lbl, route ? LV_LABEL_LONG_WRAP : LV_LABEL_LONG_DOT);
  if (myFix(&ml, &mo)) {
    char rb[40];
    const double brg = TeamWire::bearingDeg(ml, mo, la, lo);
    TeamWire::rangeBearing(TeamWire::distanceM(ml, mo, la, lo), brg, rb, sizeof rb);
    if (route) {
      char cue[72];
      c->updateFollow();
      teamroute::cueText(c->cue(), cue, sizeof cue);
      snprintf(txt, sizeof txt, "%s\n%s", label, cue);
    } else {
      snprintf(txt, sizeof txt, "%s\n%s", label, rb);
    }
    rel = brg;   // relative to north: only the M9 has a compass
#if defined(HAS_GPS_MOTION)
    float spd = NAN, crs = NAN;
    if (wadaGpsMotion(&spd, &crs) && !isnan(crs) && spd > 2.0f) rel = brg - crs;   // to travel direction
#endif
  } else {
    snprintf(txt, sizeof txt, "%s\n%s", label, TR("No own position"));
  }
  lv_label_set_text(s_hound_lbl, txt);
  // Arrow: shaft and head around (20, 20), rotated to `rel`.
  const double a = rel * M_PI / 180.0, s = sin(a), co = cos(a);
  auto pt = [&](double x, double y, int i) {   // (x right, y up) before rotation
    s_hound_pts[i].x = (lv_coord_t)lround(20 + x * co + y * s);
    s_hound_pts[i].y = (lv_coord_t)lround(20 + x * s - y * co);
  };
  pt(0, -13, 0); pt(0, 13, 1); pt(-7, 5, 2); pt(0, 13, 3); pt(7, 5, 4);
  lv_line_set_points(s_hound_arrow, s_hound_pts, 5);
  lv_obj_move_foreground(s_hound);
}

// ---- map overlay ---------------------------------------------------------------------------
constexpr int kMapObjs = 256;
static auto& s_map_objs = psramStatic<lv_obj_t*[kMapObjs]>();
static int s_map_obj_n = 0;
struct Hit { int16_t x, y; uint8_t kind; uint8_t idx; };   // kind 0 member, 1 pin, 2 watched report, 3 shared pin
static auto& s_hits = psramStatic<Hit[kMapObjs]>();
static int s_hit_n = 0;

static void mapTrack(lv_obj_t* o) { if (s_map_obj_n < kMapObjs) s_map_objs[s_map_obj_n++] = o; else lv_obj_del(o); }

static void mapFree() {
  for (int i = 0; i < s_map_obj_n; i++) if (s_map_objs[i]) lv_obj_del(s_map_objs[i]);
  s_map_obj_n = 0;
  s_hit_n = 0;
}

static lv_obj_t* mapDot(lv_obj_t* parent, int sx, int sy, int size, uint32_t fill, uint32_t border, int radius, lv_opa_t opa) {
  lv_obj_t* d = lv_obj_create(parent);
  lv_obj_remove_style_all(d);
  lv_obj_set_size(d, size, size);
  lv_obj_set_pos(d, sx - size / 2, sy - size / 2);
  lv_obj_set_style_bg_color(d, lv_color_hex(fill), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(d, opa, LV_PART_MAIN);
  lv_obj_set_style_border_color(d, lv_color_hex(border), LV_PART_MAIN);
  lv_obj_set_style_border_width(d, 2, LV_PART_MAIN);
  lv_obj_set_style_radius(d, radius, LV_PART_MAIN);
  lv_obj_clear_flag(d, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  mapTrack(d);
  return d;
}

// One of the app's map icons (map_icons.h) centred on a map dot, optionally
// turned by [deg] (a heading).
static lv_obj_t* mapIcon(lv_obj_t* dot, const char* glyph, uint32_t color, float deg = 0) {
  lv_obj_t* l = lv_label_create(dot);
  lv_label_set_text(l, glyph);
  lv_obj_set_style_text_font(l, &map_icons_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(l, lv_color_hex(color), LV_PART_MAIN);
  lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_center(l);
  if (deg != 0) {
    lv_obj_set_style_transform_pivot_x(l, 7, LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_y(l, 7, LV_PART_MAIN);
    lv_obj_set_style_transform_angle(l, (int16_t)((int)(deg * 10) % 3600), LV_PART_MAIN);
  }
  return l;
}

// Black or white, whichever reads on [rgb] (the app's luminance test).
static uint32_t onColor(uint32_t rgb) {
  const float r = ((rgb >> 16) & 0xFF) / 255.0f, g = ((rgb >> 8) & 0xFF) / 255.0f, b = (rgb & 0xFF) / 255.0f;
  return 0.2126f * r + 0.7152f * g + 0.0722f * b > 0.5f ? 0x000000 : 0xFFFFFF;
}

static void mapLabelNow(lv_obj_t* parent, int sx, int sy, const char* text, uint32_t color) {
  lv_obj_t* l = lv_label_create(parent);
  lv_label_set_text(l, text);
  lv_obj_set_style_text_font(l, &g_font_12, LV_PART_MAIN);
  lv_obj_set_style_text_color(l, lv_color_hex(color), LV_PART_MAIN);
  lv_obj_set_style_bg_color(l, lv_color_hex(0x101418), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(l, LV_OPA_60, LV_PART_MAIN);
  lv_obj_set_style_pad_hor(l, 2, LV_PART_MAIN);
  lv_obj_set_style_radius(l, 3, LV_PART_MAIN);
  lv_obj_set_pos(l, sx + 9, sy - 8);
  lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
  mapTrack(l);
}

// Labels go up after every point is drawn, most important first, and a label
// that would cover one already placed is left out: zoomed out, points keep
// their names ("ICE Kent Pacific Hwy S") until there are too many to read.
// prio: 0 teammates, 1 drones and quakes, 2 watched reports, 3 pins,
// 4 routes and drone operators. Within one, newer first (`order`).
struct PendingLabel { int16_t x, y; uint8_t prio; uint32_t color, order; char text[64]; };
static constexpr int kMaxLabels = 96;
static auto& s_lbls = psramStatic<PendingLabel[kMaxLabels]>();
static int s_lbl_n = 0;

static void mapLabel(lv_obj_t*, int sx, int sy, const char* text, uint32_t color, uint8_t prio = 3,
                     uint32_t order = 0) {
  if (!text || !*text || s_lbl_n >= kMaxLabels) return;
  if (sx < -40 || sx > k_map_canvas_w || sy < -10 || sy > k_map_canvas_h + 10) return;
  PendingLabel& l = s_lbls[s_lbl_n++];
  l.x = (int16_t)sx;
  l.y = (int16_t)sy;
  l.prio = prio;
  l.color = color;
  l.order = order;
  snprintf(l.text, sizeof l.text, "%s", text);
}

static void mapLabelsPlace(lv_obj_t* parent) {
  int idx[kMaxLabels];
  for (int i = 0; i < s_lbl_n; i++) idx[i] = i;
  for (int i = 1; i < s_lbl_n; i++)   // by priority, then newest
    for (int j = i; j > 0; j--) {
      const PendingLabel& a = s_lbls[idx[j]];
      const PendingLabel& b = s_lbls[idx[j - 1]];
      if (a.prio > b.prio || (a.prio == b.prio && a.order <= b.order)) break;
      const int t = idx[j]; idx[j] = idx[j - 1]; idx[j - 1] = t;
    }
  lv_area_t placed[kMaxLabels];
  int n = 0;
  for (int k = 0; k < s_lbl_n; k++) {
    const PendingLabel& l = s_lbls[idx[k]];
    lv_point_t sz;
    lv_txt_get_size(&sz, l.text, &g_font_12, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lv_area_t a;
    a.x1 = (lv_coord_t)(l.x + 9 - 2);
    a.y1 = (lv_coord_t)(l.y - 8 - 1);
    a.x2 = (lv_coord_t)(a.x1 + sz.x + 4 + 2);
    a.y2 = (lv_coord_t)(a.y1 + sz.y + 2);
    bool clash = false;
    for (int q = 0; q < n && !clash; q++) clash = _lv_area_is_on(&a, &placed[q]);
    if (clash) continue;
    placed[n++] = a;
    mapLabelNow(parent, l.x, l.y, l.text, l.color);
  }
  s_lbl_n = 0;
}

// A new point's highlight: a ring in [rgb] that flashes (its border fades
// in and out), under the point's own dot. Deleted with the map objects.
// As the app's NewPointAura: a ring that swells from the point and fades as it
// goes, over and over (1.2 s, eased out). The ring keeps its centre on the point
// as it grows: the centre rides in the object's user data.
static constexpr int kRingFrom = 20, kRingTo = 56;   // diameter, px
static void mapRing(lv_obj_t* parent, int sx, int sy, uint32_t rgb) {
  lv_obj_t* d = mapDot(parent, sx, sy, kRingFrom, rgb, rgb, LV_RADIUS_CIRCLE, LV_OPA_30);
  lv_obj_set_style_border_width(d, 3, LV_PART_MAIN);
  lv_obj_set_user_data(d, (void*)(intptr_t)(((uint32_t)(uint16_t)sx << 16) | (uint16_t)sy));
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, d);
  lv_anim_set_values(&a, 0, 1000);
  lv_anim_set_time(&a, 1200);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_exec_cb(&a, [](void* p, int32_t v) {   // v: 0..1000 of the swell
    lv_obj_t* o = (lv_obj_t*)p;
    const uint32_t c = (uint32_t)(uintptr_t)lv_obj_get_user_data(o);
    const int cx = (int16_t)(c >> 16), cy = (int16_t)(c & 0xFFFF);
    const int size = kRingFrom + (kRingTo - kRingFrom) * v / 1000;
    lv_obj_set_size(o, size, size);
    lv_obj_set_pos(o, cx - size / 2, cy - size / 2);
    lv_obj_set_style_bg_opa(o, (lv_opa_t)(71 * (1000 - v) / 1000), LV_PART_MAIN);         // 0.28 -> 0
    lv_obj_set_style_border_opa(o, (lv_opa_t)(255 - 204 * v / 1000), LV_PART_MAIN);     // 1 -> 0.2
  });
  lv_anim_start(&a);
}

// Whether a point should flash now (see MapHighlight.h), noting when the
// next highlight runs out so the map is redrawn then.
static bool mapIsNew(uint32_t key, uint32_t now, uint32_t age_ms) {
  maphl::Table* t = highlights();
  if (!t) return false;
  uint32_t until = s_hl_active ? s_hl_until : now + 0x7FFFFFFFUL;
  if (!t->isNew(key, now, age_ms, &until)) return false;
  s_hl_until = until;
  s_hl_active = true;
  return true;
}

static constexpr uint32_t kNewPointRgb = 0x00E5FF;   // as the app's newPointAuraColor

// Points' keys, as the app's highlightPointsOf: a drone is one point however
// often it reports (its serial); a teammate is their key.
static uint32_t hlKeyWatch(const watchfeed::Item& it) {
  if (it.r.track[0]) return maphl::key('d', it.r.track, strlen(it.r.track), maphl::key('c', &it.channel, 1));
  const int32_t pos[2] = {(int32_t)lround(it.r.lat * 1e5), (int32_t)lround(it.r.lon * 1e5)};
  return maphl::key('w', pos, sizeof pos, maphl::key('t', &it.at_ms, sizeof it.at_ms));
}
static uint32_t hlKeyMarker(const TeamWire::Marker& m) {
  return maphl::key('p', m.key4, 4, maphl::key('i', &m.id, sizeof m.id));
}
static uint32_t hlKeyMember(const team::Member& m) { return maphl::key('m', m.pos.key4, 4); }
static uint32_t hlAge(uint32_t now, uint32_t at) { return (int32_t)(now - at) > 0 ? now - at : 0; }
static uint32_t hlMarkerAge(const TeamWire::Marker& m) {
  const uint32_t epoch = s_io.epoch();
  return epoch && m.time && epoch >= m.time ? (epoch - m.time) * 1000UL : maphl::kUnknownAge;
}

// Notes the points there now, so "new" means new here, map open or not.
// Teammates already there in the first seconds after boot aren't new.
static void highlightsObserve(uint32_t now) {
  maphl::Table* t = highlights();
  if (!t) return;
  static uint32_t start = 0;
  static bool started = false;
  if (!started) { started = true; start = now; }
  const bool settling = now - start < 10000;
  uint8_t self4[4];
  s_io.selfKey4(self4);
  if (watchfeed::Store* w = s_watch) {
    for (int i = 0; i < watchfeed::kMaxItems; i++) {
      const watchfeed::Item& it = w->items[i];
      if (w->live(it, now)) t->note(hlKeyWatch(it), now, hlAge(now, it.at_ms));
    }
  }
  team::Core* c = s_core;
  if (!c || !c->cfg.enabled) return;
  for (int i = 0; i < team::kMaxMarkers; i++) {
    const TeamWire::Marker* m = c->marker(i);
    if (m && memcmp(m->key4, self4, 4) != 0) t->note(hlKeyMarker(*m), now, hlMarkerAge(*m));
  }
  for (int i = 0; i < team::kMaxMembers; i++) {
    team::Member* m = c->member(i);
    if (m && memcmp(m->pos.key4, self4, 4) != 0)
      t->note(hlKeyMember(*m), now, settling ? maphl::kNeverNew : maphl::kUnknownAge);
  }
}

static bool toScreen(double lat, double lon, double cwx, double cwy, int* sx, int* sy) {
  double wx, wy;
  latLonToWorldPx(lat, lon, s_map_zoom, &wx, &wy);
  *sx = (int)(wx - cwx + k_map_canvas_w / 2);
  *sy = (int)(wy - cwy + k_map_canvas_h / 2);
  return *sx >= -12 && *sx < k_map_canvas_w + 12 && *sy >= -12 && *sy < k_map_canvas_h + 12;
}

// Shared routes as lines, the followed one thicker. lv_line keeps a pointer to
// its points, so they live here (PSRAM) until the next redraw.
static auto& s_route_pts = psramStatic<lv_point_t[team::kMaxRoutes][TeamWire::kMaxShapePoints]>();
// A route parsed for drawing or listing (about 1 KB, in PSRAM, made once).
static TeamWire::Shape* routeScratch() {
  static TeamWire::Shape* p = nullptr;
  if (!p) {
    void* m = heap_caps_malloc(sizeof(TeamWire::Shape), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (m) p = new (m) TeamWire::Shape();
  }
  return p;
}
static void drawRoutes(lv_obj_t* parent, double cwx, double cwy) {
  team::Core* c = s_core;
  if (!c) return;
  TeamWire::Shape* scratch = routeScratch();
  if (!scratch) return;
  for (int i = 0; i < team::kMaxRoutes; i++) {
    TeamWire::Shape& sh = *scratch;
    if (!c->routeShape(i, sh) || sh.n < 2) continue;
    lv_point_t* pts = s_route_pts[i];
    bool any = false;
    for (int k = 0; k < sh.n; k++) {
      int sx, sy;
      any |= toScreen(sh.lat5[k] / 1e5, sh.lon5[k] / 1e5, cwx, cwy, &sx, &sy);
      pts[k].x = (lv_coord_t)(sx < -4000 ? -4000 : sx > 4000 ? 4000 : sx);
      pts[k].y = (lv_coord_t)(sy < -4000 ? -4000 : sy > 4000 ? 4000 : sy);
    }
    if (!any && sh.n < 3) continue;
    const bool followed = c->followedRoute() == i;
    lv_obj_t* l = lv_line_create(parent);
    lv_line_set_points(l, pts, (uint16_t)sh.n);
    lv_obj_set_style_line_width(l, followed ? 6 : 4, LV_PART_MAIN);
    lv_obj_set_style_line_color(l, lv_color_hex(followed ? 0x2979FF : TeamWire::colorRgb(sh.color)), LV_PART_MAIN);
    lv_obj_set_style_line_opa(l, followed ? LV_OPA_COVER : LV_OPA_80, LV_PART_MAIN);
    lv_obj_set_style_line_rounded(l, true, LV_PART_MAIN);
    lv_obj_set_pos(l, 0, 0);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    mapTrack(l);
    if (sh.label[0] && pts[0].x > -12 && pts[0].x < k_map_canvas_w && pts[0].y > -12 &&
        pts[0].y < k_map_canvas_h)
      mapLabel(parent, pts[0].x, pts[0].y, sh.label, 0xFFFFFF, 4);
  }
}

}  // namespace teamui

// Bloodhound to a point from outside Team (a contact's sheet): works whether or
// not team mode is set up.
static void teamHoundPoint(double lat, double lon, const char* label) {
  using namespace teamui;
  if (!core()) { toast(TR("Out of memory")); return; }
  s_core->houndPoint(lat, lon, label);
  houndRefresh();
}

// Called from renderMapMarkers: pins under people, people on top, alerts ringed.
static void teamDrawMapOverlay(lv_obj_t* parent, double cwx, double cwy) {
  using namespace teamui;
  mapFree();
  int sx, sy;
  const uint32_t hl_now = millis();
  highlightsObserve(hl_now);
  s_hl_active = false;   // recomputed as the points are drawn
  uint8_t self4[4];
  s_io.selfKey4(self4);
  drawRoutes(parent, cwx, cwy);
  // Watched reports underneath everything: a dot in the report's colour, a
  // drone as an orange square with its label, its operator as a small grey dot.
  if (watchfeed::Store* w = s_watch) {
    const uint32_t now = millis();
    for (int i = 0; i < watchfeed::kMaxItems; i++) {
      const watchfeed::Item& it = w->items[i];
      if (!w->live(it, now)) continue;
      const bool drone = it.r.track[0] != 0;
      if (drone && it.r.has2 && toScreen(it.r.lat2, it.r.lon2, cwx, cwy, &sx, &sy)) {
        // The operator: a white disc with a person, ringed in the report's colour.
        lv_obj_t* op_dot = mapDot(parent, sx, sy, 18, 0xFFFFFF, watchfeed::levelRgb(it.r.level), LV_RADIUS_CIRCLE,
                                  LV_OPA_COVER);
        mapIcon(op_dot, mapicons::kPerson, 0x000000);
        char op[24];
        snprintf(op, sizeof op, "%s %.8s", it.r.label2[0] ? it.r.label2 : "Op", it.r.title + 6);
        mapLabel(parent, sx, sy, op, 0xC8CCD0, 4, it.at_ms);
        if (s_hit_n < kMapObjs) s_hits[s_hit_n++] = {(int16_t)sx, (int16_t)sy, 2, (uint8_t)i};
      }
      if (!toScreen(it.r.lat, it.r.lon, cwx, cwy, &sx, &sy)) continue;
      if (mapIsNew(hlKeyWatch(it), hl_now, hlAge(hl_now, it.at_ms)))
        mapRing(parent, sx, sy, watchfeed::levelRgb(it.r.level));
      if (drone) {
        // A plane in the report's colour, pointing where it is heading when known.
        lv_obj_t* dd = mapDot(parent, sx, sy, 22, watchfeed::levelRgb(it.r.level), 0x101418, LV_RADIUS_CIRCLE,
                              LV_OPA_COVER);
        mapIcon(dd, mapicons::kFlight, 0x000000,
                it.r.has_bearing ? it.r.bearing : it.has_course ? it.course : 0);
        char lbl[64];
        watchfeed::droneLabel(it.r, it.has_course, it.course, lbl, sizeof lbl);
        mapLabel(parent, sx, sy, lbl, 0xFFCC80, 1, it.at_ms);
      } else if (it.r.quake_m10) {
        // An earthquake: a ring that grows with the magnitude, always labelled.
        const int m = it.r.quake_m10 < 20 ? 0 : it.r.quake_m10 > 80 ? 60 : it.r.quake_m10 - 20;
        lv_obj_t* d = mapDot(parent, sx, sy, 12 + m / 4, watchfeed::levelRgb(it.r.level), watchfeed::levelRgb(it.r.level),
                             LV_RADIUS_CIRCLE, LV_OPA_50);
        lv_obj_set_style_border_width(d, 3, LV_PART_MAIN);
        mapIcon(d, mapicons::kWaves, 0x000000);
        mapLabel(parent, sx, sy, it.r.title, 0xFFFFFF, 1, it.at_ms);
      } else {
        mapDot(parent, sx, sy, 14, watchfeed::levelRgb(it.r.level), 0x101418, LV_RADIUS_CIRCLE, LV_OPA_COVER);
        mapLabel(parent, sx, sy, it.r.title, 0xFFFFFF, 2, it.at_ms);
      }
      if (s_hit_n < kMapObjs) s_hits[s_hit_n++] = {(int16_t)sx, (int16_t)sy, 2, (uint8_t)i};
    }
  }
  // Pins shared in chats ("m:lat,lon|label|flags", from any DM or channel), as
  // the app draws them: a flag on a disc, blue for a DM, orange for the Public
  // channel, purple for other channels.
  if (g_lv.task) {
    const sharedpins::Pin* pins = nullptr;
    const int n = g_lv.task->sharedPins(&pins);
    for (int i = 0; pins && i < n; i++) {
      const sharedpins::Pin& p = pins[i];
      if (!toScreen(p.lat, p.lon, cwx, cwy, &sx, &sy)) continue;
      const uint32_t fill = !p.channel ? 0x0369A1 : threadIsPublicChannel(p.thread) ? 0xF97316 : 0x7C3AED;
      lv_obj_t* d = mapDot(parent, sx, sy, 22, fill, 0xFFFFFF, LV_RADIUS_CIRCLE, LV_OPA_COVER);
      mapIcon(d, mapicons::kFlag, 0xFFFFFF);
      if (s_hit_n < kMapObjs) s_hits[s_hit_n++] = {(int16_t)sx, (int16_t)sy, 3, (uint8_t)i};
    }
  }
  team::Core* c = s_core;
  if (!c || !c->cfg.enabled) { mapLabelsPlace(parent); return; }
  for (int i = 0; i < team::kMaxMarkers; i++) {
    const TeamWire::Marker* m = c->marker(i);
    if (!m || !toScreen(m->lat, m->lon, cwx, cwy, &sx, &sy)) continue;
    if (memcmp(m->key4, self4, 4) != 0 && mapIsNew(hlKeyMarker(*m), hl_now, hlMarkerAge(*m)))
      mapRing(parent, sx, sy, kNewPointRgb);
    // The kind's icon in its colour on a dark tile with a border in that colour.
    const uint32_t kc = TeamWire::kindRgb(m->kind);
    lv_obj_t* d = mapDot(parent, sx, sy, 22, 0x101418, kc, 5, LV_OPA_COVER);
    mapIcon(d, mapicons::markerKind(m->kind), kc);
    mapLabel(parent, sx, sy, m->label, 0xFFFFFF, 3, m->time);
    if (s_hit_n < kMapObjs) s_hits[s_hit_n++] = {(int16_t)sx, (int16_t)sy, 1, (uint8_t)i};
  }
  for (int i = 0; i < team::kMaxMembers; i++) {
    team::Member* m = c->member(i);
    if (!m || !m->pos.has_pos || !toScreen(m->pos.lat, m->pos.lon, cwx, cwy, &sx, &sy)) continue;
    const team::Fresh fr = c->freshness(*m);
    if (fr == team::Fresh::Lost) continue;
    if (memcmp(m->pos.key4, self4, 4) != 0 && mapIsNew(hlKeyMember(*m), hl_now, maphl::kUnknownAge))
      mapRing(parent, sx, sy, TeamWire::colorRgb(m->pos.color));
    if (m->pos.sos) mapDot(parent, sx, sy, 30, 0xE53935, 0xE53935, LV_RADIUS_CIRCLE, LV_OPA_40);
    // Heading: the app's arrow just outside the disc, turned to the direction of travel.
    if (m->pos.has_motion && m->pos.speed_mps > 0.5f) {
      const float r = m->pos.heading_deg * 0.0174533f;
      const int hx = sx + (int)(15 * sinf(r)), hy = sy - (int)(15 * cosf(r));
      lv_obj_t* h = mapDot(parent, hx, hy, 14, 0x000000, 0x000000, 0, LV_OPA_TRANSP);
      lv_obj_set_style_border_width(h, 0, LV_PART_MAIN);
      mapIcon(h, mapicons::kNavigation, 0xFFFFFF, m->pos.heading_deg);
    }
    const uint32_t mc = TeamWire::colorRgb(m->pos.color);
    lv_obj_t* d = mapDot(parent, sx, sy, 24, mc, m->pos.sos ? 0xE53935 : 0x101418, LV_RADIUS_CIRCLE,
                         fr == team::Fresh::Live ? LV_OPA_COVER : LV_OPA_50);
    if (m->pos.sos) lv_obj_set_style_border_width(d, 3, LV_PART_MAIN);
    // The role's icon, as in the app; a library icon this radio lacks shows the role's letter.
    if (const char* glyph = mapicons::role(m->pos.role, c->roleIcon(m->pos.role))) {
      mapIcon(d, glyph, onColor(mc));
    } else {
      char letter[2] = {c->roleLetter(m->pos.role), 0};
      lv_obj_t* l = lv_label_create(d);
      lv_label_set_text(l, letter);
      lv_obj_set_style_text_font(l, &g_font_12, LV_PART_MAIN);
      lv_obj_set_style_text_color(l, lv_color_hex(onColor(mc)), LV_PART_MAIN);
      lv_obj_center(l);
    }
    mapLabel(parent, sx, sy, m->pos.callsign[0] ? m->pos.callsign : "?", fr == team::Fresh::Live ? 0xFFFFFF : 0x9AA0A6,
             0, m->heard_ms);
    if (s_hit_n < kMapObjs) s_hits[s_hit_n++] = {(int16_t)sx, (int16_t)sy, 0, (uint8_t)i};
  }
  mapLabelsPlace(parent);
}
static void teamFreeMapOverlay() { teamui::mapFree(); }

namespace teamui {
// ---- member / pin actions -------------------------------------------------------------------
static int s_sel_kind = 0, s_sel_idx = 0;   // what an action sheet is about
static lv_obj_t* s_sheet = nullptr;
static void sheetClose() { if (s_sheet) popupClose(&s_sheet); }

static lv_obj_t* sheetOpen(const char* title, const char* body) {
  sheetClose();
  const lv_coord_t sw = lv_disp_get_hor_res(nullptr), sh = lv_disp_get_ver_res(nullptr);
  s_sheet = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(s_sheet);
  lv_obj_set_size(s_sheet, sw, sh - STATUSBAR_H);
  lv_obj_set_pos(s_sheet, 0, STATUSBAR_H);
  lv_obj_set_style_bg_color(s_sheet, lv_color_black(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_sheet, LV_OPA_50, LV_PART_MAIN);
  lv_obj_add_flag(s_sheet, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(s_sheet, [](lv_event_t* e) {
    if (lv_event_get_target(e) == s_sheet) sheetClose();
  }, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* card = lv_obj_create(s_sheet);
  lv_obj_remove_style_all(card);
  lv_obj_set_size(card, sw - 16, LV_SIZE_CONTENT);
  lv_obj_set_style_max_height(card, sh - STATUSBAR_H - 12, LV_PART_MAIN);
  lv_obj_align(card, LV_ALIGN_BOTTOM_MID, 0, -6);
  styleCard(card);
  tintSolid(card);
  lv_obj_set_style_pad_all(card, 8, LV_PART_MAIN);
  lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_style_pad_row(card, 6, LV_PART_MAIN);
  lv_obj_set_style_pad_column(card, 6, LV_PART_MAIN);
  lv_obj_t* t = lv_label_create(card);
  lv_label_set_text(t, title);
  lv_obj_set_width(t, LV_PCT(100));
  lv_obj_set_style_text_font(t, &g_font_16, LV_PART_MAIN);
  lv_obj_set_style_text_color(t, lv_color_hex(COLOR_TEXT), LV_PART_MAIN);
  useChainedFont(t);
  if (body && *body) {
    lv_obj_t* b = lv_label_create(card);
    lv_label_set_text(b, body);
    lv_obj_set_width(b, LV_PCT(100));
    lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(b, &g_font_12, LV_PART_MAIN);
    lv_obj_set_style_text_color(b, lv_color_hex(COLOR_SUB), LV_PART_MAIN);
    useChainedFont(b);
  }
  return card;
}

static lv_obj_t* sheetBtn(lv_obj_t* card, const char* txt, lv_event_cb_t cb, void* ud = nullptr, uint32_t bg = 0) {
  lv_obj_t* b = lv_btn_create(card);
  styleButton(b);
  lv_obj_set_size(b, LV_SIZE_CONTENT, 32);
  lv_obj_set_style_pad_hor(b, 10, LV_PART_MAIN);
  if (bg) fill(b, bg);
  else tintSolid(b);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
  lv_obj_t* l = lv_label_create(b);
  lv_label_set_text(l, txt);
  useChainedFont(l);
  lv_obj_center(l);
  return b;
}

#if CAP_TAK_EXTRAS
// ---- quick replies in the sheets --------------------------------------------------------------
static void rqRef(char* ref, size_t cap) {
  if (s_rq_kind == kRqWatch) quickreply::refWatch(s_rq_ch, s_rq_hash, ref, cap);
  else quickreply::refTeam(s_rq_kind, s_rq_to4, s_rq_id, ref, cap);
}

// A watched report is answered on its own channel, in plain text every client reads.
static bool rqSendWatch(uint8_t ch, const char* text) {
  ChannelDetails cd;
  if (!the_mesh.getChannel(ch, cd) || !cd.name[0]) return false;
  const uint32_t ts = the_mesh.getRTCClock()->getCurrentTimeUnique();
  const char* me = the_mesh.getNodePrefs()->node_name;
  if (!sendGroupMessageScoped(ch, cd, ts, me && *me ? me : "me", text)) return false;
  if (g_lv.task) g_lv.task->appSentMsgToChannel(cd.name, text, the_mesh.uiLastSentFp());
  return true;
}

static void rqSend(uint8_t code, uint8_t arg) {
  bool ok;
  if (s_rq_kind == kRqWatch) {
    char t[64];
    quickreply::watchText(code, arg, s_rq_hash, t, sizeof t);
    ok = rqSendWatch(s_rq_ch, t);
  } else {
    ok = s_core && s_core->sendReply(s_rq_kind, s_rq_to4, s_rq_id, code, arg);
  }
  char w[24], msg[48];
  quickreply::words(code, arg, w, sizeof w);
  if (!ok) { toast(TR("Couldn't send the answer")); return; }
  if (quickreply::Book* bk = book()) {
    char ref[28];
    rqRef(ref, sizeof ref);
    bk->add(ref, "You", code, arg, nullptr, millis(), true);
  }
  snprintf(msg, sizeof msg, "%s: %s", TR("Sent"), w);
  toast(msg, 1400);
  Io::s_version++;
}

static void rqEtaSheet() {
  lv_obj_t* card = sheetOpen(TR("How long until you're there?"), "");
  for (uint8_t m : quickreply::kEtas) {
    char t[16];
    snprintf(t, sizeof t, "%u min", (unsigned)m);
    sheetBtn(card, t, [](lv_event_t* e) {
      const uint8_t m = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
      sheetClose();
      rqSend(quickreply::kEta, m);
    }, (void*)(uintptr_t)m, 0x2979FF);
  }
  sheetBtn(card, TR("Close"), [](lv_event_t*) { sheetClose(); });
}

// The answers so far, one line each, then (unless it's ours) the buttons.
static void rqBlock(lv_obj_t* card, const uint8_t* codes, int n, bool can_answer) {
  quickreply::Book* bk = book();
  char ref[28];
  rqRef(ref, sizeof ref);
  if (bk) {
    const quickreply::Entry* a[6];
    const int k = bk->answers(ref, a, 6);
    for (int i = 0; i < k; i++) {
      char line[64], ago[12];
      quickreply::line(*a[i], line, sizeof line);
      const uint32_t m = (millis() - a[i]->at_ms) / 60000;
      if (m) snprintf(ago, sizeof ago, "  %lum", (unsigned long)m);
      else ago[0] = 0;
      char full[80];
      snprintf(full, sizeof full, LV_SYMBOL_LEFT " %s%s", line, ago);
      lv_obj_t* l = lv_label_create(card);
      lv_label_set_text(l, full);
      lv_obj_set_width(l, LV_PCT(100));
      lv_obj_set_style_text_font(l, &g_font_12, LV_PART_MAIN);
      lv_obj_set_style_text_color(l, lv_color_hex(a[i]->own ? COLOR_SUB : COLOR_TEXT), LV_PART_MAIN);
      useChainedFont(l);
    }
  }
  if (!can_answer) return;
  const uint8_t mine = bk ? bk->mine(ref) : quickreply::kCustom;
  for (int i = 0; i < n; i++) {
    const uint8_t code = codes[i];
    lv_obj_t* b = sheetBtn(card, code == quickreply::kEta ? TR("ETA...") : TR(quickreply::english(code)),
                           [](lv_event_t* e) {
      const uint8_t c = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
      sheetClose();
      if (c == quickreply::kEta) rqEtaSheet();
      else rqSend(c, 0);
    }, (void*)(uintptr_t)code, 0x37474F);
    if (code == mine) {
      lv_obj_set_style_border_color(b, lv_color_white(), LV_PART_MAIN);
      lv_obj_set_style_border_width(b, 2, LV_PART_MAIN);
    }
  }
}
#endif

static void memberSheet(int i) {
  team::Member* m = s_core ? s_core->member(i) : nullptr;
  if (!m) return;
  s_sel_kind = 0;
  s_sel_idx = i;
  char body[200], rb[40];
  memberRange(*m, rb, sizeof rb);
  const uint32_t ago = (millis() - m->heard_ms) / 60000;
  char batt[12] = "";
  if (m->pos.battery_pct >= 0) snprintf(batt, sizeof batt, " · %d%%", m->pos.battery_pct);
  snprintf(body, sizeof body, "%s%s%s%s\n%s · heard %lu min ago%s", roleStr(m->pos.role),
           m->pos.status ? " · " : "", TeamWire::statusName(m->pos.status), m->pos.sos ? " · SOS" : "", rb,
           (unsigned long)ago, batt);
  lv_obj_t* card = sheetOpen(m->pos.callsign[0] ? m->pos.callsign : "?", body);
#if CAP_TAK_EXTRAS
  for (int k = 0; k < team::kMaxAlerts; k++) {   // their alert, if one is on: answer it
    team::AlertRec* r = s_core->alert(k);
    if (!r || !r->a.active || memcmp(r->a.key4, m->pos.key4, 4)) continue;
    s_rq_kind = TeamWire::kRefAlert;
    memcpy(s_rq_to4, r->a.key4, 4);
    s_rq_id = r->a.id;
    rqBlock(card, quickreply::kAlertCodes, 4, true);
    break;
  }
#endif
#if CAP_BIKE_GAMES
  sheetBtn(card, TR("Monitor bike rider"), [](lv_event_t*) { bikeui::monitor(s_sel_idx); });
#endif
  if (m->pos.has_pos) {
    sheetBtn(card, TR("Hound"), [](lv_event_t*) {
      team::Member* x = s_core ? s_core->member(s_sel_idx) : nullptr;
      if (x) s_core->houndMember(x->pos.key4);
      sheetClose();
      houndRefresh();
    }, nullptr, 0xF2A33A);
    sheetBtn(card, TR("Map"), [](lv_event_t*) {
      team::Member* x = s_core ? s_core->member(s_sel_idx) : nullptr;
      sheetClose();
      if (x && x->pos.has_pos) showOnMap(x->pos.lat, x->pos.lon);
    });
  }
  sheetBtn(card, TR("Close"), [](lv_event_t*) { sheetClose(); });
}

static void routeSheet(int i) {
  TeamWire::Shape* scratch = routeScratch();
  if (!scratch || !s_core || !s_core->routeShape(i, *scratch)) return;
  TeamWire::Shape& sh = *scratch;
  s_sel_kind = 4;
  s_sel_idx = i;
  double len = 0;
  for (int k = 1; k < sh.n; k++)
    len += TeamWire::distanceM(sh.lat5[k - 1] / 1e5, sh.lon5[k - 1] / 1e5, sh.lat5[k] / 1e5, sh.lon5[k] / 1e5);
  char body[160], dist[16];
  teamroute::distanceText(len, dist, sizeof dist);
  snprintf(body, sizeof body, "%s, %d %s. %s", dist, sh.n, TR("points"),
           TR("Follow it for turn cues; the arrow points to the next corner."));
  lv_obj_t* card = sheetOpen(sh.label[0] ? sh.label : TR("Route"), body);
  sheetBtn(card, TR("Follow"), [](lv_event_t*) {
    if (s_core && !s_core->followRoute(s_sel_idx, false)) toast(TR("Out of memory"));
    sheetClose();
    houndRefresh();
    renderMapMarkers();
  }, nullptr, 0x2979FF);
  sheetBtn(card, TR("Follow back"), [](lv_event_t*) {
    if (s_core && !s_core->followRoute(s_sel_idx, true)) toast(TR("Out of memory"));
    sheetClose();
    houndRefresh();
    renderMapMarkers();
  });
  sheetBtn(card, TR("Map"), [](lv_event_t*) {
    TeamWire::Shape* x = routeScratch();
    const bool ok = x && s_core && s_core->routeShape(s_sel_idx, *x);
    sheetClose();
    if (ok) showOnMap(x->lat5[0] / 1e5, x->lon5[0] / 1e5);
  });
  sheetBtn(card, TR("Close"), [](lv_event_t*) { sheetClose(); });
}

static void pinSheet(int i) {
  TeamWire::Marker* m = s_core ? s_core->marker(i) : nullptr;
  if (!m) return;
  s_sel_kind = 1;
  s_sel_idx = i;
  char body[200], rb[40];
  rangeTo(m->lat, m->lon, rb, sizeof rb);
  snprintf(body, sizeof body, "%s%s%s\n%s%s", TeamWire::kindName(m->kind), m->note[0] ? " · " : "", m->note, rb,
           m->att_mode ? "\nHas a photo or voice note: open it in MeshCore Open TAK" : "");
  lv_obj_t* card = sheetOpen(m->label[0] ? m->label : TeamWire::kindName(m->kind), body);
#if CAP_TAK_EXTRAS
  s_rq_kind = TeamWire::kRefPin;
  memcpy(s_rq_to4, m->key4, 4);
  s_rq_id = m->id;
  rqBlock(card, quickreply::kReportCodes, 4, !s_core->isMine(*m));
#endif
  sheetBtn(card, TR("Hound"), [](lv_event_t*) {
    TeamWire::Marker* x = s_core ? s_core->marker(s_sel_idx) : nullptr;
    if (x) s_core->houndPoint(x->lat, x->lon, x->label[0] ? x->label : TeamWire::kindName(x->kind));
    sheetClose();
    houndRefresh();
  }, nullptr, 0xF2A33A);
  sheetBtn(card, TR("Map"), [](lv_event_t*) {
    TeamWire::Marker* x = s_core ? s_core->marker(s_sel_idx) : nullptr;
    sheetClose();
    if (x) showOnMap(x->lat, x->lon);
  });
  if (s_core->isMine(*m)) {
    sheetBtn(card, TR("Delete"), [](lv_event_t*) {
      TeamWire::Marker* x = s_core ? s_core->marker(s_sel_idx) : nullptr;
      if (x) s_core->deleteMarker(*x);
      sheetClose();
      renderMapMarkers();
    }, nullptr, COLOR_STATUS_DANGER);
  }
  sheetBtn(card, TR("Close"), [](lv_event_t*) { sheetClose(); });
}

static void watchSheet(int i) {
  watchfeed::Store* w = s_watch;
  if (!w || i < 0 || i >= watchfeed::kMaxItems || !w->items[i].used) return;
  const watchfeed::Item& it = w->items[i];
  s_sel_kind = 2;
  s_sel_idx = i;
  char title[64], body[260], rb[40], op[48] = "";
  if (it.r.track[0]) watchfeed::droneLabel(it.r, it.has_course, it.course, title, sizeof title);
  else snprintf(title, sizeof title, "%s", it.r.title[0] ? it.r.title : channelName(it.channel));
  rangeTo(it.r.lat, it.r.lon, rb, sizeof rb);
  // A transit card: the whole reply laid out, countdowns from now.
  static char tx_body[900];
  if (txDescribe(it, i, true, tx_body, sizeof tx_body - 120)) {
    const size_t n = strlen(tx_body);
    snprintf(tx_body + n, sizeof tx_body - n, "\n%s\n%s \xC2\xB7 %lu min ago", rb, channelName(it.channel),
             (unsigned long)((millis() - it.at_ms) / 60000));
  }
  if (it.r.has2) {
    char r2[40];
    rangeTo(it.r.lat2, it.r.lon2, r2, sizeof r2);
    snprintf(op, sizeof op, "\n%s: %s", it.r.label2[0] ? it.r.label2 : "2nd", r2);
  }
  snprintf(body, sizeof body, "%s%s%s\n%s%s\n%s · %lu min ago", it.r.track[0] ? it.r.track : it.r.body,
           it.r.track[0] && it.r.body[0] ? " · " : "", it.r.track[0] ? it.r.body : "", rb, op,
           channelName(it.channel), (unsigned long)((millis() - it.at_ms) / 60000));
  lv_obj_t* card = sheetOpen(title, txText(i) ? tx_body : body);
#if CAP_TAK_EXTRAS
  s_rq_kind = kRqWatch;
  s_rq_ch = it.channel;
  s_rq_hash = it.reply_hash;
  rqBlock(card, quickreply::kReportCodes, 4, true);
#endif
  sheetBtn(card, TR("Hound"), [](lv_event_t*) {
    const watchfeed::Item* x = s_watch ? &s_watch->items[s_sel_idx] : nullptr;
    if (x && x->used && core()) s_core->houndPoint(x->r.lat, x->r.lon, x->r.title);
    sheetClose();
    houndRefresh();
  }, nullptr, 0xF2A33A);
  sheetBtn(card, TR("Map"), [](lv_event_t*) {
    const watchfeed::Item* x = s_watch ? &s_watch->items[s_sel_idx] : nullptr;
    sheetClose();
    if (x && x->used) showOnMap(x->r.lat, x->r.lon);
  });
  if (it.r.has2) {
    sheetBtn(card, it.r.label2[0] ? it.r.label2 : "2nd", [](lv_event_t*) {
      const watchfeed::Item* x = s_watch ? &s_watch->items[s_sel_idx] : nullptr;
      sheetClose();
      if (x && x->used) showOnMap(x->r.lat2, x->r.lon2);
    });
  }
  sheetBtn(card, TR("Close"), [](lv_event_t*) { sheetClose(); });
}

// A pin shared in a chat: who, where, and the conversation it came in.
static sharedpins::Pin s_sel_pin;
static void sharedPinSheet(int i) {
  const sharedpins::Pin* pins = nullptr;
  const int n = g_lv.task ? g_lv.task->sharedPins(&pins) : 0;
  if (!pins || i < 0 || i >= n) return;
  s_sel_pin = pins[i];
  char body[200], rb[40];
  rangeTo(s_sel_pin.lat, s_sel_pin.lon, rb, sizeof rb);
  snprintf(body, sizeof body, "%s %s%s%s\n%.5f, %.5f\n%s", TR("From"), s_sel_pin.from,
           s_sel_pin.channel ? " · " : "", s_sel_pin.channel ? s_sel_pin.thread : "", s_sel_pin.lat, s_sel_pin.lon, rb);
  lv_obj_t* card = sheetOpen(s_sel_pin.label[0] ? s_sel_pin.label : TR("Shared pin"), body);
  sheetBtn(card, TR("Hound"), [](lv_event_t*) {
    if (core()) s_core->houndPoint(s_sel_pin.lat, s_sel_pin.lon, s_sel_pin.label[0] ? s_sel_pin.label : "Pin");
    sheetClose();
    houndRefresh();
  }, nullptr, 0xF2A33A);
  sheetBtn(card, TR("Close"), [](lv_event_t*) { sheetClose(); });
}

// Drop a pin: pick a kind, then an optional label.
static double s_drop_lat = 0, s_drop_lon = 0;
static int s_drop_kind = 0;
static void dropLabelDone(const char* text) {
  if (!text || !s_core) return;
  s_core->putMarker(nullptr, (uint8_t)s_drop_kind, s_drop_lat, s_drop_lon, text, "");
  toast(TR("Pin shared with the team"));
  renderMapMarkers();
}
static void dropSheet(double lat, double lon) {
  s_drop_lat = lat;
  s_drop_lon = lon;
  char body[48];
  snprintf(body, sizeof body, "%.5f, %.5f", lat, lon);
  lv_obj_t* card = sheetOpen(TR("Drop a team pin"), body);
  for (int k = 0; k < TeamWire::kMarkerKinds; k++) {
    lv_obj_t* b = sheetBtn(card, TeamWire::kindName(k), [](lv_event_t* e) {
      s_drop_kind = (int)(intptr_t)lv_event_get_user_data(e);
      sheetClose();
      fmTextPrompt(TR("Label (optional)"), "", dropLabelDone);
    }, (void*)(intptr_t)k);
    lv_obj_set_style_border_width(b, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(b, lv_color_hex(TeamWire::kindRgb(k)), LV_PART_MAIN);
  }
}

}  // namespace teamui

// Map canvas tap: true when it hit a teammate or a pin (and handled it).
static bool teamMapTap(lv_point_t p) {
  using namespace teamui;
  if (!s_hit_n || !s_map_canvas) return false;
  lv_area_t a;
  lv_obj_get_coords(s_map_pan_layer ? s_map_pan_layer : s_map_canvas, &a);
  const int lx = p.x - a.x1, ly = p.y - a.y1;
  int best = -1, best_d = 18 * 18;
  for (int i = 0; i < s_hit_n; i++) {
    const int dx = s_hits[i].x - lx, dy = s_hits[i].y - ly, d = dx * dx + dy * dy;
    // people over pins over watched reports
    if (d <= best_d && (best < 0 || s_hits[i].kind <= s_hits[best].kind)) { best = i; best_d = d; }
  }
  if (best < 0) return false;
  if (s_hits[best].kind == 0) memberSheet(s_hits[best].idx);
  else if (s_hits[best].kind == 1) pinSheet(s_hits[best].idx);
  else if (s_hits[best].kind == 2) watchSheet(s_hits[best].idx);
  else sharedPinSheet(s_hits[best].idx);
  return true;
}

// Map canvas long-press: drop a pin there.
static bool teamMapLongPress(lv_point_t p) {
  using namespace teamui;
  if (!s_core || !s_core->cfg.enabled || !s_map_canvas) return false;
  if (!s_core->active()) { toast(TR("Team: pick the team channel in Team > Setup")); return true; }
  lv_area_t a;
  lv_obj_get_coords(s_map_canvas, &a);
  double cwx, cwy, lat, lon;
  latLonToWorldPx(s_map_center_lat, s_map_center_lon, s_map_zoom, &cwx, &cwy);
  worldPxToLatLon(cwx + (p.x - a.x1) - k_map_canvas_w / 2, cwy + (p.y - a.y1) - k_map_canvas_h / 2, s_map_zoom, &lat, &lon);
  dropSheet(lat, lon);
  return true;
}

void teamOnChannelData(int idx, const uint8_t* data, size_t len) {
  team::Core* c = teamui::core();
  ChannelDetails cd;
  if (!c || idx < 0 || !the_mesh.getChannel(idx, cd)) return;
  c->onPayload(cd.name, data, len);
}

// Called by MyMesh for every channel text message: a report with a position on
// a watched channel goes on the map.
// Back-track (BACKTRACK.md): follow a recorded track back to its start.
bool teamFollowTrack(const int32_t* lat5, const int32_t* lon5, int n, const char* label) {
  using namespace teamui;
  team::Core* c = core();
  if (!c || !c->followTrack(lat5, lon5, n, label)) return false;
  houndRefresh();
  return true;
}

// The map's "On map" list, first tab: teammates, team pins, watched reports and
// pins shared in chats, nearest first (or newest, without a position of our
// own). A tap centres the map there. Returns how many rows it added.
struct OnMapRow { double lat, lon; uint32_t age_s; double dist_km; uint32_t color; char name[40]; char kind[40]; };
static OnMapRow* s_onmap = nullptr;
static constexpr int kOnMapMax = 96;

int teamOnMapFill(lv_obj_t* list, lv_coord_t rw, double self_lat, double self_lon) {
  using namespace teamui;
  if (!s_onmap) s_onmap = (OnMapRow*)psAlloc(sizeof(OnMapRow) * kOnMapMax);
  if (!s_onmap || !list) return 0;
  const bool fix = self_lat != 0.0 || self_lon != 0.0;
  const uint32_t now = millis();
  const uint32_t epoch = s_io.epoch();
  int n = 0;
  auto add = [&](double lat, double lon, uint32_t age_s, uint32_t color, const char* name, const char* kind) {
    if (n >= kOnMapMax || (lat == 0.0 && lon == 0.0)) return;
    OnMapRow& r = s_onmap[n++];
    r.lat = lat;
    r.lon = lon;
    r.age_s = age_s;
    r.dist_km = fix ? TeamWire::distanceM(self_lat, self_lon, lat, lon) / 1000.0 : -1;
    r.color = color;
    snprintf(r.name, sizeof r.name, "%s", name && *name ? name : "?");
    snprintf(r.kind, sizeof r.kind, "%s", kind);
  };
  if (team::Core* c = s_core) {
    for (int i = 0; i < team::kMaxMembers; i++) {
      team::Member* m = c->member(i);
      if (!m || !m->pos.has_pos) continue;   // the list has them; this is the map's
      char kind[40];
      c->roleName(m->pos.role, kind, sizeof kind);
      if (m->pos.sos) snprintf(kind + strlen(kind), sizeof kind - strlen(kind), " \xC2\xB7 SOS");
      add(m->pos.lat, m->pos.lon, (now - m->heard_ms) / 1000, TeamWire::colorRgb(m->pos.color), m->pos.callsign,
          kind);
    }
    for (int i = 0; i < team::kMaxMarkers; i++) {
      const TeamWire::Marker* m = c->marker(i);
      if (!m) continue;
      const uint32_t age = epoch && m->time && m->time < epoch ? epoch - m->time : 0;
      add(m->lat, m->lon, age, TeamWire::kindRgb(m->kind), m->label[0] ? m->label : TeamWire::kindName(m->kind),
          TeamWire::kindName(m->kind));
    }
  }
  if (watchfeed::Store* w = s_watch) {
    for (int i = 0; i < watchfeed::kMaxItems; i++) {
      const watchfeed::Item& it = w->items[i];
      if (!w->live(it, now)) continue;
      char name[64];
      if (it.r.track[0]) watchfeed::droneLabel(it.r, it.has_course, it.course, name, sizeof name);
      else snprintf(name, sizeof name, "%s", it.r.title);
      add(it.r.lat, it.r.lon, (now - it.at_ms) / 1000, watchfeed::levelRgb(it.r.level), name, channelName(it.channel));
    }
  }
  if (g_lv.task) {
    const sharedpins::Pin* pins = nullptr;
    const int k = g_lv.task->sharedPins(&pins);
    for (int i = 0; pins && i < k; i++) {
      const sharedpins::Pin& p = pins[i];
      char kind[48];
      snprintf(kind, sizeof kind, "%s %s", TR("Shared in"), p.thread);
      const uint32_t age = epoch && p.ts && p.ts < epoch ? epoch - p.ts : 0;
      add(p.lat, p.lon, age, 0x7C3AED, p.label[0] ? p.label : p.from, kind);
    }
  }
  // Nearest first; without a position, newest first.
  for (int i = 1; i < n; i++)
    for (int j = i; j > 0; j--) {
      const OnMapRow& a = s_onmap[j];
      const OnMapRow& b = s_onmap[j - 1];
      const bool before = fix ? a.dist_km < b.dist_km : a.age_s < b.age_s;
      if (!before) break;
      const OnMapRow t = s_onmap[j];
      s_onmap[j] = s_onmap[j - 1];
      s_onmap[j - 1] = t;
    }
  for (int i = 0; i < n; i++) {
    const OnMapRow& r = s_onmap[i];
    lv_obj_t* b = lv_btn_create(list);
    lv_obj_set_size(b, rw, 46);
    styleButton(b);
    lv_obj_set_style_bg_color(b, lv_color_hex(COLOR_CONTROL), LV_PART_MAIN);
    lv_obj_set_style_pad_all(b, 0, LV_PART_MAIN);
    lv_obj_set_style_border_side(b, LV_BORDER_SIDE_LEFT, LV_PART_MAIN);
    lv_obj_set_style_border_width(b, 4, LV_PART_MAIN);
    lv_obj_set_style_border_color(b, lv_color_hex(r.color), LV_PART_MAIN);
    lv_obj_add_event_cb(b, [](lv_event_t* e) {
      if (lv_event_get_code(e) != LV_EVENT_CLICKED || !s_onmap) return;
      const OnMapRow& x = s_onmap[(int)(intptr_t)lv_event_get_user_data(e)];
      const double la = x.lat, lo = x.lon;
      lv_indev_t* a = lv_indev_get_act();
      if (a) lv_indev_wait_release(a);
      closeMapContacts();
      mapJumpTo(la, lo, 14);   // also turns auto-follow off, which would snap straight back
    }, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    lv_obj_t* nl = lv_label_create(b);
    lv_label_set_text(nl, r.name);
    lv_label_set_long_mode(nl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(nl, rw - 96);
    lv_obj_set_height(nl, lv_font_get_line_height(&g_font_14));
    lv_obj_set_style_text_font(nl, &g_font_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(nl, lv_color_hex(COLOR_TEXT), LV_PART_MAIN);
    useChainedFont(nl);
    lv_obj_align(nl, LV_ALIGN_TOP_LEFT, 8, 5);
    char dist[16] = "", age[12], meta[32];
    if (r.dist_km >= 0) {
      if (r.dist_km < 1) snprintf(dist, sizeof dist, "%d m", (int)(r.dist_km * 1000));
      else snprintf(dist, sizeof dist, "%.1f km", r.dist_km);
    }
    formatAgeBadge(age, sizeof age, r.age_s);
    if (dist[0]) snprintf(meta, sizeof meta, "%s \xC2\xB7 %s", dist, age);
    else snprintf(meta, sizeof meta, "%s", age);
    lv_obj_t* ml = lv_label_create(b);
    lv_label_set_text(ml, meta);
    lv_obj_set_style_text_font(ml, &g_font_12, LV_PART_MAIN);
    lv_obj_set_style_text_color(ml, lv_color_hex(COLOR_SUB), LV_PART_MAIN);
    lv_obj_align(ml, LV_ALIGN_TOP_RIGHT, -8, 6);
    lv_obj_t* kl = lv_label_create(b);
    lv_label_set_text(kl, r.kind);
    lv_label_set_long_mode(kl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(kl, rw - 16);
    lv_obj_set_height(kl, lv_font_get_line_height(&g_font_12));
    lv_obj_set_style_text_font(kl, &g_font_12, LV_PART_MAIN);
    lv_obj_set_style_text_color(kl, lv_color_hex(COLOR_SUB), LV_PART_MAIN);
    useChainedFont(kl);
    lv_obj_align(kl, LV_ALIGN_BOTTOM_LEFT, 8, -4);
  }
  return n;
}

// MeshCore TEAM's channel text (MESHCORE_TEAM.md): true when it is, so it isn't
// shown as chat; on the team channel it becomes members, pins and routes.
bool teamCompatOnChannelText(int idx, const char* text) {
  using namespace teamui;
  if (idx < 0 || !text) return false;
  char sender[40];
  const char* body = watchfeed::messageText(text, sender, sizeof sender);
  if (!body || !teamcompat::isCompatText(body)) return false;
  ChannelDetails cd;
  if (!the_mesh.getChannel(idx, cd)) return false;
  team::Core* c = core();
  return c ? c->onText(cd.name, sender, body) : true;
}

// Teammates whose beacons said they heard one of our channel messages
// (MESSAGE_REACH.md), for the message's info and its map. Our stored time can
// be a second or two off the one that went on the air, so nearby ones count too.
int teamHeardBy(uint32_t ts, const char* text, char (*names)[21], double* lat, double* lon, int cap) {
  using namespace teamui;
  team::Core* c = s_core;
  if (!c || !c->active() || !text) return 0;
  const char* me = the_mesh.getNodePrefs()->node_name;
  int n = 0;
  for (int dt = -2; dt <= 2; dt++) {
    uint8_t keys[8][4];
    const int k = c->heardBy(quickreply::reportHash(ts + dt, me && *me ? me : "me", text), keys, nullptr, 8);
    for (int i = 0; i < k && n < cap; i++) {
      for (int j = 0; j < team::kMaxMembers; j++) {
        team::Member* m = c->member(j);
        if (!m || memcmp(m->pos.key4, keys[i], 4)) continue;
        bool dup = false;
        for (int q = 0; q < n && !dup; q++) dup = !strcmp(names[q], m->pos.callsign[0] ? m->pos.callsign : "?");
        if (dup) break;
        snprintf(names[n], 21, "%s", m->pos.callsign[0] ? m->pos.callsign : "?");
        if (lat) lat[n] = m->pos.has_pos ? m->pos.lat : 0;   // 0, 0: not plotted
        if (lon) lon[n] = m->pos.has_pos ? m->pos.lon : 0;
        n++;
        break;
      }
    }
  }
  return n;
}

bool teamReachActive() { return teamui::s_core && teamui::s_core->active(); }

// When a watched report happened, on the millis() clock: the sender's time
// if both clocks are plausible (a report that waited in the mesh is older),
// else now.
static uint32_t watchAtMs(uint32_t sent) {
  using namespace teamui;
  uint32_t at = millis();
  const uint32_t now_epoch = s_io.epoch();
  if (now_epoch && sent > 1700000000UL && sent < now_epoch) {
    const uint32_t age_ms = (now_epoch - sent) > 4000000UL ? 4000000000UL : (now_epoch - sent) * 1000UL;
    at = age_ms < at ? at - age_ms : 0;
  }
  return at;
}

// A transit bot's reply on a watched channel (TransitFeed.h): true when it was
// one. A reply split over messages is joined (same sender, within 2 minutes)
// and goes on the map once its last part, with the stop's position, is in.
static bool transitOnText(watchfeed::Store* w, int idx, const char* channel, const char* sender, const char* body,
                          uint32_t sent) {
  using namespace teamui;
  const bool posts = transitfeed::transitChannel(channel);
  {
    if (!s_txr) {   // read with this first; the rest is made on the first transit reply
      void* m = heap_caps_malloc(sizeof(transitfeed::Reply), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (!m) return false;
      s_txr = new (m) transitfeed::Reply();
    }
    if (!transitfeed::parse(body, *s_txr, posts)) return false;
    const bool part = s_txr->kind == transitfeed::kPart;
    if (!txAlloc()) return false;
    TxPending& p = *s_txp;
    const uint32_t now = millis();
    if (part && p.ch == idx && !strcmp(p.sender, sender) && now - p.ms < 120000UL) {
      const size_t n = strlen(p.text);
      if (n + 2 < sizeof p.text) snprintf(p.text + n, sizeof p.text - n, "\n%s", body);
    } else {
      p.ch = (int8_t)idx;
      p.sent = sent;
      watchfeed::copyN(p.sender, sizeof p.sender, sender, strlen(sender));
      watchfeed::copyN(p.text, sizeof p.text, body, strlen(body));
    }
    p.ms = now;
  }
  TxPending& p = *s_txp;
  if (!transitfeed::parse(p.text, *s_txr, posts) || !s_txr->has_pos) return true;   // the rest is on its way
  watchfeed::Report r;
  transitfeed::toReport(*s_txr, r);
  const uint32_t at = watchAtMs(p.sent);
  if (watchfeed::Item* it = w->add(r, (uint8_t)idx, at)) {
    txSet((int)(it - w->items), p.text);
    it->reply_hash = quickreply::reportHash(p.sent, sender, body);
    s_io.changed();
    if (watchZoomOn() && (uint32_t)(millis() - at) < 30UL * 60UL * 1000UL) {
      s_zoom_lat = r.lat;
      s_zoom_lon = r.lon;
      s_zoom_pending = true;
    }
  }
  p.ch = -1;
  return true;
}

void teamOnChannelText(int idx, const char* text, uint32_t sent) {
  using namespace teamui;
  if (idx < 0 || !text) return;
  ChannelDetails cd;
  if (!the_mesh.getChannel(idx, cd)) return;
  {   // a teammate's message: say we heard it in our next beacon (MESSAGE_REACH.md)
    char who[40];
    const char* b = watchfeed::messageText(text, who, sizeof who);
    if (s_core && who[0]) s_core->noteChannelText(cd.name, who, sent, b);
  }
  watchfeed::Store* w = watch();
  if (!w || !w->watchedCount() || !w->isWatched(cd.name)) return;
  watchfeed::Report r;
  char sender[40];
  const char* body = watchfeed::messageText(text, sender, sizeof sender);
#if CAP_TAK_EXTRAS
  // A quick reply belongs to the report it answers ("On my way ↩3fa1").
  quickreply::Parsed qp;
  if (quickreply::parseWatch(body, qp)) {
    if (quickreply::Book* bk = book()) {
      char ref[28];
      quickreply::refWatch((uint8_t)idx, qp.hash, ref, sizeof ref);
      bk->add(ref, sender[0] ? sender : "?", qp.code, qp.arg, qp.said, millis(), false);
      s_io.changed();
    }
    return;
  }
#endif
  // A transit bot's reply (BusBot's #transit) becomes a transit card.
  if (transitOnText(w, idx, cd.name, sender, body, sent)) return;
  if (!watchfeed::parse(body, r)) return;
  if (!r.track[0] && !r.quake_m10 && watchfeed::isIce(cd.name, sender)) watchfeed::tagIce(r);
  const uint32_t at = watchAtMs(sent);
  if (watchfeed::Item* it = w->add(r, (uint8_t)idx, at)) {
    txSet((int)(it - w->items), nullptr);   // the slot may have held a transit card
    it->reply_hash = quickreply::reportHash(sent, sender, body);
    s_io.changed();
    // Auto-zoom: only a report that is happening now (not a backlog).
    if (watchZoomOn() && (uint32_t)(millis() - at) < 30UL * 60UL * 1000UL) {
      s_zoom_lat = r.lat;
      s_zoom_lon = r.lon;
      s_zoom_pending = true;   // moved from the UI loop (loopTick), not from here
    }
  }
}

namespace teamui {
// ---- Team page ----------------------------------------------------------------------------------
static lv_obj_t* s_root = nullptr;
static lv_obj_t* s_body = nullptr;
static int s_tab = 0;   // 0 team, 1 pins, 2 setup, 3 watch
static bool s_show_lost = false;   // the team list's "Not heard lately" opened
static lv_timer_t* s_page_timer = nullptr;

static void pageBuild();

static lv_obj_t* lbl(lv_obj_t* p, const char* t, const lv_font_t* f, uint32_t col) {
  lv_obj_t* l = lv_label_create(p);
  lv_label_set_text(l, t);
  lv_obj_set_style_text_font(l, f, LV_PART_MAIN);
  lv_obj_set_style_text_color(l, lv_color_hex(col), LV_PART_MAIN);
  lv_obj_set_width(l, LV_PCT(100));
  lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
  useChainedFont(l);
  return l;
}

static lv_obj_t* rowCard(lv_obj_t* p, lv_event_cb_t cb, void* ud) {
  lv_obj_t* c = lv_obj_create(p);
  lv_obj_remove_style_all(c);
  lv_obj_set_size(c, LV_PCT(100), LV_SIZE_CONTENT);
  styleCard(c);
  lv_obj_set_style_pad_all(c, 6, LV_PART_MAIN);
  lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(c, 1, LV_PART_MAIN);
  lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(c, cb, LV_EVENT_CLICKED, ud);
  return c;
}

static void sosHoldDone() {
  if (!s_core) return;
  if (s_core->sosActive()) s_core->cancelAlert();
  else s_core->raiseAlert(0, "");
  pageBuild();
}

static void buildTeam(lv_obj_t* b) {
  team::Core* c = s_core;
  char line[160];
  // status line
  const int ch = Io::channelIdx(c->cfg.channel);
  if (!c->cfg.enabled) snprintf(line, sizeof line, "%s", TR("Team mode is off. Turn it on in Setup."));
  else if (ch < 0) snprintf(line, sizeof line, "%s", TR("Pick the team channel in Setup."));
  else {
    double la, lo;
    const bool fix = myFix(&la, &lo);
    const uint32_t ago = c->everBeaconed() ? (millis() - c->lastBeaconMs()) / 60000 : 0;
    snprintf(line, sizeof line, "#%s · %s · %s", c->cfg.channel, c->callsign(),
             !c->cfg.share ? "not sharing position" : !fix ? "no position (no GPS fix or set one in Settings)"
             : c->everBeaconed() ? (ago ? "position sent" : "position sent just now") : "sending position");
    (void)ago;
  }
  lbl(b, line, &g_font_12, COLOR_SUB);
  // SOS
  lv_obj_t* sos = lv_btn_create(b);
  lv_obj_set_size(sos, LV_PCT(100), 40);
  styleButton(sos);
  fill(sos, c->sosActive() ? 0x7F1D1D : 0xD32F2F);
  lv_obj_add_event_cb(sos, [](lv_event_t*) {
    if (!s_core || !s_core->active()) { toast(TR("Team is not set up yet")); return; }
    if (s_core->sosActive())
      showConfirm(TR("Cancel your SOS?"), TR("Cancel SOS"), sosHoldDone);
    else
      showConfirm(TR("Send SOS to the team with your position?"), TR("Send SOS"), sosHoldDone);
  }, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* sl = lv_label_create(sos);
  lv_label_set_text(sl, c->sosActive() ? TR("SOS active: tap to cancel") : LV_SYMBOL_WARNING " SOS");
  lv_obj_set_style_text_font(sl, &g_font_16, LV_PART_MAIN);
  lv_obj_set_style_text_color(sl, lv_color_white(), LV_PART_MAIN);
  useChainedFont(sl);
  lv_obj_center(sl);
#if CAP_TAK_EXTRAS
  if (c->sosActive()) {   // the team's answers to our SOS
    char ref[28];
    uint8_t me[4];
    s_io.selfKey4(me);
    quickreply::refTeam(TeamWire::kRefAlert, me, c->ownAlert().id, ref, sizeof ref);
    if (quickreply::Book* bk = book()) {
      const quickreply::Entry* a[6];
      const int k = bk->answers(ref, a, 6);
      for (int i = 0; i < k; i++) {
        quickreply::line(*a[i], line, sizeof line);
        lbl(b, line, &g_font_14, 0xFFCDD2);
      }
      if (!k) lbl(b, TR("No answers yet."), &g_font_12, COLOR_SUB);
    }
  }
#endif
  // active alerts first
  for (int i = 0; i < team::kMaxAlerts; i++) {
    team::AlertRec* r = c->alert(i);
    if (!r || !r->a.active) continue;
    char rb[40] = "";
    if (r->a.has_pos) rangeTo(r->a.lat, r->a.lon, rb, sizeof rb);
    snprintf(line, sizeof line, LV_SYMBOL_WARNING " %s: %s  %s", TeamWire::alertName(r->a.type),
             r->a.callsign[0] ? r->a.callsign : "?", rb);
    lv_obj_t* card = rowCard(b, [](lv_event_t* e) {
      team::AlertRec* x = s_core ? s_core->alert((int)(intptr_t)lv_event_get_user_data(e)) : nullptr;
      if (x) alarmShow(*x);
    }, (void*)(intptr_t)i);
    lv_obj_set_style_border_color(card, lv_color_hex(0xE53935), LV_PART_MAIN);
    lbl(card, line, &g_font_14, 0xFF8A80);
  }
  // roster, nearest first
  int order[team::kMaxMembers], n = 0;
  double dist[team::kMaxMembers];
  double ml = 0, mo = 0;
  const bool fix = myFix(&ml, &mo);
  for (int i = 0; i < team::kMaxMembers; i++) {
    team::Member* m = c->member(i);
    if (!m) continue;
    // No position: after everyone who has one.
    dist[i] = !m->pos.has_pos ? 1e18 : fix ? TeamWire::distanceM(ml, mo, m->pos.lat, m->pos.lon) : 0;
    order[n++] = i;
  }
  for (int i = 1; i < n; i++)
    for (int j = i; j > 0 && dist[order[j]] < dist[order[j - 1]]; j--) { int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t; }
  if (!n && c->cfg.enabled) lbl(b, TR("No teammates heard yet."), &g_font_12, COLOR_SUB);
  // Live and stale teammates first; lost ones (over an hour, already off the
  // map) under "Not heard lately", collapsed. As the app's team list.
  int n_lost = 0;
  for (int k = 0; k < n; k++)
    if (c->freshness(*c->member(order[k])) == team::Fresh::Lost) n_lost++;
  for (int pass = 0; pass < 2; pass++) {
    if (pass == 1) {
      if (!n_lost) break;
      snprintf(line, sizeof line, "%s %s (%d)", s_show_lost ? LV_SYMBOL_DOWN : LV_SYMBOL_RIGHT,
               TR("Not heard lately"), n_lost);
      lv_obj_t* t = rowCard(b, [](lv_event_t*) { s_show_lost = !s_show_lost; pageBuild(); }, nullptr);
      lbl(t, line, &g_font_14, COLOR_SUB);
      lbl(t, TR("Over an hour since their last beacon. Off the map until they're heard again."), &g_font_12,
          COLOR_SUB);
      if (!s_show_lost) break;
    }
  for (int k = 0; k < n; k++) {
    team::Member* m = c->member(order[k]);
    const team::Fresh fr = c->freshness(*m);
    if ((fr == team::Fresh::Lost) != (pass == 1)) continue;
    lv_obj_t* card = rowCard(b, [](lv_event_t* e) { memberSheet((int)(intptr_t)lv_event_get_user_data(e)); },
                             (void*)(intptr_t)order[k]);
    lv_obj_set_style_border_color(card, lv_color_hex(TeamWire::colorRgb(m->pos.color)), LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 2, LV_PART_MAIN);
    char rb[40];
    memberRange(*m, rb, sizeof rb);
    snprintf(line, sizeof line, "%s%s  %s", m->pos.sos ? LV_SYMBOL_WARNING " " : "", m->pos.callsign[0] ? m->pos.callsign : "?", rb);
    lbl(card, line, &g_font_14, fr == team::Fresh::Live ? COLOR_TEXT : COLOR_SUB);
    const uint32_t ago = (millis() - m->heard_ms) / 60000;
    char batt[12] = "";
    if (m->pos.battery_pct >= 0) snprintf(batt, sizeof batt, " · %d%%", m->pos.battery_pct);
    snprintf(line, sizeof line, "%s%s%s · %lu min%s%s", roleStr(m->pos.role), m->pos.status ? " · " : "",
             TeamWire::statusName(m->pos.status), (unsigned long)ago, batt,
             fr == team::Fresh::Live ? "" : fr == team::Fresh::Stale ? " · stale" : " · lost");
    lbl(card, line, &g_font_12, COLOR_SUB);
  }
  }
}

static void buildPins(lv_obj_t* b) {
  team::Core* c = s_core;
  lv_obj_t* add = lv_btn_create(b);
  styleButton(add);
  lv_obj_set_size(add, LV_PCT(100), 32);
  lv_obj_add_event_cb(add, [](lv_event_t*) {
    double la, lo;
    if (!s_core || !s_core->active()) { toast(TR("Team is not set up yet")); return; }
    if (!myFix(&la, &lo)) { toast(TR("No position: long-press the map instead"), 2200); return; }
    dropSheet(la, lo);
  }, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* al = lv_label_create(add);
  lv_label_set_text(al, TR("Pin at my position  (or long-press the map)"));
  useChainedFont(al);
  lv_obj_center(al);
  int n = 0;
  char line[120];
  for (int i = 0; i < team::kMaxMarkers; i++) {
    TeamWire::Marker* m = c->marker(i);
    if (!m) continue;
    n++;
    lv_obj_t* card = rowCard(b, [](lv_event_t* e) { pinSheet((int)(intptr_t)lv_event_get_user_data(e)); }, (void*)(intptr_t)i);
    lv_obj_set_style_border_color(card, lv_color_hex(TeamWire::kindRgb(m->kind)), LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 2, LV_PART_MAIN);
    char rb[40];
    rangeTo(m->lat, m->lon, rb, sizeof rb);
    snprintf(line, sizeof line, "%s  %s", m->label[0] ? m->label : TeamWire::kindName(m->kind), rb);
    lbl(card, line, &g_font_14, COLOR_TEXT);
    snprintf(line, sizeof line, "%s%s%s%s", TeamWire::kindName(m->kind), m->note[0] ? " · " : "", m->note,
             c->isMine(*m) ? " · mine" : "");
    lbl(card, line, &g_font_12, COLOR_SUB);
  }
  if (!n) lbl(b, TR("No pins yet."), &g_font_12, COLOR_SUB);

  // Routes the team shared: MeshCore Open TAK's road directions (Share with
  // team) and routes drawn there. Follow one for turn-by-turn cues.
  lbl(b, TR("Routes"), &g_font_14, COLOR_TEXT);
  int rn = 0;
  TeamWire::Shape* scratch = routeScratch();
  for (int i = 0; scratch && i < team::kMaxRoutes; i++) {
    TeamWire::Shape& sh = *scratch;
    if (!c->routeShape(i, sh)) continue;
    rn++;
    lv_obj_t* card = rowCard(b, [](lv_event_t* e) { routeSheet((int)(intptr_t)lv_event_get_user_data(e)); },
                             (void*)(intptr_t)i);
    lv_obj_set_style_border_color(card, lv_color_hex(c->followedRoute() == i ? 0x2979FF : TeamWire::colorRgb(sh.color)),
                                  LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 2, LV_PART_MAIN);
    double len = 0;
    for (int k = 1; k < sh.n; k++)
      len += TeamWire::distanceM(sh.lat5[k - 1] / 1e5, sh.lon5[k - 1] / 1e5, sh.lat5[k] / 1e5, sh.lon5[k] / 1e5);
    char dist[16], rb[40];
    teamroute::distanceText(len, dist, sizeof dist);
    rangeTo(sh.lat5[0] / 1e5, sh.lon5[0] / 1e5, rb, sizeof rb);
    snprintf(line, sizeof line, "%s  %s", sh.label[0] ? sh.label : TR("Route"), dist);
    lbl(card, line, &g_font_14, COLOR_TEXT);
    snprintf(line, sizeof line, "%s %s%s", TR("Starts"), rb, c->followedRoute() == i ? TR(" \xC2\xB7 following") : "");
    lbl(card, line, &g_font_12, COLOR_SUB);
  }
  if (!rn)
    lbl(b, TR("No routes yet. In MeshCore Open TAK, start Bloodhound's Directions and tap Share with team."), &g_font_12,
        COLOR_SUB);
}

static void openRolePicker();
static void openColourPicker();
static void openStatusPicker();
static void setupCycle(lv_event_t* e) {
  team::Core* c = s_core;
  switch ((int)(intptr_t)lv_event_get_user_data(e)) {
    case 0: c->cfg.enabled = !c->cfg.enabled; break;
    case 1: openRolePicker(); return;
    case 2: openColourPicker(); return;
    case 3: openStatusPicker(); return;
    case 4: c->cfg.share = !c->cfg.share; break;
  }
  c->identityChanged();
  renderMapMarkers();
  pageBuild();
}

// Role, Colour and Status open a list to choose from (they used to step to the
// next one on each tap).
static void setupApplied() {
  s_core->identityChanged();
  renderMapMarkers();
  pageBuild();
}
static char s_pick_lbl[TeamWire::kMaxRoleId + 1][TeamWire::kMaxRoleName + 4];
static const char* s_pick_ptr[TeamWire::kMaxRoleId + 1];
static int s_pick_role[TeamWire::kMaxRoleId + 1];   // list row -> role id
static uint32_t s_pick_rgb[TeamWire::kColors];

static void openRolePicker() {
  team::Core* c = s_core;
  if (!c) return;
  int n = 0, sel = -1;
  for (int id = 0; id <= TeamWire::kMaxRoleId; id++) {
    if (!c->roleAvailable(id)) continue;
    c->roleName(id, s_pick_lbl[n], sizeof s_pick_lbl[n]);
    s_pick_ptr[n] = s_pick_lbl[n];
    s_pick_role[n] = id;
    if (id == c->cfg.role) sel = n;
    n++;
  }
  openChoiceList(TR("Role"), s_pick_ptr, n, sel, [](int i) {
    if (!s_core) return;
    s_core->cfg.role = (uint8_t)s_pick_role[i];
    setupApplied();
  });
}
static void openColourPicker() {
  team::Core* c = s_core;
  if (!c) return;
  for (int i = 0; i < TeamWire::kColors; i++) {
    snprintf(s_pick_lbl[i], sizeof s_pick_lbl[i], "%s", TR(TeamWire::colorName(i)));
    s_pick_ptr[i] = s_pick_lbl[i];
    s_pick_rgb[i] = TeamWire::colorRgb(i);
  }
  openChoiceList(TR("Colour"), s_pick_ptr, TeamWire::kColors, c->cfg.color, [](int i) {
    if (!s_core) return;
    s_core->cfg.color = (uint8_t)i;
    setupApplied();
  }, s_pick_rgb);
}
static void openStatusPicker() {
  team::Core* c = s_core;
  if (!c) return;
  for (int i = 0; i < TeamWire::kStatuses; i++) {
    snprintf(s_pick_lbl[i], sizeof s_pick_lbl[i], "%s", i ? TR(TeamWire::statusName(i)) : TR("none"));
    s_pick_ptr[i] = s_pick_lbl[i];
  }
  openChoiceList(TR("Status"), s_pick_ptr, TeamWire::kStatuses, c->cfg.status, [](int i) {
    if (!s_core) return;
    s_core->cfg.status = (uint8_t)i;
    setupApplied();
  });
}

static void callsignDone(const char* t) {
  if (!t || !s_core) return;
  snprintf(s_core->cfg.callsign, sizeof s_core->cfg.callsign, "%s", t);
  s_core->identityChanged();
  pageBuild();
}

// ---- one-scan team setup (TEAM_SETUP.md) ---------------------------------------------------
#if CAP_SOUND_LINK
}  // namespace teamui
namespace soundui { static bool playRaw(const char* msg); static void listenFor(uint32_t ms); }
namespace teamui {
#endif

// This team's setup: the team channel, the watched channels, radio2's profile
// when it's on. False without a team channel on this radio.
static bool currentSetup(teamsetup::Setup& s) {
  team::Core* c = s_core;
  if (!c || !c->cfg.channel[0]) return false;
  const int idx = Io::channelIdx(c->cfg.channel);
  ChannelDetails cd;
  if (idx < 0 || !the_mesh.getChannel(idx, cd)) return false;
  s = teamsetup::Setup();
  snprintf(s.team.name, sizeof s.team.name, "%s", cd.name);
  memcpy(s.team.secret, cd.channel.secret, 16);
  watchfeed::Store* w = watch();
  for (int i = 0; w && i < MAX_GROUP_CHANNELS && s.nw < teamsetup::kMaxWatched; i++) {
    ChannelDetails d;
    if (i == idx || !the_mesh.getChannel(i, d) || !d.name[0] || !w->isWatched(d.name)) continue;
    if (!strcasecmp(d.name, "public") || threadIsPublicChannel(d.name)) continue;
    teamsetup::Channel& x = s.watched[s.nw++];
    snprintf(x.name, sizeof x.name, "%s", d.name);
    x.hashtag = d.name[0] == '#';
    if (!x.hashtag) memcpy(x.secret, d.channel.secret, 16);
  }
  const mediamesh::Config& m = mediamesh::config();
  if (mediamesh::supported() && m.mode != mediamesh::MODE_OFF) {
    s.has_mm = true;
    s.khz = (uint32_t)lroundf(m.p.freq * 1000.0f);
    s.bw_hz = (uint32_t)lroundf(m.p.bw * 1000.0f);
    s.sf = m.p.sf;
    s.cr = m.p.cr;
  }
  return true;
}

static void setupSummary(const teamsetup::Setup& s, char* out, size_t cap) {
  // The firmware builds as C++11: no generic lambdas, so each part is its own snprintf.
  size_t n = 0;
  auto adv = [&](int w) { if (w > 0 && n < cap) n += (size_t)w < cap - n ? (size_t)w : cap - n - 1; };
  if (!cap) return;
  out[0] = 0;
  adv(snprintf(out, cap, "%s %s", TR("Team channel:"), s.team.name));
  for (int i = 0; i < s.nw && n + 1 < cap; i++) {
    if (i) adv(snprintf(out + n, cap - n, ", %s", s.watched[i].name));
    else adv(snprintf(out + n, cap - n, "\n%s %s", TR("Watch:"), s.watched[i].name));
  }
  if (s.has_mm && n + 1 < cap)
    adv(snprintf(out + n, cap - n, "\n%s %.3f MHz, %g kHz, SF%u", TR("MediaMesh:"), s.khz / 1000.0, s.bw_hz / 1000.0,
                 (unsigned)s.sf));
}

static auto& s_setup_link = psramStatic<char[teamsetup::kMaxLink + 1]>();   // PSRAM

static void openShareSetup() {
  teamsetup::Setup* s = new (std::nothrow) teamsetup::Setup();
  if (!s) return;
  if (!currentSetup(*s) || !teamsetup::toLink(*s, s_setup_link, sizeof s_setup_link)) {
    delete s;
    toast(TR("Pick the team channel first"));
    return;
  }
  char summary[200];
  setupSummary(*s, summary, sizeof summary);
  delete s;
  lv_obj_t* body = createSettingsModal(TR("Share team setup"), SettingsModalKind::AddContact);   // reuse a kind
  lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(body, 6, LV_PART_MAIN);
  const lv_coord_t qs = LV_MIN((lv_coord_t)SC(170), (lv_coord_t)(lv_disp_get_ver_res(nullptr) - 70));
  lv_obj_t* qr = lv_qrcode_create(body, qs, lv_color_hex(0x000000), lv_color_hex(0xFFFFFF));
  lv_qrcode_update(qr, s_setup_link, strlen(s_setup_link));
  lv_obj_set_style_border_color(qr, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_border_width(qr, 6, LV_PART_MAIN);
  lv_obj_t* l = lbl(body, summary, &g_font_12, COLOR_TEXT);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
#if CAP_SOUND_LINK
  lv_obj_t* x = lv_btn_create(body);
  styleButton(x);
  lv_obj_set_size(x, LV_PCT(100), 34);
  lv_obj_add_event_cb(x, [](lv_event_t*) {
    char parts[teamsetup::kMaxParts][teamsetup::kSoundBytes + 1];
    const int n = teamsetup::soundParts(s_setup_link, parts);
    int queued = 0;
    for (int i = 0; i < n; i++) queued += soundui::playRaw(parts[i]) ? 1 : 0;
    char t[64];
    snprintf(t, sizeof t, TR("Playing the team setup (%d part(s))"), queued);
    toast(queued ? t : TR("Sound link is busy, try again"), 2000);
  }, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* xl = lv_label_create(x);
  lv_label_set_text(xl, TR("Play by sound"));
  useChainedFont(xl);
  lv_obj_center(xl);
#endif
  lbl(body, TR("Anyone who scans or hears this can join your team channel and see your positions."), &g_font_12,
      0xF2A33A);
}

// Joining: a setup heard by Sound link waits here for "Join".
static teamsetup::Setup* s_join = nullptr;

static int ensureChannel(const char* name, const uint8_t secret[16]) {
  for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
    ChannelDetails cd;
    if (the_mesh.getChannel(i, cd) && cd.name[0] && !memcmp(cd.channel.secret, secret, 16)) return i;
  }
  const int slot = the_mesh.findFirstEmptyChannelSlot();
  if (slot < 0 || !the_mesh.uiAddOrUpdateChannel(slot, name, secret)) return -1;
  if (g_lv.task) { g_lv.task->refreshThreadsFromMesh(); g_lv.dirty_threads = true; }
  return slot;
}

static void joinApply() {
  teamsetup::Setup* s = s_join;
  s_join = nullptr;
  team::Core* c = core();
  if (!s || !c) { delete s; return; }
  int missing = 0;
  if (ensureChannel(s->team.name, s->team.secret) >= 0) {
    snprintf(c->cfg.channel, sizeof c->cfg.channel, "%s", s->team.name);
    c->cfg.enabled = true;
    c->identityChanged();   // saves, and a position goes out soon
  } else {
    missing++;
  }
  watchfeed::Store* w = watch();
  for (int i = 0; i < s->nw; i++) {
    teamsetup::Channel& x = s->watched[i];
    uint8_t secret[16];
    char hashed[42];
    if (x.hashtag) {
      if (!deriveHashtagChannelKey(x.name, hashed, sizeof hashed, secret)) continue;
      snprintf(x.name, sizeof x.name, "%s", hashed);
    } else {
      memcpy(secret, x.secret, 16);
    }
    if (ensureChannel(x.name, secret) < 0) { missing++; continue; }
    if (w) w->setWatched(x.name, true);
  }
  if (w) watchSave();
  if (s->has_mm && mediamesh::supported())
    mmFindApply(s->khz / 1000.0f, s->bw_hz / 1000.0f, s->sf, s->cr, TR("MediaMesh set"));
  char t[96];
  if (missing) snprintf(t, sizeof t, TR("Joined %s (%d channel(s) didn't fit)"), s->team.name, missing);
  else snprintf(t, sizeof t, TR("Joined %s"), s->team.name);
  toast(t, 2500);
  delete s;
  s_io.changed();
}

}  // namespace teamui

// Sound link heard a whole team setup: ask, then set it up.
void teamSetupHeard(const teamsetup::Setup& s) {
  using namespace teamui;
  delete s_join;
  s_join = new (std::nothrow) teamsetup::Setup(s);
  if (!s_join) return;
  static char msg[320];
  char summary[200];
  setupSummary(s, summary, sizeof summary);
  snprintf(msg, sizeof msg, "%s\n\n%s\n\n%s", TR("Join this team?"), summary,
           s.has_mm ? TR("Joining adds the channels, turns team mode on, and sets radio2 to the team's MediaMesh.")
                    : TR("Joining adds the channels and turns team mode on."));
  showConfirm(msg, TR("Join"), joinApply);
}

namespace teamui {

static void buildSetup(lv_obj_t* b) {
  team::Core* c = s_core;
  char line[96];
  auto btn = [&](const char* t, int which, uint32_t bg = 0) {
    lv_obj_t* x = lv_btn_create(b);
    styleButton(x);
    lv_obj_set_size(x, LV_PCT(100), 32);
    if (bg) fill(x, bg);
    lv_obj_add_event_cb(x, setupCycle, LV_EVENT_CLICKED, (void*)(intptr_t)which);
    lv_obj_t* l = lv_label_create(x);
    lv_label_set_text(l, t);
    useChainedFont(l);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
  };
  snprintf(line, sizeof line, "%s: %s", TR("Team mode"), c->cfg.enabled ? TR("on") : TR("off"));
  btn(line, 0, c->cfg.enabled ? 0x2E7D32 : 0);
  lbl(b, TR("Team channel (private channels only):"), &g_font_12, COLOR_SUB);
  lv_obj_t* row = lv_obj_create(b);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_style_pad_column(row, 6, LV_PART_MAIN);
  lv_obj_set_style_pad_row(row, 6, LV_PART_MAIN);
  int shown = 0;
  for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
    ChannelDetails cd;
    if (!the_mesh.getChannel(i, cd) || !cd.name[0] || cd.name[0] == '#') continue;
    if (!strcasecmp(cd.name, "public")) continue;   // a pad-free broadcast to strangers
    lv_obj_t* x = lv_btn_create(row);
    styleButton(x);
    lv_obj_set_size(x, LV_SIZE_CONTENT, 30);
    lv_obj_set_style_pad_hor(x, 10, LV_PART_MAIN);
    if (!strcmp(cd.name, c->cfg.channel)) selected(x);
    lv_obj_add_event_cb(x, [](lv_event_t* e) {
      ChannelDetails d;
      if (s_core && the_mesh.getChannel((int)(intptr_t)lv_event_get_user_data(e), d)) {
        snprintf(s_core->cfg.channel, sizeof s_core->cfg.channel, "%s", d.name);
        s_core->identityChanged();
        pageBuild();
      }
    }, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    lv_obj_t* l = lv_label_create(x);
    lv_label_set_text(l, cd.name);
    useChainedFont(l);
    lv_obj_center(l);
    shown++;
  }
  if (!shown) lbl(row, TR("No private channels. Join a team by sound below, add the team's channel in Chats, or add it from MeshCore Open TAK with this radio connected."), &g_font_12, 0xF2A33A);
  {
    // One scan sets a newcomer up (TEAM_SETUP.md).
    lv_obj_t* x = lv_btn_create(b);
    styleButton(x);
    lv_obj_set_size(x, LV_PCT(100), 32);
    lv_obj_add_event_cb(x, [](lv_event_t*) { openShareSetup(); }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* l = lv_label_create(x);
    lv_label_set_text(l, TR("Share team setup (QR / sound)"));
    useChainedFont(l);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
#if CAP_SOUND_LINK
    x = lv_btn_create(b);
    styleButton(x);
    lv_obj_set_size(x, LV_PCT(100), 32);
    lv_obj_add_event_cb(x, [](lv_event_t*) {
      soundui::listenFor(120000);
      toast(TR("Listening for 2 minutes: play the team setup on the other phone or standalone"), 2500);
    }, LV_EVENT_CLICKED, nullptr);
    l = lv_label_create(x);
    lv_label_set_text(l, TR("Join a team by sound"));
    useChainedFont(l);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
#endif
  }
  {
    lv_obj_t* x = lv_btn_create(b);
    styleButton(x);
    lv_obj_set_size(x, LV_PCT(100), 32);
    lv_obj_add_event_cb(x, [](lv_event_t*) {
      fmTextPrompt(TR("Callsign"), s_core ? s_core->cfg.callsign : "", callsignDone);
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* l = lv_label_create(x);
    snprintf(line, sizeof line, "%s: %s", TR("Callsign"), c->callsign());
    lv_label_set_text(l, line);
    useChainedFont(l);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
  }
  snprintf(line, sizeof line, "%s: %s", TR("Role"), roleStr(c->cfg.role));
  btn(line, 1);
  snprintf(line, sizeof line, "%s: %s", TR("Colour"), TeamWire::colorName(c->cfg.color));
  btn(line, 2, TeamWire::colorRgb(c->cfg.color) == 0xFFFFFF ? 0 : TeamWire::colorRgb(c->cfg.color));
  snprintf(line, sizeof line, "%s: %s", TR("Status"), c->cfg.status ? TeamWire::statusName(c->cfg.status) : TR("none"));
  btn(line, 3);
  snprintf(line, sizeof line, "%s: %s", TR("Share my position"), c->cfg.share ? TR("yes") : TR("no"));
  btn(line, 4);
  lbl(b, TR("Your position comes from GPS, or from the position set in Settings when there is no fix. "
            "It is sent often while moving and every 10 minutes when still."), &g_font_12, COLOR_SUB);
}

static void buildWatch(lv_obj_t* b) {
  watchfeed::Store* w = watch();
  if (!w) { lbl(b, TR("Out of memory"), &g_font_12, COLOR_SUB); return; }
  char line[160];
  lbl(b, TR("Reports with a position on watched channels go on the map: IceBot's \xF0\x9F\x93\x8D lat,lon, "
            "a drone detector's DRONE reports (with the operator), shared map points."), &g_font_12, COLOR_SUB);
  {
    char ttl[16];
    watchfeed::ttlLabel(w->ttl, ttl, sizeof ttl);
    lv_obj_t* x = lv_btn_create(b);
    styleButton(x);
    lv_obj_set_size(x, LV_PCT(100), 32);
    lv_obj_add_event_cb(x, [](lv_event_t*) {
      if (!s_watch) return;
      s_watch->ttl = (uint8_t)((s_watch->ttl + 1) % watchfeed::kTtlChoices);
      watchSave();
      renderMapMarkers();
      pageBuild();
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* l = lv_label_create(x);
    snprintf(line, sizeof line, "%s: %s", TR("On map for"), ttl);
    lv_label_set_text(l, line);
    useChainedFont(l);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
  }
  if (maphl::Table* t = highlights()) {
    lv_obj_t* x = lv_btn_create(b);
    styleButton(x);
    lv_obj_set_size(x, LV_PCT(100), 32);
    lv_obj_add_event_cb(x, [](lv_event_t*) {
      if (!s_hl) return;
      s_hl->choice = (uint8_t)((s_hl->choice + 1) % maphl::kChoices);
      highlightsSave();
      renderMapMarkers();
      pageBuild();
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* l = lv_label_create(x);
    char v[16];
    if (t->minutes()) snprintf(v, sizeof v, "%u min", (unsigned)t->minutes());
    else snprintf(v, sizeof v, "%s", TR("off"));
    snprintf(line, sizeof line, "%s: %s", TR("Flash new points"), v);
    lv_label_set_text(l, line);
    useChainedFont(l);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
    lbl(b, TR("A teammate, pin or watched report that appears on the map flashes with a ring for this long."),
        &g_font_12, COLOR_SUB);
  }
  {
    lv_obj_t* x = lv_btn_create(b);
    styleButton(x);
    lv_obj_set_size(x, LV_PCT(100), 32);
    lv_obj_add_event_cb(x, [](lv_event_t*) {
      watchZoomSet(!watchZoomOn());
      pageBuild();
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* l = lv_label_create(x);
    snprintf(line, sizeof line, "%s: %s", TR("Zoom to new reports"), watchZoomOn() ? TR("on") : TR("off"));
    lv_label_set_text(l, line);
    useChainedFont(l);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
    lbl(b, TR("A new report with a position moves the map to it, zoomed in, while the map is open."),
        &g_font_12, COLOR_SUB);
  }
  lbl(b, TR("Watched channels:"), &g_font_12, COLOR_SUB);
  lv_obj_t* row = lv_obj_create(b);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_style_pad_column(row, 6, LV_PART_MAIN);
  lv_obj_set_style_pad_row(row, 6, LV_PART_MAIN);
  for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
    ChannelDetails cd;
    if (!the_mesh.getChannel(i, cd) || !cd.name[0]) continue;
    lv_obj_t* x = lv_btn_create(row);
    styleButton(x);
    lv_obj_set_size(x, LV_SIZE_CONTENT, 30);
    lv_obj_set_style_pad_hor(x, 10, LV_PART_MAIN);
    if (w->isWatched(cd.name)) selected(x);
    lv_obj_add_event_cb(x, [](lv_event_t* e) {
      ChannelDetails d;
      if (!s_watch || !the_mesh.getChannel((int)(intptr_t)lv_event_get_user_data(e), d)) return;
      if (!s_watch->setWatched(d.name, !s_watch->isWatched(d.name))) {
        toast(TR("Watching 8 channels already"));
        return;
      }
      watchSave();
      pageBuild();
    }, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    lv_obj_t* l = lv_label_create(x);
    lv_label_set_text(l, cd.name);
    useChainedFont(l);
    lv_obj_center(l);
  }
  // On the map now, newest first.
  const uint32_t now = millis();
  int order[watchfeed::kMaxItems], n = 0;
  for (int i = 0; i < watchfeed::kMaxItems; i++)
    if (w->live(w->items[i], now)) order[n++] = i;
  for (int i = 1; i < n; i++)
    for (int j = i; j > 0 && (now - w->items[order[j]].at_ms) < (now - w->items[order[j - 1]].at_ms); j--) {
      const int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t;
    }
  if (!n) {
    lbl(b, w->watchedCount() ? TR("Nothing on the map yet. Reports appear as they arrive.")
                             : TR("Pick the channels to watch."), &g_font_12, COLOR_SUB);
    return;
  }
  for (int k = 0; k < n; k++) {
    const watchfeed::Item& it = w->items[order[k]];
    lv_obj_t* card = rowCard(b, [](lv_event_t* e) { watchSheet((int)(intptr_t)lv_event_get_user_data(e)); },
                             (void*)(intptr_t)order[k]);
    const uint32_t col = it.r.track[0] ? 0xFF9800 : watchfeed::levelRgb(it.r.level);
    lv_obj_set_style_border_color(card, lv_color_hex(col), LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 2, LV_PART_MAIN);
    char head[64], rb[40];
    if (it.r.track[0]) watchfeed::droneLabel(it.r, it.has_course, it.course, head, sizeof head);
    else snprintf(head, sizeof head, "%s", it.r.title);
    rangeTo(it.r.lat, it.r.lon, rb, sizeof rb);
    snprintf(line, sizeof line, "%s  %s", head, rb);
    lbl(card, line, &g_font_14, COLOR_TEXT);
    const bool tx = txText(order[k]) != nullptr;
    snprintf(line, sizeof line, "%s · %lu min%s%s", channelName(it.channel), (unsigned long)((now - it.at_ms) / 60000),
             !tx && !it.r.track[0] && it.r.body[0] ? " · " : "", tx || it.r.track[0] ? "" : it.r.body);
    lbl(card, line, &g_font_12, COLOR_SUB);
    // A transit card's first lines, countdowns as of now.
    char txl[240];
    if (tx && txDescribe(it, order[k], false, txl, sizeof txl)) lbl(card, txl, &g_font_12, COLOR_TEXT);
  }
}

static void pageClose();
static void pageBack() { pageClose(); }

static void pageBuild() {
  if (!s_root || !s_core) return;
  if (s_body) { lv_obj_add_flag(s_body, LV_OBJ_FLAG_HIDDEN); lv_obj_del_async(s_body); }
  s_body = lv_obj_create(s_root);
  lv_obj_remove_style_all(s_body);
  lv_obj_set_size(s_body, LV_PCT(100), LV_PCT(100));
  lv_obj_set_flex_flow(s_body, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(s_body, 6, LV_PART_MAIN);
  lv_obj_set_style_pad_row(s_body, 6, LV_PART_MAIN);
  lv_obj_set_scroll_dir(s_body, LV_DIR_VER);
  // tabs
  lv_obj_t* tabs = lv_obj_create(s_body);
  lv_obj_remove_style_all(tabs);
  lv_obj_set_size(tabs, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(tabs, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(tabs, 6, LV_PART_MAIN);
  static const char* const names[4] = {"Team", "Pins", "Watch", "Setup"};
  static const int ids[4] = {0, 1, 3, 2};
  for (int k = 0; k < 4; k++) {
    const int i = ids[k];
    lv_obj_t* x = lv_btn_create(tabs);
    styleButton(x);
    lv_obj_set_size(x, LV_SIZE_CONTENT, 28);
    lv_obj_set_flex_grow(x, 1);
    if (i == s_tab) selected(x);
    lv_obj_add_event_cb(x, [](lv_event_t* e) { s_tab = (int)(intptr_t)lv_event_get_user_data(e); pageBuild(); },
                        LV_EVENT_CLICKED, (void*)(intptr_t)i);
    lv_obj_t* l = lv_label_create(x);
    lv_label_set_text(l, TR(names[k]));
    useChainedFont(l);
    lv_obj_center(l);
  }
  if (s_tab == 0) buildTeam(s_body);
  else if (s_tab == 1) buildPins(s_body);
  else if (s_tab == 3) buildWatch(s_body);
  else buildSetup(s_body);
  tint(s_body);
}

static void pageClose() {
  if (s_page_timer) { lv_timer_del(s_page_timer); s_page_timer = nullptr; }
  sheetClose();
  s_body = nullptr;
  if (s_root) { lv_obj_t* r = s_root; s_root = nullptr; appPageDeleteRootAsync(r); }
  appPageEnd(pageBack);
}

static void pageOpen() {
  if (!core()) { toast(TR("Out of memory")); return; }
  if (s_root) return;
  s_root = appPageCreateRoot(COLOR_BG);
  appPageBeginSlim("Team", pageBack);
  s_tab = (s_core->cfg.enabled && Io::channelIdx(s_core->cfg.channel) >= 0) ? 0 : 2;
  pageBuild();
  // A roster refresh at most every 3 s, and only when something changed.
  s_page_timer = lv_timer_create([](lv_timer_t*) {
    static uint32_t seen = 0;
    if (Io::s_version != seen && s_tab != 2) pageBuild();
    seen = Io::s_version;
  }, 3000, nullptr);
  if (g_statusbar.root) lv_obj_move_foreground(g_statusbar.root);
}

// ---- loop ---------------------------------------------------------------------------------------
static void loopTick() {
  if (s_zoom_pending) {   // auto-zoom: a new watched report, with the map in view
    s_zoom_pending = false;
    if (mapOnScreen()) mapJumpTo(s_zoom_lat, s_zoom_lon, kWatchZoomLevel);
  }
  team::Core* c = core();
  if (!c) return;
  const uint32_t now = millis();
  // Four times a second is plenty for beacons, alert repeats and route cues;
  // every pass of the UI loop read the GPS (and its motion) for nothing.
  static uint32_t last_core = 0;
  if (now - last_core >= 250) {
    last_core = now;
#if CAP_BIKE_GAMES
    bikeui::tick(now);
#endif
    c->tick();
  }
  static uint32_t map_seen = 0, last_map = 0;
  if (Io::s_version != map_seen && now - last_map > 1500) {
    map_seen = Io::s_version;
    last_map = now;
    if (s_map_canvas) renderMapMarkers();
  }
  // Alarm: show the newest unacknowledged alert; chime every 4 s while it is up.
  if (Io::s_alarm_pending || (!s_alarm && c->firstUnacked())) {
    Io::s_alarm_pending = false;
    if (team::AlertRec* r = c->firstUnacked()) alarmShow(*r);
  }
  if (s_alarm && now - s_alarm_beep_ms >= 4000) {
    s_alarm_beep_ms = now;
    if (g_lv.task && !g_lv.task->isBuzzerQuiet()) uiPlaySlot(TOUCH_SND_MEN);
  }
  // Note new points as they arrive (the map may not be open).
  static uint32_t last_hl = 0;
  if (now - last_hl >= 2000) { last_hl = now; highlightsObserve(now); }
  // A new point's highlight ran out: redraw without its ring.
  if (s_hl_active && (int32_t)(now - s_hl_until) >= 0) {
    s_hl_active = false;
    Io::s_version++;
  }
  // Watched reports leave the map as they pass the time limit.
  static uint32_t last_watch = 0;
  static int watch_live = 0;
  if (s_watch && now - last_watch >= 30000) {
    last_watch = now;
    const int live = s_watch->liveCount(now);
    if (live != watch_live) { watch_live = live; Io::s_version++; }
  }
  static uint32_t last_hound = 0;
  if (now - last_hound >= 1000) { last_hound = now; houndRefresh(); }
#if CAP_TAK_EXTRAS
  // A teammate answered our alert, report or pin.
  if (s_reply_toast_pending) {
    s_reply_toast_pending = false;
    toast(s_reply_toast, 3000);
    if (g_lv.task && !g_lv.task->isBuzzerQuiet()) uiPlaySlot(TOUCH_SND_MEN);
    if (s_root) pageBuild();
  }
#endif
}

}  // namespace teamui

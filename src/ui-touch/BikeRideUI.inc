// SPDX-License-Identifier: GPL-3.0-or-later
// Included beside the team screens. Bike beacons use their existing wire format.
#if defined(BLE_PIN_CODE) && !defined(WADA_WEB) && __has_include(<NimBLEDevice.h>)
#define WADA_BIKE_BLE 1
#include <NimBLEDevice.h>
#else
#define WADA_BIKE_BLE 0
#endif

namespace bikeui {
using teamui::sheetBtn;
using teamui::sheetOpen;
using teamui::sheetClose;
struct Phone { uint8_t type = 0, addr[6]{}; };
struct Config {
  bike::Boundary boundary;
  Phone phones[2];
  uint8_t phoneMask = 0, riding = 0, watching = 0;
  uint8_t teamId[8]{}, watchKey[4]{};
  uint16_t interval = 60, timeout = 180, repeat = 30;
};
static Config cfg;
static bool loaded, running, watchRunning, fresh;
static uint8_t connectedMask;
static uint32_t lastCheck, previousUtc, freshAt, lastTone, lastHeard;
static bike::Fence fence;
static bike::Absence phoneTime, watchTime;
static bike::Boundary draft;
static bool drafting;
static char pendingFence[48], pendingPhone[48];
static uint32_t nextNotice;
static lv_obj_t* liveStatus;
static void describe(char* out, size_t cap, team::Core& core);
static void open();
#ifndef WADA_WEB
static void tagPage();
#endif
static void boundaryPage();

static void toast(const char* text) { if (g_lv.task) g_lv.task->showAlert(text, 3200); }
static void load() {
  if (loaded) return;
  loaded = true;
  SdNvsPrefs p; Config v;
  if (!p.begin("bike", true)) return;
  if (p.getBytes("v1", &v, sizeof(v)) == sizeof(v) && v.interval >= 10 && v.interval <= 3600 &&
      v.timeout && v.timeout <= 3600 && v.repeat <= 3600 && v.phoneMask <= 3 &&
      v.riding <= 1 && v.watching <= 1 && (!v.boundary.count || bike::valid(v.boundary))) cfg = v;
  p.end();
}
static bool save(const Config& v) {
  SdNvsPrefs p;
  if (!p.begin("bike", false)) { toast("Bike settings storage unavailable"); return false; }
  const bool ok = p.putBytes("v1", &v, sizeof(v)) == sizeof(v);
  p.end();
  if (ok) cfg = v;
  else toast("Could not save bike settings");
  return ok;
}
static bool teamId(uint8_t out[8]) {
  team::Core* c = teamui::core();
  if (!c || !c->active() || c->cfg.channel[0] == '#' ||
      teamui::threadIsPublicChannel(c->cfg.channel)) return false;
  int idx = teamui::Io::channelIdx(c->cfg.channel);
  ChannelDetails cd;
  if (idx < 0 || !the_mesh.getChannel(idx, cd)) return false;
  mesh::Utils::sha256(out, 8, cd.channel.secret, sizeof(cd.channel.secret));
  return true;
}
static bool bind(Config& v) {
  uint8_t id[8];
  if (!teamId(id)) { toast("Set up a private family channel in Team first"); return false; }
  if (memcmp(id, v.teamId, 8)) {
    v.watching = v.riding = 0;
    memcpy(v.teamId, id, 8);
  }
  return true;
}
#if WADA_BIKE_BLE
static bool linked(const Phone& phone) {
  if (!NimBLEDevice::getInitialized()) return false;
  ble_addr_t id{};
  id.type = phone.type;
  memcpy(id.val, phone.addr, 6);
  ble_gap_conn_desc desc{};
  return !ble_gap_conn_find_by_addr(&id, &desc) && desc.role == BLE_GAP_ROLE_SLAVE &&
         desc.sec_state.encrypted && desc.sec_state.bonded && !ble_addr_cmp(&desc.peer_id_addr, &id);
}
#endif
static uint8_t phoneMask() {
  uint8_t mask = 0;
#if WADA_BIKE_BLE
  for (unsigned i = 0; i < 2; ++i)
    if ((cfg.phoneMask & (1 << i)) && linked(cfg.phones[i])) mask |= 1 << i;
#endif
  return mask;
}
static team::Member* watched(team::Core& c) {
  for (int i = 0; i < team::kMaxMembers; ++i) {
    team::Member* m = c.member(i);
    if (m && !memcmp(m->pos.key4, cfg.watchKey, 4)) return m;
  }
  return nullptr;
}
static void monitor(int index) {
  load(); team::Core* c = teamui::core();
  team::Member* m = c ? c->member(index) : nullptr;
  Config v = cfg;
  if (!m || !bind(v)) return;
  memcpy(v.watchKey, m->pos.key4, 4);
  v.watching = 1;
  if (save(v)) { watchRunning = false; open(); }
}
static void note(char* dest, const char* msg) {
  snprintf(dest, 48, "%s", msg);
  toast(msg);
}
static void tick(uint32_t now) {
  if (now - lastCheck < 1000) return;
  lastCheck = now;
  load();
  if (!cfg.riding && !cfg.watching && !running && !watchRunning) return;
  team::Core* c = teamui::s_core;
  uint8_t id[8];
  const bool same = teamId(id) && !memcmp(id, cfg.teamId, 8);
  const bool ride = cfg.riding && same && c->cfg.share;
  if (ride != running) {
    running = ride; c->ride_attempted = c->ride_sent = false;
    previousUtc = 0; fresh = false; fence = {}; lastTone = now - 3600000UL;
    phoneTime.reset(now); connectedMask = 0;
    pendingFence[0] = pendingPhone[0] = 0; nextNotice = now;
  }
  c->ride_interval = ride ? cfg.interval : 0;
  if (ride) {
    bike::Point point; uint32_t utc = 0;
    const bool fix = g_lv.task && g_lv.task->rideGps(point.lat, point.lon, utc);
    if (fix && utc != previousUtc) {
      fresh = previousUtc && utc > previousUtc && utc - previousUtc <= 15;
      previousUtc = utc; freshAt = now;
      if (fresh) {
        team::Fix f;
        teamui::s_io.fix(f);  // extras such as speed, heading and altitude
        f.lat = point.lat / 1e6; f.lon = point.lon / 1e6; f.gps = true;
        c->ride_fix = f;
        if (fence.observe(cfg.boundary, point, utc))
          note(pendingFence, fence.side == bike::Outside ? "Bike left the home boundary" : "Bike returned inside");
      }
    }
    if (!fix || now - freshAt > 15000) fresh = false;
    c->ride_has_fix = fresh;
    if (fence.side == bike::Outside && cfg.repeat && now - lastTone >= cfg.repeat * 1000UL) {
      if (soundPreviewAllowed(false)) uiPlaySlot(TOUCH_SND_MEN);
      lastTone = now;
    }
    connectedMask = phoneMask();
    if (cfg.phoneMask && phoneTime.update(now, connectedMask != 0, cfg.timeout)) {
      c->ride_attempted = c->ride_sent = false;
      note(pendingPhone, phoneTime.lost ? "Bike separated from owner phones" : "Bike near an owner phone again");
    }
    if (phoneTime.lost && c->ride_interval > 60) c->ride_interval = 60;
    if ((pendingFence[0] || pendingPhone[0]) && (int32_t)(now - nextNotice) >= 0) {
      char* msg = pendingFence[0] ? pendingFence : pendingPhone;
      if (teamui::s_io.sendText(c->cfg.channel, msg)) msg[0] = 0;
      nextNotice = now + 5000;
    }
  }
  if (cfg.watching && same) {
    team::Member* m = watched(*c);
    if (!watchRunning) {
      watchTime.reset(m ? m->heard_ms : now);
      lastHeard = m ? m->heard_ms : now;
    }
    watchRunning = true;
    bool heard = m && lastHeard != m->heard_ms;
    if (heard) lastHeard = m->heard_ms;
    if (watchTime.update(now, heard, cfg.timeout)) {
      toast(watchTime.lost ? "Bike rider check-in overdue" : "Bike rider heard again");
      if (soundPreviewAllowed(false)) uiPlaySlot(TOUCH_SND_DM);
    }
  } else watchRunning = false;
  if (liveStatus) {
    char body[380];
    describe(body, sizeof(body), *c);
    if (strcmp(body, lv_label_get_text(liveStatus))) lv_label_set_text(liveStatus, body);
  }
}

enum Field { Report, Timeout, Repeat, Radius };
static Field field;
static int value;
static void numbers() {
  char body[72];
  snprintf(body, sizeof(body), "%d %s", value, field == Radius ? "meters" : "seconds");
  const char* names[] = {"Position interval", "Missing timeout", "Return-home sound (0 mutes)", "Circle radius"};
  lv_obj_t* card = sheetOpen(names[field], body);
  for (int d : {-60,-10,-1,1,10,60}) {
    char label[8]; snprintf(label, sizeof(label), "%+d", d);
    sheetBtn(card, label, [](lv_event_t* e) {
      int lo = field == Report ? 10 : field == Repeat ? 0 : 1;
      int hi = field == Radius ? 100000 : 3600;
      value = constrain(value + (int)(intptr_t)lv_event_get_user_data(e), lo, hi);
      numbers();
    }, (void*)(intptr_t)d);
  }
  sheetBtn(card, "Save", [](lv_event_t*) {
    if (field == Radius) { draft.radius = value; boundaryPage(); return; }
    Config v = cfg;
    if (field == Report) v.interval = value;
    else if (field == Timeout) v.timeout = value;
    else v.repeat = value;
    if (save(v)) open();
  });
  sheetBtn(card, "Cancel", [](lv_event_t*) { if (field == Radius) boundaryPage(); else open(); });
}
static void number(Field f, int initial) { field = f; value = initial; numbers(); }

static void boundaryPage() {
  if (!drafting) { draft = cfg.boundary; drafting = true; }
  char body[128];
  snprintf(body, sizeof(body), "%u points. One = circle (%lu m), two = opposite box corners, 3-9 = polygon. Pan the map to each point.",
           draft.count, (unsigned long)draft.radius);
  lv_obj_t* card = sheetOpen("Ride boundary", body);
  sheetBtn(card, "Add map center", [](lv_event_t*) {
    if (draft.count >= 9) { toast("Nine points already selected"); return; }
    if (!s_map_view_inited) { toast("Open the map and choose a point first"); return; }
    draft.points[draft.count++] = {(int32_t)lround(s_map_center_lat * 1e6), (int32_t)lround(s_map_center_lon * 1e6)};
    boundaryPage();
  });
  sheetBtn(card, "Back to map", [](lv_event_t*) { sheetClose(); goToTab(MAP_TAB_INDEX); toast("Pan the map, then reopen Kid on a bike > Boundary"); });
  sheetBtn(card, "Undo point", [](lv_event_t*) { if (draft.count) --draft.count; boundaryPage(); });
  sheetBtn(card, "Radius", [](lv_event_t*) { number(Radius, draft.radius); });
  sheetBtn(card, "Clear draft", [](lv_event_t*) { draft.count = 0; boundaryPage(); });
  sheetBtn(card, "Save boundary", [](lv_event_t*) {
    if (draft.count && !bike::valid(draft)) { toast("Check corners, duplicate points or crossing edges"); return; }
    Config v = cfg; v.boundary = draft;
    if (save(v)) { fence = {}; pendingFence[0] = 0; drafting = false; open(); }
  });
  sheetBtn(card, "Discard edits", [](lv_event_t*) { drafting = false; open(); });
}
#if WADA_BIKE_BLE
static void phones() {
  lv_obj_t* card = sheetOpen("Owner phones", "Pair in the MeshCore phone app. Connect one new phone, then save it in a slot.");
  for (unsigned i = 0; i < 2; ++i) {
    const bool filled = cfg.phoneMask & (1 << i);
    char label[64];
    snprintf(label, sizeof(label), "%s phone %u%s", filled ? "Forget" : "Save connected", i+1,
             filled ? (linked(cfg.phones[i]) ? " (nearby)" : " (away)") : "");
    sheetBtn(card, label, [](lv_event_t* e) {
      unsigned slot = (uintptr_t)lv_event_get_user_data(e);
      Config v = cfg;
      if (v.phoneMask & (1 << slot)) { v.phoneMask &= ~(1 << slot); v.phones[slot] = {}; }
      else {
        unsigned candidates = 0;
        if (NimBLEDevice::getInitialized()) {
          // Enumerate the bond store once. Match resolved identities on encrypted peripheral links.
          ble_addr_t bonds[MYNEWT_VAL(BLE_STORE_MAX_BONDS)];
          int count = 0;
          if (!ble_store_util_bonded_peers(bonds, &count, MYNEWT_VAL(BLE_STORE_MAX_BONDS))) {
            for (int j = 0; j < count; ++j) {
              Phone p; p.type = bonds[j].type; memcpy(p.addr, bonds[j].val, 6);
              if (!linked(p) || ((v.phoneMask & (1 << (1-slot))) && !memcmp(&v.phones[1-slot], &p, sizeof(p)))) continue;
              ++candidates; v.phones[slot] = p;
            }
          }
        }
        if (candidates != 1) { toast("Connect exactly one new bonded phone"); return; }
        v.phoneMask |= 1 << slot;
      }
      if (save(v)) { phoneTime.reset(millis()); pendingPhone[0] = 0; phones(); }
    }, (void*)(uintptr_t)i);
  }
  sheetBtn(card, "Back", [](lv_event_t*) { open(); });
}
#endif
static void describe(char* body, size_t cap, team::Core& c) {
  snprintf(body, cap, "Team: %.32s\nRide: %s. GPS: %s. Boundary: %s.\nOwner phones: %s. Rider monitor: %s.\nPosition %u s / timeout %u s / sound %u s.",
           c.cfg.channel[0] ? c.cfg.channel : "select a private family team", running ? "active" : cfg.riding ? "paused" : "off",
           fresh ? "live" : "waiting", !cfg.boundary.count ? "off" : fence.side == bike::Outside ? "outside" : fence.side == bike::Inside ? "inside" : "waiting for fixes",
           !cfg.phoneMask ? "off" : !running ? "idle" : connectedMask ? "nearby" : phoneTime.lost ? "separated" : "waiting",
           !cfg.watching ? "off" : !watchRunning ? "paused" : watchTime.lost ? "overdue" : "watching",
           cfg.interval, cfg.timeout, cfg.repeat);
  team::Member* rider = watchRunning ? watched(c) : nullptr;
  if (rider) {
    size_t used = strlen(body);
    snprintf(body + used, cap - used, "\n%.32s: heard %lu s ago.", rider->pos.callsign,
             (unsigned long)((millis() - rider->heard_ms) / 1000));
    used = strlen(body);
    if (rider->pos.has_pos)
      snprintf(body + used, cap - used, " Position %lu s ago%s.", (unsigned long)((millis() - rider->position_ms) / 1000),
               rider->latest_has_position ? "" : " (last report had no fix)");
    else snprintf(body + used, cap - used, " Waiting for a position.");
  }
}
static void open() {
  load();
  team::Core* c = teamui::core();
  if (!c) { toast("Team memory unavailable"); return; }
  char body[380];
  describe(body, sizeof(body), *c);
  lv_obj_t* card = sheetOpen("Kid on a bike", body);
  liveStatus = lv_obj_get_child(card, 1);
  lv_obj_add_event_cb(liveStatus, [](lv_event_t* e) {
    if (liveStatus == lv_event_get_target(e)) liveStatus = nullptr;
  }, LV_EVENT_DELETE, nullptr);
  sheetBtn(card, cfg.riding ? "End ride" : "Start ride", [](lv_event_t*) {
    Config v = cfg;
    if (v.riding) v.riding = 0;
    else {
      if (!bind(v)) return;
      if (!teamui::s_core->cfg.share || !g_lv.task->getGPSState()) { toast("Enable GPS and Team position sharing first"); return; }
      v.riding = 1;
    }
    if (save(v)) { lastCheck = millis()-1000; tick(millis()); open(); }
  });
#ifndef WADA_WEB
  sheetBtn(card, "RAK tag monitor", [](lv_event_t*) { tagPage(); });
#endif
  sheetBtn(card, "Team / riders", [](lv_event_t*) { sheetClose(); teamui::pageOpen(); });
  if (cfg.watching) sheetBtn(card, "Stop monitoring", [](lv_event_t*) {
    Config v = cfg; v.watching = 0; if (save(v)) { watchRunning = false; open(); }
  });
  sheetBtn(card, "Position interval", [](lv_event_t*) { number(Report, cfg.interval); });
  sheetBtn(card, "Missing timeout", [](lv_event_t*) { number(Timeout, cfg.timeout); });
  sheetBtn(card, "Boundary", [](lv_event_t*) { boundaryPage(); });
  sheetBtn(card, "Return-home sound", [](lv_event_t*) { number(Repeat, cfg.repeat); });
#if WADA_BIKE_BLE
  sheetBtn(card, "Owner phones", [](lv_event_t*) { phones(); });
#endif
  if (cfg.watching) sheetBtn(card, "Rider on map", [](lv_event_t*) {
    auto* c = teamui::core();
    auto* m = c && watchRunning ? watched(*c) : nullptr;
    if (!m || !m->pos.has_pos) { toast("Waiting for a rider position"); return; }
    sheetClose(); teamui::showOnMap(m->pos.lat, m->pos.lon);
  });
  sheetBtn(card, "Close", [](lv_event_t*) { sheetClose(); });
}
}

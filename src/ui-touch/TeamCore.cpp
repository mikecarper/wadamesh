// SPDX-License-Identifier: GPL-3.0-or-later
#include "TeamCore.h"
#include "device_caps.h"   // CAP_TAK_EXTRAS: who-heard-what is off where the app slot is full

#include <new>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

namespace team {

static bool keyEq(const uint8_t* a, const uint8_t* b) { return memcmp(a, b, 4) == 0; }

// ---- settings ---------------------------------------------------------------
// "/teamcfg": enabled|role|color|status|share|channel|callsign
void Core::saveSettings() {
  char buf[96];
  const int n = snprintf(buf, sizeof buf, "%d|%u|%u|%u|%d|%s|%s", cfg.enabled ? 1 : 0, cfg.role, cfg.color,
                         cfg.status, cfg.share ? 1 : 0, cfg.channel, cfg.callsign);
  io_->writeFile("teamcfg", (const uint8_t*)buf, (size_t)n);
}

void Core::loadSettings() {
  char buf[97];
  size_t n = 0;
  if (!io_->readFile("teamcfg", (uint8_t*)buf, sizeof buf - 1, &n)) return;
  buf[n] = 0;
  char* f[7] = {buf};
  int k = 1;
  for (char* c = buf; *c && k < 7; ++c)
    if (*c == '|') { *c = 0; f[k++] = c + 1; }
  if (k < 7) return;
  cfg.enabled = atoi(f[0]) != 0;
  cfg.role = (uint8_t)atoi(f[1]);
  if (cfg.role > TeamWire::kMaxRoleId) cfg.role = 0;
  cfg.color = (uint8_t)(atoi(f[2]) % TeamWire::kColors);
  cfg.status = (uint8_t)(atoi(f[3]) % TeamWire::kStatuses);
  cfg.share = atoi(f[4]) != 0;
  snprintf(cfg.channel, sizeof cfg.channel, "%s", f[5]);
  snprintf(cfg.callsign, sizeof cfg.callsign, "%s", f[6]);
}

// "/teampins": [len u8][encoded marker] ...
static constexpr size_t kPinsFileMax = kMaxMarkers * 120;

void Core::saveMarkers() {
  uint8_t* buf = (uint8_t*)malloc(kPinsFileMax);   // PSRAM on the device; never on the loop stack
  if (!buf) return;
  size_t n = 0;
  for (int i = 0; i < kMaxMarkers; i++) {
    if (!mused_[i]) continue;
    uint8_t one[TeamWire::kMaxPayload];
    const size_t k = TeamWire::encode(markers_[i], one, sizeof one);
    if (!k || n + 1 + k > kPinsFileMax) continue;
    buf[n++] = (uint8_t)k;
    memcpy(buf + n, one, k);
    n += k;
  }
  io_->writeFile("teampins", buf, n);
  free(buf);
}

void Core::loadMarkers() {
  uint8_t* buf = (uint8_t*)malloc(kPinsFileMax);
  if (!buf) return;
  size_t n = 0;
  if (!io_->readFile("teampins", buf, kPinsFileMax, &n)) { free(buf); return; }
  size_t o = 0;
  int slot = 0;
  while (o < n && slot < kMaxMarkers) {
    const size_t k = buf[o++];
    if (o + k > n) break;
    if (TeamWire::parse(buf + o, k, markers_[slot])) mused_[slot++] = true;
    o += k;
  }
  free(buf);
}

void Core::begin() {
  loadSettings();
  loadMarkers();
  loadRoles();
  loadRoutes();
}

// ---- routes -----------------------------------------------------------------------
// "/teamroutes": [len u8][payload as it arrived] ...
static constexpr size_t kRoutesFileMax = kMaxRoutes * (TeamWire::kMaxPayload + 1);

void Core::saveRoutes() {
  uint8_t* buf = (uint8_t*)malloc(kRoutesFileMax);
  if (!buf) return;
  size_t n = 0;
  for (auto& r : routes_) {
    if (!r.used || !r.len) continue;
    buf[n++] = r.len;
    memcpy(buf + n, r.raw, r.len);
    n += r.len;
  }
  io_->writeFile("teamroutes", buf, n);
  free(buf);
}

void Core::loadRoutes() {
  uint8_t* buf = (uint8_t*)malloc(kRoutesFileMax);
  if (!buf) return;
  size_t n = 0;
  if (io_->readFile("teamroutes", buf, kRoutesFileMax, &n)) {
    loading_routes_ = true;
    size_t o = 0;
    while (o < n) {
      const size_t k = buf[o++];
      if (o + k > n) break;
      onShape(buf + o, k);
      o += k;
    }
    loading_routes_ = false;
  }
  free(buf);
}

// Only routes are kept (lines and areas are the app's for now).
void Core::onShape(const uint8_t* p, size_t n) {
  if (n > TeamWire::kMaxPayload) return;
  TeamWire::Shape* s = (TeamWire::Shape*)malloc(sizeof(TeamWire::Shape));
  if (!s) return;
  new (s) TeamWire::Shape();
  const bool ok = TeamWire::parse(p, n, *s) && (s->isRoute() || s->deleted);
  int slot = -1;
  if (ok) {
    for (int i = 0; i < kMaxRoutes && slot < 0; i++)
      if (routes_[i].used && keyEq(routes_[i].key4, s->key4) && routes_[i].id == s->id) slot = i;
    if (slot >= 0) {
      RouteRec& r = routes_[slot];
      // A newer edit wins; a delete wins over the same time.
      if (s->time < r.time || (s->time == r.time && (r.deleted || !s->deleted))) slot = -2;
    } else if (!s->deleted) {
      for (int i = 0; i < kMaxRoutes && slot < 0; i++) if (!routes_[i].used) slot = i;
      if (slot < 0) {   // full: the oldest goes (tombstones first)
        slot = 0;
        for (int i = 1; i < kMaxRoutes; i++) {
          const RouteRec& a = routes_[i];
          const RouteRec& b = routes_[slot];
          if ((a.deleted && !b.deleted) || (a.deleted == b.deleted && a.time < b.time)) slot = i;
        }
      }
    } else {
      slot = -2;   // a delete for a route never seen: nothing to do
    }
  }
  if (ok && slot >= 0) {
    RouteRec& r = routes_[slot];
    r.used = true;
    r.deleted = s->deleted;
    memcpy(r.key4, s->key4, 4);
    r.id = s->id;
    r.time = s->time;
    r.len = (uint8_t)n;
    memcpy(r.raw, p, n);
    if (hound_ == 3 && follow_idx_ == slot) {
      if (r.deleted) hound_ = 0;
      else followRoute(slot, false);   // an edit: follow the new one from where we are
    }
  }
  free(s);
  if (ok && slot >= 0 && !loading_routes_) {
    saveRoutes();
    io_->changed();
  }
}

bool Core::routeShape(int i, TeamWire::Shape& out) const {
  const RouteRec* r = route(i);
  return r && TeamWire::parse(r->raw, r->len, out) && out.n > 0;
}

bool Core::followRoute(int i, bool reverse) {
  TeamWire::Shape* s = (TeamWire::Shape*)malloc(sizeof(TeamWire::Shape));
  if (!s) return false;
  new (s) TeamWire::Shape();
  const bool ok = routeShape(i, *s);
  if (ok) {
    Fix f;
    const bool pos = io_->fix(f);
    follow_.start(s->lat5, s->lon5, s->n, reverse, pos, f.lat, f.lon);
    hound_ = 3;
    follow_idx_ = i;
    follow_id_ = s->id;
    memcpy(follow_key_, s->key4, 4);
    snprintf(hound_label_, sizeof hound_label_, "%s", s->label[0] ? s->label : "Route");
    cue_ = teamroute::Cue();
    updateFollow();
    io_->changed();
  }
  free(s);
  return ok;
}

bool Core::followTrack(const int32_t* lat5, const int32_t* lon5, int n, const char* label) {
  if (n < 2) return false;
  int32_t* la = (int32_t*)malloc(sizeof(int32_t) * teamroute::kMax);
  int32_t* lo = (int32_t*)malloc(sizeof(int32_t) * teamroute::kMax);
  if (!la || !lo) { free(la); free(lo); return false; }
  const int k = teamroute::simplifyTrack(lat5, lon5, n, la, lo, teamroute::kMax);
  Fix f;
  const bool pos = io_->fix(f);
  follow_.start(la, lo, k, true, pos, f.lat, f.lon);   // from the newest point back to the first
  free(la);
  free(lo);
  hound_ = 3;
  follow_idx_ = -1;   // not a shared route
  follow_id_ = 0;
  memset(follow_key_, 0, 4);
  snprintf(hound_label_, sizeof hound_label_, "%s", label && *label ? label : "Back-track");
  cue_ = teamroute::Cue();
  updateFollow();
  io_->changed();
  return true;
}

void Core::updateFollow() {
  if (hound_ != 3) return;
  Fix f;
  if (!io_->fix(f)) return;
  cue_ = follow_.update(f.lat, f.lon);
}

// ---- roles --------------------------------------------------------------------
// "/teamroles": [len u8][encoded role edit] ...
static constexpr size_t kRolesFileMax = (TeamWire::kMaxRoleId + 1) * 40;

void Core::saveRoles() {
  uint8_t* buf = (uint8_t*)malloc(kRolesFileMax);
  if (!buf) return;
  size_t n = 0;
  for (int i = 0; i <= TeamWire::kMaxRoleId; i++) {
    if (!rused_[i]) continue;
    uint8_t one[40];
    const size_t k = TeamWire::encode(roles_[i], one, sizeof one);
    if (!k || n + 1 + k > kRolesFileMax) continue;
    buf[n++] = (uint8_t)k;
    memcpy(buf + n, one, k);
    n += k;
  }
  io_->writeFile("teamroles", buf, n);
  free(buf);
}

void Core::loadRoles() {
  uint8_t* buf = (uint8_t*)malloc(kRolesFileMax);
  if (!buf) return;
  size_t n = 0;
  if (!io_->readFile("teamroles", buf, kRolesFileMax, &n)) { free(buf); return; }
  size_t o = 0;
  while (o < n) {
    const size_t k = buf[o++];
    if (o + k > n) break;
    TeamWire::RoleDef d;
    if (TeamWire::parse(buf + o, k, d)) { roles_[d.role_id] = d; rused_[d.role_id] = true; }
    o += k;
  }
  free(buf);
}

void Core::onRoleDef(const TeamWire::RoleDef& d) {
  if (rused_[d.role_id] && !TeamWire::beats(d, roles_[d.role_id])) return;
  roles_[d.role_id] = d;
  rused_[d.role_id] = true;
  saveRoles();
  io_->changed();
}

const TeamWire::RoleDef* Core::roleEdit(int id) const {
  if (id < 0 || id > TeamWire::kMaxRoleId || !rused_[id] || roles_[id].reset) return nullptr;
  return &roles_[id];
}

bool Core::roleAvailable(int id) const {
  return (id >= 0 && id < TeamWire::kRoles) || roleEdit(id) != nullptr;
}

void Core::roleName(int id, char* out, size_t cap) const {
  const TeamWire::RoleDef* d = roleEdit(id);
  if (d && d->name[0]) snprintf(out, cap, "%s", d->name);
  else if (id >= 0 && id < TeamWire::kRoles) snprintf(out, cap, "%s", TeamWire::roleName(id));
  else snprintf(out, cap, "Role %d", id);
}

uint16_t Core::roleIcon(int id) const {
  const TeamWire::RoleDef* d = roleEdit(id);
  return d ? d->icon : 0;
}

char Core::roleLetter(int id) const {
  const TeamWire::RoleDef* d = roleEdit(id);
  if (d)
    for (const char* c = d->name; *c; ++c) {
      if (*c >= 'a' && *c <= 'z') return (char)(*c - 32);
      if ((*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9')) return *c;
    }
  return id < TeamWire::kRoles ? TeamWire::roleLetter(id) : '?';
}

int Core::nextRole(int id) const {
  for (int k = 1; k <= TeamWire::kMaxRoleId + 1; k++) {
    const int c = (id + k) % (TeamWire::kMaxRoleId + 1);
    if (roleAvailable(c)) return c;
  }
  return 0;
}

// ---- sending -----------------------------------------------------------------
bool Core::sendRaw(const uint8_t* p, size_t n) {
  if (!active() || !n) return false;
  return io_->send(cfg.channel, p, n);
}

// With no GPS fix and no location set, still a beacon ("here, no position",
// TeamWire.h), so teammates list this radio.
bool Core::sendPosition() {
  Fix f;
  if (!active()) return false;
  const bool has = ride_interval ? ride_has_fix : io_->fix(f);
  if (ride_interval && has) f = ride_fix;
  TeamWire::Position p;
  io_->selfKey4(p.key4);
  p.has_pos = has;
  p.lat = has ? f.lat : 0;
  p.lon = has ? f.lon : 0;
  p.has_alt = has && f.has_alt;
  p.alt_m = (int16_t)f.alt_m;
  p.has_motion = has && f.has_motion;
  p.speed_mps = f.speed_mps;
  p.heading_deg = f.heading_deg;
  p.accuracy_m = has ? f.accuracy_m : 0;
  p.battery_pct = io_->batteryPct();
  p.role = cfg.role;
  p.color = cfg.color;
  p.status = cfg.status;
  p.from_radio = true;
  p.sos = own_active_;
  snprintf(p.callsign, sizeof p.callsign, "%s", callsign());
#if CAP_TAK_EXTRAS
  p.has_name_hash = true;
  p.name_hash = nameHash(io_->nodeName());
  p.heard_n = heard_recent_n_;
  for (uint8_t i = 0; i < heard_recent_n_; i++) p.heard[i] = heard_recent_[i];
#endif
  // A new beacon number each time, so a still radio's beacons aren't dropped
  // as already seen (TeamWire.h).
  if (!seq_set_) { seq_ = (uint16_t)io_->random32(); seq_set_ = true; }
  p.has_seq = true;
  p.seq = ++seq_;
  uint8_t buf[TeamWire::kMaxPayload];
  const size_t n = TeamWire::encode(p, buf, sizeof buf);
  if (!sendRaw(buf, n)) return false;
  force_beacon_ = false;
  beaconed_ = true;
  last_beacon_ms_ = io_->nowMs();
  if (has) {
    last_fix_ = f;
    compatAfterBeacon();   // TEAM's #TEL needs a position
  }
  io_->changed();
  return true;
}

void Core::raiseAlert(uint8_t type, const char* note) {
  Fix f;
  const bool has = io_->fix(f);
  own_ = TeamWire::Alert();
  io_->selfKey4(own_.key4);
  own_.id = (uint16_t)io_->random32();
  own_.active = true;
  own_.type = type;
  own_.has_pos = has;
  own_.lat = has ? f.lat : 0;
  own_.lon = has ? f.lon : 0;
  own_.accuracy_m = has ? f.accuracy_m : 0;
  snprintf(own_.callsign, sizeof own_.callsign, "%s", callsign());
  snprintf(own_.note, sizeof own_.note, "%s", note ? note : "");
  own_active_ = true;
  own_sends_ = 0;
  own_next_ms_ = io_->nowMs();
  cancel_sends_ = 2;   // a cancel still pending is overtaken
  force_beacon_ = true;
  tick();
  io_->changed();
}

void Core::cancelAlert() {
  if (!own_active_) return;
  own_active_ = false;
  cancel_ = own_;
  cancel_.active = false;
  cancel_sends_ = 0;
  cancel_next_ms_ = io_->nowMs();
  own_sends_ = 3;
  force_beacon_ = true;
  tick();
  io_->changed();
}

void Core::tick() {
  const uint32_t now = io_->nowMs();
  if (!active()) return;
  if (cap_due_ && (int32_t)(now - cap_due_ms_) >= 0) {   // just found TEAM users: introduce ourselves
    cap_due_ = false;
    compatCap(true);
  }
  updateFollow();
  // Own alert: three sends (radio collisions happen), then it rides on every position.
  if (own_active_ && own_sends_ < 3 && (int32_t)(now - own_next_ms_) >= 0) {
    uint8_t buf[TeamWire::kMaxPayload];
    own_.has_nonce = true;   // each repeat new bytes, or the mesh drops it as seen
    own_.nonce = own_sends_;
    if (sendRaw(buf, TeamWire::encode(own_, buf, sizeof buf))) {
      own_sends_++;
      own_next_ms_ = now + (own_sends_ == 1 ? 15000 : 30000);
    }
  }
  if (cancel_sends_ < 2 && (int32_t)(now - cancel_next_ms_) >= 0) {
    uint8_t buf[TeamWire::kMaxPayload];
    cancel_.has_nonce = true;
    cancel_.nonce = cancel_sends_;
    if (sendRaw(buf, TeamWire::encode(cancel_, buf, sizeof buf))) {
      cancel_sends_++;
      cancel_next_ms_ = now + 10000;
    }
  }
  if (!cfg.share) return;
  if (ride_interval) {
    if ((!ride_attempted || now - ride_attempt >= 5000) &&
        (!ride_sent || now - last_beacon_ms_ >= 1000UL * ride_interval ||
         (force_beacon_ && now - last_beacon_ms_ >= 5000))) {
      ride_attempted = true;
      ride_attempt = now;
      if (sendPosition()) ride_sent = true;
    }
    return;
  }
  Fix f;
  const long since = beaconed_ ? (long)((now - last_beacon_ms_) / 1000) : -1;
  if (!io_->fix(f)) {
    // No fix or location set: still say we're here (TeamWire.h), as often as
    // a still member, sooner when a teammate is heard or an alert is up.
    if (heard_beacon_due_ms_ && (int32_t)(now - heard_beacon_due_ms_) >= 0) {
      heard_beacon_due_ms_ = 0;
      heard_beacon_last_ms_ = now;
      heard_beaconed_ = true;
      force_beacon_ = true;
    }
    if (since < 0 || (force_beacon_ && since >= 5) || since >= 600 || (own_active_ && since >= 60)) sendPosition();
    return;
  }
  double turn = 0;
  if (beaconed_ && f.has_motion && last_fix_.has_motion) {
    turn = fmod(f.heading_deg - last_fix_.heading_deg + 540.0, 360.0) - 180.0;
  }
  const double moved = beaconed_ ? TeamWire::distanceM(last_fix_.lat, last_fix_.lon, f.lat, f.lon) : 0;
  if (heard_beacon_due_ms_ && (int32_t)(now - heard_beacon_due_ms_) >= 0) {   // we heard a teammate: say so
    heard_beacon_due_ms_ = 0;
    heard_beacon_last_ms_ = now;
    heard_beaconed_ = true;
    force_beacon_ = true;
  }
  bool send = force_beacon_ && (since < 0 || since >= 5);
  if (!send) send = TeamWire::shouldBeacon(since, moved, f.has_motion ? f.speed_mps : 0, turn);
  if (!send && own_active_ && since >= 60) send = true;
  if (send) sendPosition();
}

// ---- receiving -----------------------------------------------------------------
void Core::onPayload(const char* channel, const uint8_t* p, size_t n) {
  if (!cfg.enabled || !channel || strcmp(channel, cfg.channel) != 0) return;
  if (n < 5) return;
  uint8_t me[4];
  io_->selfKey4(me);
  if (keyEq(p + 1, me)) return;
  switch (p[0]) {
    case TeamWire::kPosition: { TeamWire::Position x; if (TeamWire::parse(p, n, x)) onPosition(x); break; }
    case TeamWire::kAlert: { TeamWire::Alert x; if (TeamWire::parse(p, n, x)) onAlert(x); break; }
    case TeamWire::kMarker: { TeamWire::Marker x; if (TeamWire::parse(p, n, x)) onMarker(x); break; }
    case TeamWire::kRole: { TeamWire::RoleDef x; if (TeamWire::parse(p, n, x)) onRoleDef(x); break; }
    case TeamWire::kShape: onShape(p, n); break;
    case TeamWire::kReply: { TeamWire::Reply x; if (TeamWire::parse(p, n, x)) io_->reply(x); break; }
    default: break;   // reports: not on this radio yet
  }
}

void Core::noteChannelText(const char* channel, const char* sender, uint32_t sent, const char* body) {
#if !CAP_TAK_EXTRAS
  (void)channel; (void)sender; (void)sent; (void)body;
  return;
#endif
  if (!active() || !sender || !body) return;
  const uint16_t from = nameHash(sender);
  bool teammate = channel && !strcmp(channel, cfg.channel);
  for (int i = 0; i < kMaxMembers && !teammate; i++)
    teammate = members_[i].used && members_[i].pos.has_name_hash && members_[i].pos.name_hash == from;
  if (!teammate) return;
  const uint16_t h = quickreply::reportHash(sent, sender, body);
  for (uint8_t i = 0; i < heard_recent_n_; i++)
    if (heard_recent_[i] == h) return;
  if (heard_recent_n_ == 4) {
    memmove(heard_recent_, heard_recent_ + 1, 3 * sizeof heard_recent_[0]);
    heard_recent_n_ = 3;
  }
  heard_recent_[heard_recent_n_++] = h;
  const uint32_t now = io_->nowMs();
  if (heard_beacon_due_ms_ || (heard_beaconed_ && now - heard_beacon_last_ms_ < 120000UL)) return;
  heard_beacon_due_ms_ = now + 15000UL + io_->random32() % 31000UL;
  if (!heard_beacon_due_ms_) heard_beacon_due_ms_ = 1;
}

int Core::heardBy(uint16_t msg, uint8_t (*keys)[4], uint32_t* ms, int cap) const {
  int n = 0;
  for (const HeardRec& r : heard_by_) {
    if (!r.used || r.msg != msg || n >= cap) continue;
    if (keys) memcpy(keys[n], r.key4, 4);
    if (ms) ms[n] = r.ms;
    n++;
  }
  return n;
}

void Core::onPosition(const TeamWire::Position& x) {
  // What this teammate heard (MESSAGE_REACH.md).
  const uint32_t at = io_->nowMs();
  for (uint8_t k = 0; CAP_TAK_EXTRAS && k < x.heard_n; k++) {
    HeardRec* slot = nullptr;
    for (HeardRec& r : heard_by_)
      if (r.used && r.msg == x.heard[k] && keyEq(r.key4, x.key4)) { slot = &r; break; }
    if (!slot)
      for (HeardRec& r : heard_by_)
        if (!r.used) { slot = &r; break; }
    if (!slot) {   // full: the oldest goes
      slot = &heard_by_[0];
      for (HeardRec& r : heard_by_)
        if ((int32_t)(r.ms - slot->ms) < 0) slot = &r;
    }
    if (slot->used && slot->msg == x.heard[k] && keyEq(slot->key4, x.key4)) continue;   // known: keep its time
    slot->used = true;
    slot->msg = x.heard[k];
    memcpy(slot->key4, x.key4, 4);
    slot->ms = at;
  }
  int slot = -1;
  for (int i = 0; i < kMaxMembers && slot < 0; i++)
    if (members_[i].used && keyEq(members_[i].pos.key4, x.key4)) slot = i;
  for (int i = 0; i < kMaxMembers && slot < 0; i++)
    if (!members_[i].used) slot = i;
  if (slot < 0) {   // full: the one heard longest ago makes room
    slot = 0;
    for (int i = 1; i < kMaxMembers; i++)
      if ((int32_t)(members_[i].heard_ms - members_[slot].heard_ms) < 0) slot = i;
  }
  // "Here, no position": listed and heard now; a position heard from them
  // before stays (their last known).
  const bool keep = !x.has_pos && members_[slot].used && keyEq(members_[slot].pos.key4, x.key4) &&
                    members_[slot].pos.has_pos;
  const TeamWire::Position before = members_[slot].pos;
  members_[slot].used = true;
  members_[slot].pos = x;
  if (keep) {
    TeamWire::Position& p = members_[slot].pos;
    p.has_pos = true;
    p.lat = before.lat;
    p.lon = before.lon;
    p.has_alt = before.has_alt;
    p.alt_m = before.alt_m;
    p.has_motion = before.has_motion;
    p.speed_mps = before.speed_mps;
    p.heading_deg = before.heading_deg;
    p.accuracy_m = before.accuracy_m;
  }
  members_[slot].heard_ms = at;
  members_[slot].latest_has_position = x.has_pos;
  if (!keep) members_[slot].position_ms = x.has_pos ? at : 0;
  io_->changed();
}

Fresh Core::freshness(const Member& m) const {
  const uint32_t age = io_->nowMs() - m.heard_ms;
  return age < kLiveMs ? Fresh::Live : age < kStaleMs ? Fresh::Stale : Fresh::Lost;
}

int Core::memberCount() const {
  int n = 0;
  for (auto& m : members_) if (m.used) n++;
  return n;
}

bool Core::sendReply(uint8_t ref, const uint8_t to4[4], uint32_t ref_id, uint8_t code, uint8_t arg) {
  if (!active()) return false;
  TeamWire::Reply r;
  io_->selfKey4(r.key4);
  memcpy(r.to4, to4, 4);
  r.ref = ref;
  r.ref_id = ref_id;
  r.code = code;
  r.arg = arg;
  snprintf(r.callsign, sizeof r.callsign, "%s", callsign());
  uint8_t buf[TeamWire::kMaxPayload];
  const size_t n = TeamWire::encode(r, buf, sizeof buf);
  return n && sendRaw(buf, n);
}

void Core::onAlert(const TeamWire::Alert& a) {
  int slot = -1;
  for (int i = 0; i < kMaxAlerts; i++)
    if (alerts_[i].used && keyEq(alerts_[i].a.key4, a.key4) && alerts_[i].a.id == a.id) { slot = i; break; }
  const bool was_active = slot >= 0 && alerts_[slot].a.active;
  if (slot < 0) {
    for (int i = 0; i < kMaxAlerts; i++) if (!alerts_[i].used) { slot = i; break; }
    if (slot < 0) {   // full: reuse the oldest inactive, else the oldest
      int best = 0;
      for (int i = 0; i < kMaxAlerts; i++) {
        if (!alerts_[i].a.active && alerts_[best].a.active) best = i;
        else if (alerts_[i].a.active == alerts_[best].a.active &&
                 (int32_t)(alerts_[i].heard_ms - alerts_[best].heard_ms) < 0) best = i;
      }
      slot = best;
    }
    alerts_[slot] = AlertRec();
  }
  AlertRec& r = alerts_[slot];
  r.used = true;
  r.a = a;
  r.heard_ms = io_->nowMs();
  if (a.active && !was_active) {
    r.acked = false;
    io_->alarm(r);
  }
  io_->changed();
}

AlertRec* Core::firstUnacked() {
  AlertRec* best = nullptr;
  for (auto& r : alerts_)
    if (r.used && r.a.active && !r.acked && (!best || (int32_t)(r.heard_ms - best->heard_ms) > 0)) best = &r;
  return best;
}

bool Core::isMine(const TeamWire::Marker& m) {
  uint8_t me[4];
  io_->selfKey4(me);
  return keyEq(m.key4, me);
}

void Core::onMarker(const TeamWire::Marker& m) {
  int slot = -1, free_slot = -1, oldest = -1;
  for (int i = 0; i < kMaxMarkers; i++) {
    if (!mused_[i]) { if (free_slot < 0) free_slot = i; continue; }
    if (keyEq(markers_[i].key4, m.key4) && markers_[i].id == m.id) { slot = i; break; }
    if (oldest < 0 || markers_[i].deleted > markers_[oldest].deleted ||
        (markers_[i].deleted == markers_[oldest].deleted && markers_[i].time < markers_[oldest].time))
      oldest = i;
  }
  if (slot >= 0) {
    const TeamWire::Marker& cur = markers_[slot];
    if (m.time <= cur.time && cur.deleted == m.deleted) return;   // not newer
  } else {
    slot = free_slot >= 0 ? free_slot : oldest;
    if (slot < 0) return;
  }
  markers_[slot] = m;
  mused_[slot] = true;
  saveMarkers();
  io_->changed();
}

TeamWire::Marker* Core::putMarker(TeamWire::Marker* existing, uint8_t kind, double lat, double lon,
                                  const char* label, const char* note) {
  TeamWire::Marker m;
  if (existing) m = *existing;
  else {
    io_->selfKey4(m.key4);
    m.id = io_->random32();
  }
  m.deleted = false;
  m.kind = kind;
  m.lat = lat;
  m.lon = lon;
  uint32_t t = io_->epoch();
  if (existing && t <= existing->time) t = existing->time + 1;   // newer wins at the other end
  m.time = t;
  snprintf(m.label, sizeof m.label, "%s", label ? label : "");
  snprintf(m.note, sizeof m.note, "%s", note ? note : "");
  onMarker(m);
  uint8_t buf[TeamWire::kMaxPayload];
  sendRaw(buf, TeamWire::encode(m, buf, sizeof buf));
  compatMarker(m);
  for (int i = 0; i < kMaxMarkers; i++)
    if (mused_[i] && keyEq(markers_[i].key4, m.key4) && markers_[i].id == m.id) return &markers_[i];
  return nullptr;
}

void Core::deleteMarker(TeamWire::Marker& m) {
  TeamWire::Marker d = m;
  d.deleted = true;
  uint32_t t = io_->epoch();
  if (t <= m.time) t = m.time + 1;
  d.time = t;
  onMarker(d);
  uint8_t buf[TeamWire::kMaxPayload];
  sendRaw(buf, TeamWire::encode(d, buf, sizeof buf));
}

// ---- Bloodhound -------------------------------------------------------------------
void Core::houndMember(const uint8_t key4[4]) {
  hound_ = 1;
  memcpy(hound_key_, key4, 4);
  io_->changed();
}
void Core::houndPoint(double lat, double lon, const char* label) {
  hound_ = 2;
  hound_lat_ = lat;
  hound_lon_ = lon;
  snprintf(hound_label_, sizeof hound_label_, "%s", label ? label : "");
  io_->changed();
}
bool Core::hound(double* lat, double* lon, char* label, size_t cap) {
  if (hound_ == 3) {
    const int k = cue_.next < follow_.count() ? cue_.next : follow_.count() - 1;
    if (k < 0) return false;
    *lat = follow_.lat(k);
    *lon = follow_.lon(k);
    snprintf(label, cap, "%s", hound_label_);
    return true;
  }
  if (hound_ == 2) {
    *lat = hound_lat_;
    *lon = hound_lon_;
    snprintf(label, cap, "%s", hound_label_);
    return true;
  }
  if (hound_ == 1) {
    for (auto& m : members_) {
      if (!m.used || !keyEq(m.pos.key4, hound_key_)) continue;
      if (!m.pos.has_pos) return false;
      *lat = m.pos.lat;
      *lon = m.pos.lon;
      snprintf(label, cap, "%s", m.pos.callsign);
      return true;
    }
  }
  return false;
}

// ---- MeshCore TEAM interop (MESHCORE_TEAM.md) -----------------------------------
// TEAM users on the team channel show as members, markers and routes, the
// way our own traffic shows; once one is heard (for 12 hours) we also send
// TEAM's #TEL with our beacons, #CAP with our callsign, our pins as #WAY.

static constexpr uint32_t kCompatActiveMs = 12UL * 3600UL * 1000UL;

static uint32_t fnv32(const char* s) {
  uint32_t h = 0x811c9dc5u;
  for (; *s; s++) { h ^= (uint8_t)*s; h *= 0x01000193u; }
  return h;
}

bool Core::compatActive() const {
  return compat_heard_ && io_->nowMs() - compat_heard_ms_ < kCompatActiveMs;
}

int Core::compatPeerCount() const {
  int n = 0;
  for (const CompatPeer& p : peers_) if (p.used) n++;
  return n;
}

CompatPeer* Core::compatPeer(const char* name, bool create) {
  for (CompatPeer& p : peers_) if (p.used && strcmp(p.name, name) == 0) return &p;
  if (!create) return nullptr;
  CompatPeer* pick = &peers_[0];
  for (CompatPeer& p : peers_) {
    if (!p.used) { pick = &p; break; }
    if ((int32_t)(p.heard_ms - pick->heard_ms) < 0) pick = &p;
  }
  *pick = CompatPeer();
  pick->used = true;
  snprintf(pick->name, sizeof pick->name, "%s", name);
  compatKey(*pick);
  return pick;
}

// Its #CAP key, else a contact by its name, else a number made from the name
// (what the app does too, so both file it alike).
void Core::compatKey(CompatPeer& p) {
  if (p.has_key) return;
  if (io_->contactKey4(p.name, p.key4)) return;
  char b[48];
  snprintf(b, sizeof b, "TEAM:%s", p.name);
  const uint32_t h = fnv32(b);
  for (int i = 0; i < 4; i++) p.key4[i] = (uint8_t)(h >> (24 - 8 * i));
}

void Core::compatShow(CompatPeer& p) {
  if (!p.has_pos) return;
  TeamWire::Position x;
  memcpy(x.key4, p.key4, 4);
  x.lat = p.pos.lat;
  x.lon = p.pos.lon;
  x.battery_pct = teamcompat::batteryPct(p.pos.mv());
  x.from_radio = p.pos.autonomous;
  const char* cs = p.alias[0] ? p.alias : p.name;
  teamcompat::field(cs, cs + strlen(cs), x.callsign, sizeof x.callsign);   // cut on a character
  onPosition(x);
}

bool Core::onText(const char* channel, const char* sender, const char* body) {
  if (!body || !teamcompat::isCompatText(body)) return false;
  teamcompat::Pos pos;
  teamcompat::Cap cap;
  teamcompat::CapReq req;
  const bool is_pos = teamcompat::parsePos(body, pos);
  const bool is_cap = !is_pos && teamcompat::parseCap(body, cap);
  const bool is_req = !is_pos && !is_cap && teamcompat::parseCapReq(body, req);
  const bool is_way = teamcompat::startsWith(body, "#WAY:") || teamcompat::startsWith(body, "#WRC:");
  if (!is_pos && !is_cap && !is_req && !is_way) return false;   // not really TEAM's: chat
  if (!cfg.enabled || !channel || strcmp(channel, cfg.channel) != 0) return true;
  if (!sender || !sender[0] || strcmp(sender, io_->nodeName()) == 0) return true;
  const uint32_t now = io_->nowMs();
  if (!compatActive()) {   // just found them: introduce ourselves after a moment
    cap_due_ = true;
    cap_due_ms_ = now + 2000 + io_->random32() % 6000;
  }
  compat_heard_ = true;
  compat_heard_ms_ = now;
  if (is_req) {
    if (strcmp(req.name, io_->nodeName()) != 0) return true;
    if (advert_sent_ && now - advert_ms_ < 10UL * 60000UL) return true;
    advert_sent_ = true;
    advert_ms_ = now;
    io_->advert();
    compatCap(true);
    return true;
  }
  CompatPeer* p = compatPeer(sender, true);
  p->heard_ms = now;
  if (is_pos) {
    p->pos = pos;
    p->has_pos = true;
    compatShow(*p);
  } else if (is_cap) {
    if (cap.key[0]) {
      uint8_t k[4];
      for (int i = 0; i < 4; i++) {
        char h[3] = {cap.key[2 * i], cap.key[2 * i + 1], 0};
        k[i] = (uint8_t)strtoul(h, nullptr, 16);
      }
      if (memcmp(k, p->key4, 4) != 0) {   // filed under a made-up key until now: move it
        for (Member& m : members_)
          if (m.used && memcmp(m.pos.key4, p->key4, 4) == 0) m.used = false;
        memcpy(p->key4, k, 4);
      }
      p->has_key = true;
    }
    if (cap.has_alias) snprintf(p->alias, sizeof p->alias, "%s", cap.alias);
    compatShow(*p);
  } else {
    if (!asm_) asm_ = new teamcompat::Assembler();
    teamcompat::Way* w = new teamcompat::Way();
    if (asm_ && w && asm_->accept(sender, body, now, *w)) compatWay(*p, *w);
    delete w;
  }
  return true;
}

void Core::compatWay(CompatPeer& p, const teamcompat::Way& w) {
  char idb[64];
  snprintf(idb, sizeof idb, "%s", w.mesh_id[0] ? w.mesh_id : "");
  if (!idb[0]) snprintf(idb, sizeof idb, "name:%s", w.name);
  const uint32_t id = fnv32(idb);
  uint32_t t = io_->epoch();
  if (!t) t = 1;
  if (w.isRoute()) {
    // As our route drawing, in the team colour nearest theirs; long routes
    // thinned to fit one team payload.
    TeamWire::Shape* sh = new TeamWire::Shape();
    if (!sh) return;
    memcpy(sh->key4, p.key4, 4);
    sh->id = id;
    sh->kind = TeamWire::kShapeRoute;
    sh->time = t;
    teamcompat::field(w.name, w.name + strlen(w.name), sh->label, sizeof sh->label);
    int best = 7;   // purple
    long bestd = -1;
    if (w.has_color) {
      for (int c = 0; c < TeamWire::kColors; c++) {
        const uint32_t rgb = TeamWire::colorRgb(c);
        const long dr = (long)((rgb >> 16) & 0xFF) - (long)((w.color >> 16) & 0xFF);
        const long dg = (long)((rgb >> 8) & 0xFF) - (long)((w.color >> 8) & 0xFF);
        const long db = (long)(rgb & 0xFF) - (long)(w.color & 0xFF);
        const long d = dr * dr + dg * dg + db * db;
        if (bestd < 0 || d < bestd) { bestd = d; best = c; }
      }
    }
    sh->color = (uint8_t)best;
    const int n = w.n >= 2 ? w.n : 0;
    uint8_t buf[TeamWire::kMaxPayload];
    size_t len = 0;
    for (int step = 1; n && step <= n && !len; step++) {   // every point, then every 2nd...
      sh->n = 0;
      for (int i = 0; i < n && sh->n < TeamWire::kMaxShapePoints; i += step) {
        sh->lat5[sh->n] = (int32_t)lround(w.rlat[i] * 1e5);
        sh->lon5[sh->n] = (int32_t)lround(w.rlon[i] * 1e5);
        sh->n++;
      }
      if ((n - 1) % step && sh->n < TeamWire::kMaxShapePoints) {   // keep the end
        sh->lat5[sh->n] = (int32_t)lround(w.rlat[n - 1] * 1e5);
        sh->lon5[sh->n] = (int32_t)lround(w.rlon[n - 1] * 1e5);
        sh->n++;
      }
      len = TeamWire::encode(*sh, buf, sizeof buf);
    }
    delete sh;
    if (len) onShape(buf, len);
    return;
  }
  TeamWire::Marker m;
  memcpy(m.key4, p.key4, 4);
  m.id = id;
  m.kind = teamcompat::markerKindForType(w.type);
  m.lat = w.lat;
  m.lon = w.lon;
  for (int i = 0; i < kMaxMarkers; i++)   // a re-sent waypoint: newer than the copy we have
    if (mused_[i] && keyEq(markers_[i].key4, m.key4) && markers_[i].id == m.id && t <= markers_[i].time)
      t = markers_[i].time + 1;
  m.time = t;
  snprintf(m.label, sizeof m.label, "%s", w.name);
  snprintf(m.note, sizeof m.note, "%s", w.desc);
  onMarker(m);
}

bool Core::compatSend(const char* text) { return active() && io_->sendText(cfg.channel, text); }

void Core::compatCap(bool force) {
  if (!force && !compatActive()) return;
  // Our radio key's first 6 bytes, which TEAM files us under ("-" if unknown).
  char key12[13] = {0};
  uint8_t k[6];
  if (io_->selfKey6(k)) for (int i = 0; i < 6; i++) snprintf(key12 + 2 * i, 3, "%02x", k[i]);
  char text[96];
  if (!teamcompat::encodeCap(key12, callsign(), text, sizeof text)) return;
  if (compatSend(text)) {
    cap_sent_ = true;
    cap_ms_ = io_->nowMs();
    snprintf(cap_alias_, sizeof cap_alias_, "%s", callsign());
  }
}

void Core::compatAfterBeacon() {
  if (!compatActive() || !cfg.share) return;
  const uint32_t now = io_->nowMs();
  if (tel_sent_ && now - tel_ms_ < 15000UL) return;
  char text[40];
  if (!teamcompat::encodeTel(last_fix_.lat, last_fix_.lon, io_->batteryMv(), -1, text, sizeof text)) return;
  if (!compatSend(text)) return;
  tel_sent_ = true;
  tel_ms_ = now;
  if (!cap_sent_ || strcmp(cap_alias_, callsign()) != 0 || now - cap_ms_ > 6UL * 3600UL * 1000UL) {
    cap_due_ = true;   // a couple of seconds after the #TEL
    cap_due_ms_ = now + 2000;
  }
}

void Core::compatMarker(const TeamWire::Marker& m) {
  if (!compatActive() || m.deleted) return;
  char id[24];
  snprintf(id, sizeof id, "%02x%02x%02x%02x-%lx", m.key4[0], m.key4[1], m.key4[2], m.key4[3], (unsigned long)m.id);
  char text[160];
  const size_t overhead = strlen(io_->nodeName()) + 2;
  if (teamcompat::encodeWay(id, m.label[0] ? m.label : TeamWire::kindName(m.kind), m.lat, m.lon, m.note,
                            teamcompat::typeForMarkerKind(m.kind), overhead, text, sizeof text))
    compatSend(text);
}

}  // namespace team

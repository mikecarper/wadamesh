// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// MeshCore Open TAK team traffic, byte-compatible with the app's
// lib/team/team_wire.dart. Everything is GRP_DATA (data type 0x5654 'VT') on
// the team's private channel; every payload starts with a type byte and the
// first 4 bytes of the sender's public key. Integers are big-endian,
// coordinates degrees x 1e7.
//
//   position [0x50][key4][flags][lat i32][lon i32][alt i16][speed u8 (0.5 m/s)]
//            [heading u8 (360/256)][accuracy u8][battery u8|255][role<<4|colour]
//            [status][callsignLen][callsign]
//   alert    [0x53][key4][id u16][type<<4|flags][lat i32][lon i32][accuracy u8]
//            [callsignLen][callsign][noteLen][note]
//   marker   [0x4D][key4][id u32][op u8][kind u8][lat i32][lon i32][time u32]
//            [labelLen][label][noteLen][note][attachMode u8][sid4][packets u8][extra u16]
//   role     [0x4C][key4][roleId u8][op u8 (1 = back to default / removed)][time u32]
//            [icon u16 (app icon library id)][nameLen][name]
//   shape    [0x44][key4][id u32][op u8 (1 = deleted)][kind u8][colour u8][time u32]
//            [labelLen][label <= 24][geofence u8][param varint (route method; a
//            circle's radius)][n u8][first lat i32][first lon i32 (degrees x 1e5)]
//            [n-1 x (dlat, dlon) zigzag varints]   (app: team_shape_wire.dart)
//   reply    [0x51][key4][toKey4][refKind u8 (1 alert, 2 report, 3 pin)][refId u32]
//            [code u8][arg u8 (ETA minutes)][callsignLen][callsign]
//            (a quick reply, QuickReply.h; app: team_reply_wire.dart)
//
// Roles 0-9 are the defaults, 10-24 roles a team added. A position's role is the
// high nibble; 15 and up write 15 there, set flag 0x20 and put the role id in
// one byte after the callsign.
//
// Position flag 0x40 (who heard what, MESSAGE_REACH.md) adds, after that:
// [nameHash u16 (the radio name's Dart string hash)][n u8 (0-4)][heard u16 x n
// (reaction hashes of the last teammates' channel messages heard)].
//
// Position flag 0x80 adds, last, a beacon number [seq u16] that goes up with
// every beacon: GRP_DATA has no timestamp and channel encryption is
// deterministic, so without it a still member's beacons were the same bytes and
// every radio dropped the repeats as already seen (MeshCore keeps the last 160).
// An alert's flag 0x04 adds [nonce u8] after the note, for its repeats. Older
// readers ignore both (TEAM_PRESENCE.md).
//
// A member with no GPS fix and no location set still beacons, so they show in
// the team list: lat and lon are both 0x7FFFFFFF (kNoPosition, out of range),
// which builds before this drop as invalid. The app's team_wire.dart matches.
//
// Host-tested against golden bytes that the app's own test checks too
// (test/test_team_wire.cpp here, test/team/team_wire_golden_test.dart there).

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace TeamWire {

constexpr uint16_t kDataType = 0x5654;
constexpr uint8_t kPosition = 0x50, kAlert = 0x53, kMarker = 0x4D, kReport = 0x52, kRole = 0x4C;
constexpr uint8_t kShape = 0x44;
constexpr uint8_t kReply = 0x51;
// Shapes: line freehand polygon rectangle circle route (the app's ShapeKind).
constexpr int kShapeRoute = 5;
constexpr size_t kMaxShapeLabel = 24;
constexpr int kMaxShapePoints = 120;
constexpr int kRouteWalking = 0, kRouteDriving = 1;   // RouteMethod: walking driving flying swimming watercraft
constexpr size_t kMaxPayload = 163;
constexpr size_t kMaxCallsign = 20, kMaxLabel = 32, kMaxNote = 60, kMaxAlertNote = 80;

// Enum orders are the app's; only the index goes on the air.
constexpr int kRoles = 10;       // member lead medic navigator comms driver security observer command press
constexpr int kMaxRoleId = 24;   // 10-24: roles the team added
constexpr int kRoleEscape = 15;
constexpr size_t kMaxRoleName = 20;
constexpr int kColors = 14;      // cyan white yellow orange magenta red maroon purple darkBlue blue teal green darkGreen brown
constexpr int kStatuses = 6;     // none ok busy needHelp returning resting
constexpr int kMarkerKinds = 10; // point lastKnown staging clue hazard landingZone medical subjectFound water searched
constexpr int kAlertTypes = 5;   // sos inContact ringTheBell geofence custom

inline const char* roleName(int i) {
  static const char* const n[kRoles] = {"Team member", "Team lead", "Medic", "Navigator", "Comms",
                                        "Driver / vehicle", "Security", "Observer", "Command / HQ", "Press"};
  return (i >= 0 && i < kRoles) ? n[i] : "Role";
}
inline char roleLetter(int i) {
  static const char l[kRoles] = {'M', 'L', '+', 'N', 'C', 'D', 'S', 'O', 'H', 'P'};
  return (i >= 0 && i < kRoles) ? l[i] : 'M';
}
inline uint32_t colorRgb(int i) {
  static const uint32_t c[kColors] = {0x00BCD4, 0xFFFFFF, 0xFFEB3B, 0xFF9800, 0xE040FB, 0xF44336, 0x8B1A1A,
                                      0x7E57C2, 0x1A237E, 0x2196F3, 0x009688, 0x4CAF50, 0x1B5E20, 0x795548};
  return (i >= 0 && i < kColors) ? c[i] : c[0];
}
inline const char* colorName(int i) {
  static const char* const n[kColors] = {"Cyan", "White", "Yellow", "Orange", "Magenta", "Red", "Maroon",
                                         "Purple", "Dark blue", "Blue", "Teal", "Green", "Dark green", "Brown"};
  return (i >= 0 && i < kColors) ? n[i] : n[0];
}
inline const char* statusName(int i) {
  static const char* const n[kStatuses] = {"", "OK", "Busy", "Need help", "Returning", "Resting"};
  return (i >= 0 && i < kStatuses) ? n[i] : n[0];
}
inline const char* kindName(int i) {
  static const char* const n[kMarkerKinds] = {"Point", "Last known point", "Staging / command post", "Clue", "Hazard",
                                              "Landing zone", "Medical", "Subject found", "Water / resources",
                                              "Area searched"};
  return (i >= 0 && i < kMarkerKinds) ? n[i] : n[0];
}
inline uint32_t kindRgb(int i) {   // the colours the app gives TAK for each kind
  static const uint32_t c[kMarkerKinds] = {0xFFFFFF, 0xFF0000, 0x0000FF, 0xFFFF00, 0xFF7F00,
                                           0x00FF00, 0xFF00FF, 0x00FFFF, 0x3399FF, 0x888888};
  return (i >= 0 && i < kMarkerKinds) ? c[i] : c[0];
}
inline const char* alertName(int i) {
  static const char* const n[kAlertTypes] = {"SOS / 911", "In contact", "Ring the bell", "Geofence breached", "Alert"};
  return (i >= 0 && i < kAlertTypes) ? n[i] : n[0];
}

struct Position {
  uint8_t key4[4] = {0};
  double lat = 0, lon = 0;
  bool has_alt = false, has_motion = false;
  int16_t alt_m = 0;
  float speed_mps = 0, heading_deg = 0;
  uint8_t accuracy_m = 0;
  int battery_pct = -1;   // -1: unknown
  uint8_t role = 0, color = 0, status = 0;
  bool from_radio = false, sos = false, relayed = false;
  char callsign[kMaxCallsign + 1] = {0};
  bool has_name_hash = false;   // flag 0x40
  uint16_t name_hash = 0;
  uint8_t heard_n = 0;
  uint16_t heard[4] = {0};
  bool has_seq = false;   // flag 0x80: the beacon number
  uint16_t seq = 0;
  // False: "here, no position" (no fix, no location set). lat/lon mean nothing.
  bool has_pos = true;
};

struct RoleDef {
  uint8_t key4[4] = {0};
  uint8_t role_id = 0;
  bool reset = false;
  uint32_t time = 0;   // unix seconds
  uint16_t icon = 0;   // the app's icon library id; 0 = the role's own
  char name[kMaxRoleName + 1] = {0};
};
// The newer edit of a role wins; equal times go to the larger key.
inline bool beats(const RoleDef& a, const RoleDef& b) {
  if (a.time != b.time) return a.time > b.time;
  return memcmp(a.key4, b.key4, 4) > 0;
}

struct Alert {
  uint8_t key4[4] = {0};
  uint16_t id = 0;
  bool active = false;
  uint8_t type = 0;
  bool has_pos = false;
  double lat = 0, lon = 0;
  uint8_t accuracy_m = 0;
  char callsign[kMaxCallsign + 1] = {0};
  char note[kMaxAlertNote + 1] = {0};
  bool has_nonce = false;   // flag 0x04: a repeat's number
  uint8_t nonce = 0;
};

struct Marker {
  uint8_t key4[4] = {0};
  uint32_t id = 0;
  bool deleted = false;
  uint8_t kind = 0;
  double lat = 0, lon = 0;
  uint32_t time = 0;   // unix seconds
  char label[kMaxLabel + 1] = {0};
  char note[kMaxNote + 1] = {0};
  uint8_t att_mode = 0;   // VOX clip mode id; 0 = none
  uint8_t att_sid[4] = {0};
  uint8_t att_packets = 0;
  uint16_t att_extra = 0;
};

// A drawing, route or geofence. Points on a 1e-5 degree grid (about a metre).
struct Shape {
  uint8_t key4[4] = {0};
  uint32_t id = 0;
  bool deleted = false;
  uint8_t kind = 0, color = 0, geofence = 0;
  uint32_t time = 0;     // unix seconds
  uint32_t param = 0;    // route method, or a circle's radius in metres
  char label[kMaxShapeLabel + 1] = {0};
  int n = 0;
  int32_t lat5[kMaxShapePoints] = {0}, lon5[kMaxShapePoints] = {0};
  bool isRoute() const { return kind == kShapeRoute; }
};

// ---- bytes ------------------------------------------------------------------
struct W {
  uint8_t* p;
  size_t cap, n = 0;
  bool ok = true;
  W(uint8_t* p_, size_t cap_) : p(p_), cap(cap_) {}
  void u8(uint32_t v) { if (n < cap) p[n++] = (uint8_t)v; else ok = false; }
  void u16(uint32_t v) { u8(v >> 8); u8(v); }
  void u32(uint32_t v) { u16(v >> 16); u16(v); }
  void raw(const uint8_t* b, size_t k) { for (size_t i = 0; i < k; i++) u8(b[i]); }
  void varint(uint32_t v) { while (v >= 0x80) { u8((v & 0x7F) | 0x80); v >>= 7; } u8(v); }
  void svarint(int32_t v) { varint(v >= 0 ? (uint32_t)v << 1 : ((uint32_t)(-(int64_t)v) << 1) - 1); }
  // Trimmed, cut to `max` bytes on a UTF-8 character boundary.
  void str(const char* s, size_t max) {
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    size_t len = strlen(s);
    while (len && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\n' || s[len - 1] == '\r')) len--;
    if (len > max) {
      len = max;
      while (len && ((uint8_t)s[len] & 0xC0) == 0x80) len--;
    }
    u8((uint32_t)len);
    raw((const uint8_t*)s, len);
  }
};
struct R {
  const uint8_t* p;
  size_t n, o = 0;
  R(const uint8_t* p_, size_t n_) : p(p_), n(n_) {}
  bool has(size_t k) const { return o + k <= n; }
  uint32_t u8() { return o < n ? p[o++] : 0; }
  uint32_t u16() { const uint32_t a = u8(); return (a << 8) | u8(); }
  uint32_t u32() { const uint32_t a = u16(); return (a << 16) | u16(); }
  int32_t i32() { return (int32_t)u32(); }
  int16_t i16() { return (int16_t)u16(); }
  bool varint(uint32_t* v) {   // unsigned LEB128
    uint32_t x = 0;
    for (int shift = 0; shift <= 28; shift += 7) {
      if (!has(1)) return false;
      const uint32_t b = u8();
      x |= (b & 0x7F) << shift;
      if (b < 0x80) { *v = x; return true; }
    }
    return false;
  }
  bool svarint(int32_t* v) {   // zigzag
    uint32_t z;
    if (!varint(&z)) return false;
    *v = (z & 1) ? -(int32_t)((z + 1) >> 1) : (int32_t)(z >> 1);
    return true;
  }
  bool str(char* out, size_t cap) {
    if (!has(1)) return false;
    const size_t k = u8();
    if (!has(k)) return false;
    const size_t c = k < cap - 1 ? k : cap - 1;
    memcpy(out, p + o, c);
    out[c] = 0;
    o += k;
    return true;
  }
};

inline int32_t deg7(double v) { return (int32_t)llround(v * 1e7); }
// Latitude and longitude of a "no position" beacon: out of range on purpose.
constexpr uint32_t kNoPosition = 0x7FFFFFFFu;
inline double undeg7(int32_t v) { return v / 1e7; }
inline long clampl(long v, long lo, long hi) { return v < lo ? lo : v > hi ? hi : v; }

inline size_t encode(const Position& x, uint8_t* out, size_t cap) {
  W w(out, cap);
  w.u8(kPosition);
  w.raw(x.key4, 4);
  const uint32_t rid = x.role > kMaxRoleId ? 0 : x.role;
  const bool ext = rid >= (uint32_t)kRoleEscape;
  w.u8((x.has_alt ? 0x01 : 0) | (x.has_motion ? 0x02 : 0) | (x.from_radio ? 0x04 : 0) | (x.sos ? 0x08 : 0) |
       (x.relayed ? 0x10 : 0) | (ext ? 0x20 : 0) | (x.has_name_hash ? 0x40 : 0) | (x.has_seq ? 0x80 : 0));
  w.u32(x.has_pos ? (uint32_t)deg7(x.lat) : kNoPosition);
  w.u32(x.has_pos ? (uint32_t)deg7(x.lon) : kNoPosition);
  w.u16((uint16_t)(int16_t)(x.has_alt ? x.alt_m : 0));
  w.u8((uint32_t)clampl(x.has_motion ? lround(x.speed_mps * 2.0) : 0, 0, 255));
  double h = x.has_motion ? fmod((double)x.heading_deg, 360.0) : 0;
  if (h < 0) h += 360;
  w.u8((uint32_t)(lround(h * 256.0 / 360.0) & 0xFF));
  w.u8(x.accuracy_m);
  w.u8(x.battery_pct < 0 ? 255 : (uint32_t)clampl(x.battery_pct, 0, 100));
  w.u8(((ext ? (uint32_t)kRoleEscape : rid) << 4) | (x.color & 0x0F));
  w.u8(x.status);
  w.str(x.callsign, kMaxCallsign);
  if (ext) w.u8(rid);
  if (x.has_name_hash) {
    const uint8_t k = x.heard_n > 4 ? 4 : x.heard_n;
    w.u16(x.name_hash);
    w.u8(k);
    for (uint8_t i = 0; i < k; i++) w.u16(x.heard[i]);
  }
  if (x.has_seq) w.u16(x.seq);
  return w.ok ? w.n : 0;
}

inline bool parse(const uint8_t* p, size_t n, Position& x) {
  R r(p, n);
  // 23: every fixed field and an empty callsign (25 dropped short callsigns)
  if (!r.has(23) || r.u8() != kPosition) return false;
  memcpy(x.key4, p + 1, 4);
  r.o = 5;
  const uint32_t flags = r.u8();
  const int32_t raw_lat = r.i32(), raw_lon = r.i32();
  x.has_pos = !((uint32_t)raw_lat == kNoPosition && (uint32_t)raw_lon == kNoPosition);
  x.lat = x.has_pos ? undeg7(raw_lat) : 0;
  x.lon = x.has_pos ? undeg7(raw_lon) : 0;
  if (fabs(x.lat) > 90 || fabs(x.lon) > 180) return false;
  const int16_t alt = r.i16();
  const uint32_t spd = r.u8(), hdg = r.u8();
  x.accuracy_m = (uint8_t)r.u8();
  const uint32_t batt = r.u8(), rc = r.u8();
  x.status = (uint8_t)r.u8();
  if (x.status >= kStatuses) x.status = 0;
  if (!r.str(x.callsign, sizeof x.callsign)) x.callsign[0] = 0;
  x.has_alt = flags & 0x01;
  x.alt_m = x.has_alt ? alt : 0;
  x.has_motion = flags & 0x02;
  x.speed_mps = x.has_motion ? spd / 2.0f : 0;
  x.heading_deg = x.has_motion ? hdg * 360.0f / 256.0f : 0;
  x.battery_pct = batt == 255 ? -1 : (int)batt;
  x.role = (uint8_t)(rc >> 4);
  if (x.role == kRoleEscape) x.role = (flags & 0x20) && r.has(1) ? (uint8_t)r.u8() : 0;
  if (x.role > kMaxRoleId) x.role = 0;
  x.has_name_hash = false;
  x.heard_n = 0;
  if ((flags & 0x40) && r.has(3)) {
    x.has_name_hash = true;
    x.name_hash = (uint16_t)r.u16();
    uint32_t k = r.u8();
    if (k > 4) k = 4;
    for (uint32_t i = 0; i < k && r.has(2); i++) x.heard[x.heard_n++] = (uint16_t)r.u16();
  }
  x.has_seq = (flags & 0x80) && r.has(2);
  x.seq = x.has_seq ? (uint16_t)r.u16() : 0;
  x.color = (uint8_t)(rc & 0x0F);
  if (x.color >= kColors) x.color = 0;
  x.from_radio = flags & 0x04;
  x.sos = flags & 0x08;
  x.relayed = flags & 0x10;
  return true;
}

inline size_t encode(const Alert& x, uint8_t* out, size_t cap) {
  W w(out, cap);
  w.u8(kAlert);
  w.raw(x.key4, 4);
  w.u16(x.id);
  w.u8((x.active ? 0x01 : 0) | (x.has_pos ? 0x02 : 0) | (x.has_nonce ? 0x04 : 0) | ((uint32_t)x.type << 4));
  w.u32((uint32_t)(x.has_pos ? deg7(x.lat) : 0));
  w.u32((uint32_t)(x.has_pos ? deg7(x.lon) : 0));
  w.u8(x.accuracy_m);
  w.str(x.callsign, kMaxCallsign);
  w.str(x.note, kMaxAlertNote);
  if (x.has_nonce) w.u8(x.nonce);
  return w.ok ? w.n : 0;
}

inline bool parse(const uint8_t* p, size_t n, Alert& x) {
  R r(p, n);
  if (!r.has(18) || r.u8() != kAlert) return false;
  memcpy(x.key4, p + 1, 4);
  r.o = 5;
  x.id = (uint16_t)r.u16();
  const uint32_t st = r.u8();
  const double lat = undeg7(r.i32()), lon = undeg7(r.i32());
  x.accuracy_m = (uint8_t)r.u8();
  x.has_pos = (st & 0x02) && fabs(lat) <= 90 && fabs(lon) <= 180;
  x.lat = x.has_pos ? lat : 0;
  x.lon = x.has_pos ? lon : 0;
  x.active = st & 0x01;
  x.type = (uint8_t)(st >> 4);
  if (x.type >= kAlertTypes) x.type = 0;
  if (!r.str(x.callsign, sizeof x.callsign)) x.callsign[0] = 0;
  if (!r.str(x.note, sizeof x.note)) x.note[0] = 0;
  x.has_nonce = (st & 0x04) && r.has(1);
  x.nonce = x.has_nonce ? (uint8_t)r.u8() : 0;
  return true;
}

// A quick reply to a teammate's alert, report or pin.
struct Reply {
  uint8_t key4[4] = {0};      // who answers
  uint8_t to4[4] = {0};       // the item's author
  uint8_t ref = 0;            // 1 alert, 2 report, 3 pin
  uint32_t ref_id = 0;        // the alert id, report id or marker id
  uint8_t code = 0;           // quickreply::Code
  uint8_t arg = 0;            // ETA minutes
  char callsign[kMaxCallsign + 1] = {0};
};
constexpr uint8_t kRefAlert = 1, kRefReport = 2, kRefPin = 3;
constexpr uint8_t kReplyCodes = 7;   // copy onMyWay eta cantHelp seenIt needBackup allClear

inline size_t encode(const Reply& x, uint8_t* out, size_t cap) {
  W w(out, cap);
  w.u8(kReply);
  w.raw(x.key4, 4);
  w.raw(x.to4, 4);
  w.u8(x.ref);
  w.u32(x.ref_id);
  w.u8(x.code);
  w.u8(x.arg);
  w.str(x.callsign, kMaxCallsign);
  return w.ok ? w.n : 0;
}

inline bool parse(const uint8_t* p, size_t n, Reply& x) {
  R r(p, n);
  if (!r.has(16) || r.u8() != kReply) return false;
  memcpy(x.key4, p + 1, 4);
  memcpy(x.to4, p + 5, 4);
  r.o = 9;
  x.ref = (uint8_t)r.u8();
  x.ref_id = r.u32();
  x.code = (uint8_t)r.u8();
  x.arg = (uint8_t)r.u8();
  if (x.ref < kRefAlert || x.ref > kRefPin || x.code >= kReplyCodes) return false;
  if (!r.str(x.callsign, sizeof x.callsign)) x.callsign[0] = 0;
  return true;
}

inline size_t encode(const Marker& x, uint8_t* out, size_t cap) {
  W w(out, cap);
  w.u8(kMarker);
  w.raw(x.key4, 4);
  w.u32(x.id);
  w.u8(x.deleted ? 1 : 0);
  w.u8(x.kind);
  w.u32((uint32_t)deg7(x.lat));
  w.u32((uint32_t)deg7(x.lon));
  w.u32(x.time);
  w.str(x.label, kMaxLabel);
  w.str(x.note, kMaxNote);
  if (!x.att_mode) {
    w.u8(0);
  } else {
    w.u8(x.att_mode);
    w.raw(x.att_sid, 4);
    w.u8(x.att_packets);
    w.u16(x.att_extra);
  }
  return w.ok ? w.n : 0;
}

inline bool parse(const uint8_t* p, size_t n, Marker& x) {
  R r(p, n);
  if (!r.has(23) || r.u8() != kMarker) return false;
  memcpy(x.key4, p + 1, 4);
  r.o = 5;
  x.id = r.u32();
  x.deleted = r.u8() == 1;
  x.kind = (uint8_t)r.u8();
  if (x.kind >= kMarkerKinds) x.kind = 0;
  x.lat = undeg7(r.i32());
  x.lon = undeg7(r.i32());
  if (fabs(x.lat) > 90 || fabs(x.lon) > 180) return false;
  x.time = r.u32();
  if (!r.str(x.label, sizeof x.label) || !r.str(x.note, sizeof x.note)) return false;
  x.att_mode = 0;
  if (r.has(1)) {
    const uint8_t mode = (uint8_t)r.u8();
    if (mode && r.has(7)) {
      x.att_mode = mode;
      memcpy(x.att_sid, p + r.o, 4);
      r.o += 4;
      x.att_packets = (uint8_t)r.u8();
      x.att_extra = (uint16_t)r.u16();
    }
  }
  return true;
}

inline size_t encode(const RoleDef& x, uint8_t* out, size_t cap) {
  W w(out, cap);
  w.u8(kRole);
  w.raw(x.key4, 4);
  w.u8(x.role_id > kMaxRoleId ? 0 : x.role_id);
  w.u8(x.reset ? 1 : 0);
  w.u32(x.time);
  w.u16(x.icon);
  w.str(x.reset ? "" : x.name, kMaxRoleName);
  return w.ok ? w.n : 0;
}

inline bool parse(const uint8_t* p, size_t n, RoleDef& x) {
  R r(p, n);
  if (!r.has(14) || r.u8() != kRole) return false;
  memcpy(x.key4, p + 1, 4);
  r.o = 5;
  x.role_id = (uint8_t)r.u8();
  if (x.role_id > kMaxRoleId) return false;
  x.reset = r.u8() & 1;
  x.time = r.u32();
  x.icon = (uint16_t)r.u16();
  return r.str(x.name, sizeof x.name);
}

// ---- geometry (as the app) -----------------------------------------------------
inline size_t encode(const Shape& x, uint8_t* out, size_t cap) {
  W w(out, cap);
  w.u8(kShape);
  w.raw(x.key4, 4);
  w.u32(x.id);
  w.u8(x.deleted ? 1 : 0);
  w.u8(x.kind);
  w.u8(x.color);
  w.u32(x.time);
  w.str(x.label, kMaxShapeLabel);
  w.u8(x.geofence);
  w.varint(x.param);
  const int n = x.n < 0 ? 0 : x.n > kMaxShapePoints ? kMaxShapePoints : x.n;
  w.u8((uint32_t)n);
  for (int i = 0; i < n; i++) {
    if (i == 0) { w.u32((uint32_t)x.lat5[0]); w.u32((uint32_t)x.lon5[0]); }
    else { w.svarint(x.lat5[i] - x.lat5[i - 1]); w.svarint(x.lon5[i] - x.lon5[i - 1]); }
  }
  return w.ok ? w.n : 0;
}

inline bool parse(const uint8_t* p, size_t n, Shape& x) {
  R r(p, n);
  if (!r.has(17) || r.u8() != kShape) return false;
  memcpy(x.key4, p + 1, 4);
  r.o = 5;
  x.id = r.u32();
  const uint32_t op = r.u8();
  x.deleted = op == 1;
  x.kind = (uint8_t)r.u8();
  x.color = (uint8_t)r.u8();
  if (x.color >= kColors) x.color = 0;
  x.time = r.u32();
  if (!r.str(x.label, sizeof x.label) || !r.has(1)) return false;
  x.geofence = (uint8_t)r.u8();
  if (!r.varint(&x.param) || !r.has(1)) return false;
  const int k = (int)r.u8();
  x.n = 0;
  int32_t la = 0, lo = 0;
  for (int i = 0; i < k; i++) {
    if (i == 0) {
      if (!r.has(8)) return false;
      la = r.i32();
      lo = r.i32();
    } else {
      int32_t dl, dn;
      if (!r.svarint(&dl) || !r.svarint(&dn)) return false;
      la += dl;
      lo += dn;
    }
    if (la > 9000000 || la < -9000000 || lo > 18000000 || lo < -18000000) return false;
    if (x.n < kMaxShapePoints) { x.lat5[x.n] = la; x.lon5[x.n] = lo; x.n++; }
  }
  return x.n > 0 || x.deleted;
}

inline double distanceM(double lat1, double lon1, double lat2, double lon2) {
  const double rr = 6371000.0, k = M_PI / 180;
  const double p1 = lat1 * k, p2 = lat2 * k, dp = (lat2 - lat1) * k, dl = (lon2 - lon1) * k;
  const double a = sin(dp / 2) * sin(dp / 2) + cos(p1) * cos(p2) * sin(dl / 2) * sin(dl / 2);
  return 2 * rr * atan2(sqrt(a), sqrt(1 - a));
}
inline double bearingDeg(double lat1, double lon1, double lat2, double lon2) {
  const double k = M_PI / 180;
  const double p1 = lat1 * k, p2 = lat2 * k, dl = (lon2 - lon1) * k;
  const double y = sin(dl) * cos(p2), x = cos(p1) * sin(p2) - sin(p1) * cos(p2) * cos(dl);
  return fmod(atan2(y, x) / k + 360.0, 360.0);
}
// "1.2 km 045° NE"
inline void rangeBearing(double m, double b, char* out, size_t cap) {
  static const char* const names[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
  char d[16];
  if (m >= 10000) snprintf(d, sizeof d, "%.0f km", m / 1000);
  else if (m >= 1000) snprintf(d, sizeof d, "%.1f km", m / 1000);
  else snprintf(d, sizeof d, "%ld m", lround(m));
  const long bi = lround(b) % 360;
  snprintf(out, cap, "%s %03ld\xC2\xB0 %s", d, bi, names[(int)((bi + 22.5) / 45) % 8]);
}

// APRS-style smart beaconing, as TeamBeaconPolicy: often while moving, rarely
// when still, at once on a sharp turn. Times in seconds; `since_s` < 0 when
// nothing has been sent yet.
inline bool shouldBeacon(long since_s, double meters_since, double speed_mps, double heading_change) {
  if (since_s < 0) return true;
  if (since_s < 30) return false;
  const bool moving = speed_mps >= 0.8;
  if (moving && fabs(heading_change) >= 35) return true;
  if (meters_since >= 75 && since_s >= 45) return true;
  if (moving && since_s >= 90) return true;
  return since_s >= 600;
}

}  // namespace TeamWire

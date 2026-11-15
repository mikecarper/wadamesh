// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Team awareness on this radio, as a member of a MeshCore Open TAK team:
// teammates' positions, SOS alerts both ways, shared tactical pins and
// Bloodhound. The wire format is TeamWire.h; this is the state and the rules
// (beacon timing, alert repeats, pin merge), platform-neutral and host-tested.

#include <stddef.h>
#include <stdint.h>

#include "QuickReply.h"
#include "TeamCompat.h"
#include "TeamRoute.h"
#include "TeamWire.h"

namespace team {

constexpr int kMaxMembers = 32;
constexpr int kMaxAlerts = 16;
constexpr int kMaxMarkers = 64;
constexpr int kMaxRoutes = 8;

// < 25 min, < 1 h, older: a still teammate beacons every 10 minutes, so live
// covers two of those going missing (the app uses the same, TEAM_PRESENCE.md).
enum class Fresh : uint8_t { Live, Stale, Lost };
constexpr uint32_t kLiveMs = 25UL * 60000UL;
constexpr uint32_t kStaleMs = 60UL * 60000UL;

struct Member {
  bool used = false;
  TeamWire::Position pos;
  uint32_t heard_ms = 0;
};

struct AlertRec {
  bool used = false;
  TeamWire::Alert a;
  bool acked = false;   // dismissed here
  uint32_t heard_ms = 0;
};

// A route shared on the team channel (the app's road directions, or one drawn
// there), kept as it arrived and parsed when used.
struct RouteRec {
  bool used = false;
  bool deleted = false;   // kept, so a late copy doesn't bring it back
  uint8_t key4[4] = {0};
  uint32_t id = 0, time = 0;
  uint8_t len = 0;
  uint8_t raw[TeamWire::kMaxPayload] = {0};
};

struct Fix {
  double lat = 0, lon = 0;
  bool has_alt = false, has_motion = false;
  int alt_m = 0;
  float speed_mps = 0, heading_deg = 0;
  uint8_t accuracy_m = 0;
  bool gps = false;   // false: the position set by hand in Settings
};

struct Settings {
  bool enabled = false;
  char channel[33] = {0};          // the team channel, by name
  char callsign[TeamWire::kMaxCallsign + 1] = {0};   // "" = node name
  uint8_t role = 0, color = 0, status = 0;
  bool share = true;               // send my position
};

class Io {
 public:
  virtual ~Io() {}
  virtual uint32_t nowMs() = 0;
  virtual uint32_t epoch() = 0;   // 0 if unknown
  virtual uint32_t random32() = 0;
  virtual void selfKey4(uint8_t out[4]) = 0;
  virtual const char* nodeName() = 0;
  virtual bool fix(Fix& f) = 0;
  virtual int batteryPct() = 0;   // -1 unknown
  virtual bool channelOk(const char* name) = 0;   // configured on this radio
  virtual bool send(const char* channel, const uint8_t* data, size_t len) = 0;
  virtual bool readFile(const char* name, uint8_t* buf, size_t cap, size_t* len) = 0;
  virtual bool writeFile(const char* name, const uint8_t* data, size_t len) = 0;
  virtual void changed() = 0;                   // redraw map / roster
  virtual void alarm(const AlertRec& r) = 0;    // a teammate's alert just went active
  virtual void reply(const TeamWire::Reply& r) { (void)r; }   // a quick reply heard (QuickReply.h)
  // MeshCore TEAM interop (MESHCORE_TEAM.md): channel text straight to the
  // radio (not into our chat), the radio's battery, an advert, and the key of
  // a contact by name.
  virtual bool sendText(const char* channel, const char* text) { (void)channel; (void)text; return false; }
  virtual int batteryMv() { return -1; }
  virtual void advert() {}
  virtual bool contactKey4(const char* name, uint8_t out[4]) { (void)name; (void)out; return false; }
  virtual bool selfKey6(uint8_t out[6]) { (void)out; return false; }   // for TEAM's #CAP
};

// A MeshCore TEAM user heard on the team channel.
struct CompatPeer {
  bool used = false;
  char name[33] = {0};           // its radio name (what channel text carries)
  bool has_key = false;          // from its #CAP
  uint8_t key4[4] = {0};         // what its member is filed under now
  char alias[teamcompat::kMaxAlias * 2 + 1] = {0};
  bool has_pos = false;
  teamcompat::Pos pos;
  uint32_t heard_ms = 0;
};
constexpr int kMaxCompatPeers = 16;

class Core {
 public:
  explicit Core(Io* io) : io_(io) {}
  ~Core() { delete asm_; }
  Core(const Core&) = delete;
  Core& operator=(const Core&) = delete;
  void begin();
  Settings cfg;
  void saveSettings();
  bool active() { return cfg.enabled && cfg.channel[0] && io_->channelOk(cfg.channel); }
  const char* callsign() { return cfg.callsign[0] ? cfg.callsign : io_->nodeName(); }
  void identityChanged() { force_beacon_ = true; saveSettings(); }

  // incoming GRP_DATA on `channel`
  void onPayload(const char* channel, const uint8_t* p, size_t n);
  // Incoming channel text on `channel` from `sender` (the body, without
  // "Name: "): true when it's MeshCore TEAM's, which isn't shown as chat.
  bool onText(const char* channel, const char* sender, const char* body);
  // A TEAM user was heard on the team channel in the last 12 hours: we send
  // TEAM's formats too.
  bool compatActive() const;
  int compatPeerCount() const;
  void tick();
  bool sendPosition();

  // members
  Member* member(int i) { return (i >= 0 && i < kMaxMembers && members_[i].used) ? &members_[i] : nullptr; }
  Fresh freshness(const Member& m) const;
  int memberCount() const;

  // alerts
  AlertRec* alert(int i) { return (i >= 0 && i < kMaxAlerts && alerts_[i].used) ? &alerts_[i] : nullptr; }
  AlertRec* firstUnacked();
  void acknowledge(AlertRec& r) { r.acked = true; io_->changed(); }
  bool sosActive() const { return own_active_; }
  const TeamWire::Alert& ownAlert() const { return own_; }
  void raiseAlert(uint8_t type, const char* note);
  // A quick reply to a teammate's alert, report or pin (TeamWire::Reply).
  bool sendReply(uint8_t ref, const uint8_t to4[4], uint32_t ref_id, uint8_t code, uint8_t arg);
  // Who heard what (MESSAGE_REACH.md): every channel text heard; a teammate's
  // goes into the next beacon, sent 15-45 s later.
  void noteChannelText(const char* channel, const char* sender, uint32_t sent, const char* body);
  // Teammates whose beacons say they heard the message with this reaction hash.
  int heardBy(uint16_t msg, uint8_t (*keys)[4], uint32_t* ms, int cap) const;
  static uint16_t nameHash(const char* name) {
    quickreply::DartHash d;
    d.utf8(name ? name : "");
    return (uint16_t)(d.done() & 0xFFFF);
  }
  void cancelAlert();

  // pins
  TeamWire::Marker* marker(int i) { return (i >= 0 && i < kMaxMarkers && mused_[i] && !markers_[i].deleted) ? &markers_[i] : nullptr; }
  bool isMine(const TeamWire::Marker& m);
  TeamWire::Marker* putMarker(TeamWire::Marker* existing, uint8_t kind, double lat, double lon, const char* label,
                              const char* note);
  void deleteMarker(TeamWire::Marker& m);

  // Bloodhound: a teammate (follows them), a fixed point, or a shared route
  // checkpoint by checkpoint with turn cues (teamroute::Follower).
  void houndMember(const uint8_t key4[4]);
  void houndPoint(double lat, double lon, const char* label);
  void stopHound() { hound_ = 0; io_->changed(); }
  bool hound(double* lat, double* lon, char* label, size_t cap);

  // Routes shared by the team, newest first by index order of arrival.
  const RouteRec* route(int i) const {
    return (i >= 0 && i < kMaxRoutes && routes_[i].used && !routes_[i].deleted) ? &routes_[i] : nullptr;
  }
  bool routeShape(int i, TeamWire::Shape& out) const;
  bool followRoute(int i, bool reverse);
  // Back-track: follow a recorded track (oldest point first) back to its
  // start, with the same turn cues as a route. Thinned to fit.
  bool followTrack(const int32_t* lat5, const int32_t* lon5, int n, const char* label);
  bool following() const { return hound_ == 3; }
  int followedRoute() const { return hound_ == 3 ? follow_idx_ : -1; }
  const teamroute::Cue& cue() const { return cue_; }
  // After the position changed (tick does it too).
  void updateFollow();

  // Roles: the team's names for the defaults and its added roles (edited in
  // the app; the radio follows them).
  void roleName(int id, char* out, size_t cap) const;
  char roleLetter(int id) const;
  // The app icon-library id the team chose for role [id]; 0 = the role's own.
  uint16_t roleIcon(int id) const;
  bool roleAvailable(int id) const;
  int nextRole(int id) const;   // the next role to offer in Setup

  uint32_t lastBeaconMs() const { return last_beacon_ms_; }
  bool everBeaconed() const { return beaconed_; }

 private:
  Io* io_;
  Member members_[kMaxMembers];
  AlertRec alerts_[kMaxAlerts];
  TeamWire::Marker markers_[kMaxMarkers];
  bool mused_[kMaxMarkers] = {false};

  TeamWire::RoleDef roles_[TeamWire::kMaxRoleId + 1];
  bool rused_[TeamWire::kMaxRoleId + 1] = {false};

  bool force_beacon_ = false;
  bool beaconed_ = false;
  // Who heard what: the last teammates' messages heard (for our beacons) and
  // what teammates' beacons said they heard.
  uint16_t heard_recent_[4] = {0};
  uint8_t heard_recent_n_ = 0;
  uint32_t heard_beacon_due_ms_ = 0;     // 0: none due
  uint32_t heard_beacon_last_ms_ = 0;
  bool heard_beaconed_ = false;
  struct HeardRec { bool used; uint16_t msg; uint8_t key4[4]; uint32_t ms; };
  HeardRec heard_by_[48] = {};
  uint32_t last_beacon_ms_ = 0;
  Fix last_fix_;
  uint16_t seq_ = 0;               // the beacon number (random start, set in begin)
  bool seq_set_ = false;

  bool own_active_ = false;
  TeamWire::Alert own_;
  uint8_t own_sends_ = 3;          // of 3 (0, 15 s, 45 s)
  uint32_t own_next_ms_ = 0;
  uint8_t cancel_sends_ = 2;       // of 2 (0, 10 s); 2 = nothing to cancel
  TeamWire::Alert cancel_;
  uint32_t cancel_next_ms_ = 0;

  uint8_t hound_ = 0;              // 0 none, 1 member, 2 point
  uint8_t hound_key_[4] = {0};
  double hound_lat_ = 0, hound_lon_ = 0;
  char hound_label_[33] = {0};

  RouteRec routes_[kMaxRoutes];
  teamroute::Follower follow_;
  teamroute::Cue cue_;
  int follow_idx_ = -1;
  uint32_t follow_id_ = 0;
  uint8_t follow_key_[4] = {0};
  bool loading_routes_ = false;
  void onShape(const uint8_t* p, size_t n);
  void saveRoutes();
  void loadRoutes();

  bool sendRaw(const uint8_t* p, size_t n);

  // MeshCore TEAM interop
  CompatPeer peers_[kMaxCompatPeers];
  bool compat_heard_ = false;
  uint32_t compat_heard_ms_ = 0;
  uint32_t tel_ms_ = 0, cap_ms_ = 0, advert_ms_ = 0, cap_due_ms_ = 0;
  bool tel_sent_ = false, cap_sent_ = false, advert_sent_ = false, cap_due_ = false;
  char cap_alias_[TeamWire::kMaxCallsign + 1] = {0};
  teamcompat::Assembler* asm_ = nullptr;
  CompatPeer* compatPeer(const char* name, bool create);
  void compatKey(CompatPeer& p);
  void compatShow(CompatPeer& p);
  void compatWay(CompatPeer& p, const teamcompat::Way& w);
  bool compatSend(const char* text);
  void compatCap(bool force);
  void compatAfterBeacon();
  void compatMarker(const TeamWire::Marker& m);
  void onPosition(const TeamWire::Position& p);
  void onAlert(const TeamWire::Alert& a);
  void onMarker(const TeamWire::Marker& m);
  void onRoleDef(const TeamWire::RoleDef& d);
  void saveRoles();
  void loadRoles();
  const TeamWire::RoleDef* roleEdit(int id) const;
  void saveMarkers();
  void loadMarkers();
  void loadSettings();
};

}  // namespace team

// Called by MyMesh for GRP_DATA with TeamWire::kDataType (defined in the UI glue).
void teamOnChannelData(int channel_idx, const uint8_t* data, size_t len);
// Called by MyMesh for channel text: reports with a position on watched channels
// go on the map (WatchFeed.h; defined in the UI glue).
void teamOnChannelText(int channel_idx, const char* text, uint32_t sent_epoch);
// Called by MyMesh for channel text first: true when it's MeshCore TEAM's
// (MESHCORE_TEAM.md), which isn't stored or shown as chat.
bool teamCompatOnChannelText(int channel_idx, const char* text);

// SPDX-License-Identifier: GPL-3.0-or-later
// MeshCore TEAM interop: its channel-text formats (src/ui-touch/TeamCompat.h),
// against the golden strings MeshCore Open TAK's test/team/team_compat_test.dart
// checks too, and TeamCore hearing and speaking them on a simulated clock.
//   g++ -std=c++17 -Wall -Wextra -fsanitize=address,undefined -I src test/test_team_compat.cpp src/ui-touch/TeamCore.cpp -o /tmp/tc && /tmp/tc
#include <stdio.h>
#include <string.h>

#include <map>
#include <string>
#include <vector>

#include "ui-touch/TeamCompat.h"
#include "ui-touch/TeamCore.h"

static int g_fail = 0;
#define CHECK(c)                                                  \
  do {                                                            \
    if (!(c)) {                                                   \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);         \
      g_fail++;                                                   \
    }                                                             \
  } while (0)

using namespace teamcompat;

static void formats() {
  char b[200];
  // #TEL, as TEAM writes it (lat/lon truncated toward zero).
  CHECK(encodeTel(47.6062, -122.3321, 3900, -1, b, sizeof b) && !strcmp(b, "#TEL:HGAhMLcVmljBAQE"));
  CHECK(encodeTel(-33.8688, 151.2093, 4100, 3800, b, sizeof b) && !strcmp(b, "#TEL:69AIAFogtUjjsQE"));
  Pos p;
  CHECK(parsePos("#TEL:HGAhMLcVmljBAQE", p));
  CHECK(fabs(p.lat - 47.6062) < 1e-7 && fabs(p.lon + 122.3321) < 1e-7);
  CHECK(p.comp_mv == 3896 && p.phone_mv == -1 && !p.autonomous);
  CHECK(batteryPct(p.mv()) == 66);
  CHECK(parsePos("#TEL:HE/swLciIWCg/wE", p) && p.autonomous && p.phone_mv == -1 && p.comp_mv == 3698);
  CHECK(p.lat == 47.5 && p.lon == -122.25);
  CHECK(parsePos("#T:BkIsQAwR56DSAQoFAQ", p) && p.lat == 10.5 && p.lon == 20.25 && p.comp_mv == 3998);
  CHECK(!parsePos("#TEL:abc", p) && !parsePos("#TEL:!!!!", p) && !parsePos("hello", p));

  // #CAP
  CHECK(encodeCap("a1b2c3d4e5f6", "Bravo: 2", b, sizeof b) && !strcmp(b, "#CAP:2:00:a1b2c3d4e5f6:-:Bravo: 2"));
  Cap c;
  CHECK(parseCap(b, c) && !strcmp(c.key, "a1b2c3d4e5f6") && !c.app[0] && !strcmp(c.alias, "Bravo: 2"));
  CHECK(parseCap("#CAP:2:05:0123456789ab:00112233aabbccdd:Echo", c) && c.flags == 5 &&
        !strcmp(c.app, "00112233aabbccdd") && !strcmp(c.alias, "Echo"));
  CHECK(parseCap("#CAP:2:01:0123456789ab:Fox", c) && !strcmp(c.alias, "Fox") && !c.app[0]);
  CHECK(parseCap("#CAP:1:03", c) && !c.has_alias && c.flags == 3);
  std::string longAlias(40, 'x');
  CHECK(encodeCap("", longAlias.c_str(), b, sizeof b) && !strcmp(b, ("#CAP:2:00:-:-:" + std::string(24, 'x')).c_str()));
  CapReq r;
  CHECK(parseCapReq("#CAP:R:a1b2c3d4e5f6:Hill Top", r) && !strcmp(r.name, "Hill Top") && !strcmp(r.key, "a1b2c3d4e5f6"));
  CHECK(!parseCap("#CAP:R:-:Hill Top", c));

  // #WAY
  Way w;
  int part, parts;
  CHECK(parseWay("#WAY:abc-1|Camp 2|47.1|-122.2|By the creek|CAMP|", w, &part, &parts, nullptr, nullptr));
  CHECK(!strcmp(w.mesh_id, "abc-1") && !strcmp(w.name, "Camp 2") && w.type == kCamp && !strcmp(w.desc, "By the creek"));
  CHECK(part == 1 && parts == 1);
  CHECK(parseWay("#WAY:Spot|1.5|2.5|note|THING", w, &part, &parts, nullptr, nullptr) && !w.mesh_id[0] && w.type == kCustom);
  CHECK(parseWay("#WAY:r1|Loop|47.000000|-122.000000|@C:FFF44336|ROUTE|47.000000,-122.000000~47.001000,-122.002000", w,
                 &part, &parts, nullptr, nullptr));
  CHECK(w.has_color && w.color == 0xFFF44336u && !w.desc[0] && w.n == 2 && w.isRoute());
  // Our waypoint, as the app writes it.
  CHECK(encodeWay("", "A|B", 1, 2, "", kCustom, 0, b, sizeof b) && !strcmp(b, "#WAY:|A/B|1.000000|2.000000||CUSTOM|"));
  // A long note is shortened to fit with the radio name in front.
  std::string note(120, 'n');
  const size_t n = encodeWay("id", "Pin", 1, 2, note.c_str(), kCamp, 20, b, sizeof b);
  CHECK(n > 0 && n + 20 <= kMaxBytes);

  // A long route in parts (as TEAM and the app split it), any order.
  Assembler* a = new Assembler();
  const char* head = "#WAY:r2|Long|47.000000|-122.000000||ROUTE|47.000000,-122.000000~47.001000,-122.000000~|1/3";
  const char* p2 = "#WRC:r2|47.002000,-122.000000~47.003000,-122.000000~|2/3";
  const char* p3 = "#WRC:r2|47.004000,-122.000000|3/3";
  CHECK(!a->accept("Alpha", p3, 1000, w));
  CHECK(!a->accept("Alpha", head, 1100, w));
  CHECK(a->accept("Alpha", p2, 1200, w));
  CHECK(w.n == 5 && fabs(w.rlat[4] - 47.004) < 1e-9 && !strcmp(w.name, "Long"));
  delete a;

  // Mapping (the app's markerKindForTeam / teamTypeForMarker).
  CHECK(markerKindForType(kCamp) == 2 && markerKindForType(kDanger) == 4 && markerKindForType(kGame) == 0);
  for (int t : {kCamp, kMeetup, kDanger, kWater}) CHECK(typeForMarkerKind(markerKindForType(t)) == t);
  CHECK(isCompatText("#WRC:x|y|2/2") && !isCompatText("#trail meets at 9"));
}

// ---- TeamCore ------------------------------------------------------------------------
struct TestIo : team::Io {
  uint32_t now = 1000000;
  std::vector<std::vector<uint8_t>> sent;
  std::vector<std::string> texts;
  std::map<std::string, std::vector<uint8_t>> files;
  team::Fix f;
  uint32_t rnd = 7;
  int adverts = 0;
  uint32_t nowMs() override { return now; }
  uint32_t epoch() override { return 1760000000u + now / 1000; }
  uint32_t random32() override { rnd = rnd * 1103515245u + 12345u; return rnd; }
  void selfKey4(uint8_t out[4]) override { out[0] = 0xC0; out[1] = 0xFF; out[2] = 0xEE; out[3] = 0x00; }
  bool selfKey6(uint8_t out[6]) override { selfKey4(out); out[4] = 0x12; out[5] = 0x34; return true; }
  const char* nodeName() override { return "TDeck"; }
  bool has_fix = true;
  bool fix(team::Fix& out) override { out = f; return has_fix; }
  int batteryPct() override { return 64; }
  int batteryMv() override { return 3900; }
  bool channelOk(const char* name) override { return !strcmp(name, "ops"); }
  bool send(const char*, const uint8_t* d, size_t n) override { sent.emplace_back(d, d + n); return true; }
  bool sendText(const char*, const char* t) override { texts.emplace_back(t); return true; }
  void advert() override { adverts++; }
  bool contactKey4(const char* name, uint8_t out[4]) override {
    if (strcmp(name, "Known")) return false;
    out[0] = 1; out[1] = 2; out[2] = 3; out[3] = 4;
    return true;
  }
  bool readFile(const char* name, uint8_t* buf, size_t cap, size_t* len) override {
    auto it = files.find(name);
    if (it == files.end() || it->second.size() > cap) return false;
    memcpy(buf, it->second.data(), it->second.size());
    *len = it->second.size();
    return true;
  }
  bool writeFile(const char* name, const uint8_t* d, size_t n) override { files[name] = std::vector<uint8_t>(d, d + n); return true; }
  void changed() override {}
  void alarm(const team::AlertRec&) override {}
  bool sentText(const char* prefix) const {
    for (auto& t : texts) if (t.rfind(prefix, 0) == 0) return true;
    return false;
  }
};

static const team::Member* memberCalled(team::Core& c, const char* cs) {
  for (int i = 0; i < team::kMaxMembers; i++) {
    const team::Member* m = c.member(i);
    if (m && !strcmp(m->pos.callsign, cs)) return m;
  }
  return nullptr;
}

static void core() {
  TestIo io;
  team::Core c(&io);
  c.begin();
  io.f.lat = 47.6; io.f.lon = -122.3; io.f.gps = true;
  c.cfg.enabled = true;
  snprintf(c.cfg.channel, sizeof c.cfg.channel, "ops");
  snprintf(c.cfg.callsign, sizeof c.cfg.callsign, "Hawk");

  // Before any TEAM user: our beacon goes out as ours only.
  CHECK(c.sendPosition());
  CHECK(io.texts.empty() && !c.compatActive());

  // TEAM's text: swallowed on any channel; only the team channel's is used.
  CHECK(c.onText("chat", "Alpha", "#TEL:HGAhMLcVmljBAQE"));
  CHECK(!c.compatActive() && c.memberCount() == 0);
  CHECK(!c.onText("ops", "Alpha", "#TELEPHONE today?"));   // not really TEAM's: chat
  CHECK(!c.onText("ops", "Alpha", "hello"));

  CHECK(c.onText("ops", "Alpha", "#TEL:HGAhMLcVmljBAQE"));
  CHECK(c.compatActive() && c.memberCount() == 1);
  const team::Member* m = memberCalled(c, "Alpha");
  CHECK(m && m->pos.battery_pct == 66 && fabs(m->pos.lat - 47.6062) < 1e-6);

  // Its #CAP: the alias becomes the callsign, under its real key now.
  CHECK(c.onText("ops", "Alpha", "#CAP:2:00:0a0b0c0d0e0f:-:Alpha Lead"));
  CHECK(c.memberCount() == 1);
  m = memberCalled(c, "Alpha Lead");
  CHECK(m && m->pos.key4[0] == 0x0a && m->pos.key4[3] == 0x0d);

  // A contact we know by name is filed under its key straight away.
  CHECK(c.onText("ops", "Known", "#TEL:HE/swLciIWCg/wE"));
  m = memberCalled(c, "Known");
  CHECK(m && m->pos.key4[0] == 1 && m->pos.from_radio);

  // Its waypoint: our marker kind, label and note.
  CHECK(c.onText("ops", "Alpha", "#WAY:abc-1|Camp 2|47.1|-122.2|By the creek|CAMP|"));
  const TeamWire::Marker* mk = nullptr;
  for (int i = 0; i < team::kMaxMarkers && !mk; i++) mk = c.marker(i);
  CHECK(mk && mk->kind == 2 && !strcmp(mk->label, "Camp 2") && !strcmp(mk->note, "By the creek") && mk->key4[0] == 0x0a);

  // Its route: our route drawing.
  CHECK(c.onText("ops", "Alpha",
                 "#WAY:r1|Loop|47.000000|-122.000000|@C:FFF44336|ROUTE|47.000000,-122.000000~47.001000,-122.002000"));
  TeamWire::Shape sh;
  CHECK(c.route(0) && c.routeShape(0, sh) && sh.n == 2 && sh.color == 5 && !strcmp(sh.label, "Loop"));

  // Found them: our #CAP a few seconds later, then #TEL with each beacon.
  io.now += 9000;
  c.tick();
  CHECK(io.sentText("#CAP:2:00:c0ffee001234:-:Hawk"));
  io.texts.clear();
  io.now += 20000;
  CHECK(c.sendPosition());
  CHECK(io.sentText("#TEL:"));
  io.texts.clear();
  io.now += 5000;
  CHECK(c.sendPosition());
  CHECK(!io.sentText("#TEL:"));   // at most every 15 s

  // Our pins go out as TEAM waypoints too.
  c.putMarker(nullptr, 4, 47.2, -122.4, "Rockfall", "left lane");
  bool way = false;
  for (auto& t : io.texts)
    if (t.rfind("#WAY:c0ffee00-", 0) == 0 && t.find("|Rockfall|47.200000|-122.400000|left lane|DANGER|") != std::string::npos)
      way = true;
  CHECK(way);

  // An advert when asked for one, at most every 10 minutes.
  CHECK(c.onText("ops", "Alpha", "#CAP:R:-:TDeck"));
  CHECK(io.adverts == 1);
  CHECK(c.onText("ops", "Alpha", "#CAP:R:-:TDeck"));
  CHECK(io.adverts == 1);
  CHECK(c.onText("ops", "Alpha", "#CAP:R:-:Someone else"));
  CHECK(io.adverts == 1);

  // Our own text, heard back: ignored.
  const int before = c.memberCount();
  CHECK(c.onText("ops", "TDeck", "#TEL:HGAhMLcVmljBAQE"));
  CHECK(c.memberCount() == before);

  // 12 hours on, nothing heard: we stop speaking TEAM.
  io.now += 12u * 3600u * 1000u + 1000;
  CHECK(!c.compatActive());
  io.texts.clear();
  CHECK(c.sendPosition());
  CHECK(io.texts.empty());
}

// A standalone with no GPS fix and no location set still beacons, and a
// teammate lists it (TeamWire.h "no position").
static void noPosition() {
  TestIo a, b;
  a.has_fix = false;
  team::Core ca(&a), cb(&b);
  ca.begin();
  cb.begin();
  for (team::Core* c : {&ca, &cb}) {
    c->cfg.enabled = true;
    snprintf(c->cfg.channel, sizeof c->cfg.channel, "ops");
  }
  snprintf(ca.cfg.callsign, sizeof ca.cfg.callsign, "M9");
  ca.tick();
  CHECK(a.sent.size() == 1);
  if (a.sent.empty()) return;
  TeamWire::Position x;
  CHECK(TeamWire::parse(a.sent[0].data(), a.sent[0].size(), x) && !x.has_pos && !strcmp(x.callsign, "M9"));
  b.selfKey4(x.key4);   // b's fake key is fixed; give the beacon another sender
  x.key4[3] = 0x55;
  uint8_t buf[TeamWire::kMaxPayload];
  size_t n = TeamWire::encode(x, buf, sizeof buf);
  cb.onPayload("ops", buf, n);
  const team::Member* m = memberCalled(cb, "M9");
  CHECK(m && !m->pos.has_pos);
  // Not again until it's due (as a still member)...
  ca.tick();
  CHECK(a.sent.size() == 1);
  a.now += 601000;
  ca.tick();
  CHECK(a.sent.size() == 2);
  // ...and a position heard before stays when the fix goes.
  x.has_pos = true; x.lat = 47.5; x.lon = -122.2; x.has_seq = true; x.seq = 9;
  n = TeamWire::encode(x, buf, sizeof buf);
  cb.onPayload("ops", buf, n);
  x.has_pos = false; x.seq = 10; x.battery_pct = 12;
  n = TeamWire::encode(x, buf, sizeof buf);
  cb.onPayload("ops", buf, n);
  m = memberCalled(cb, "M9");
  CHECK(m && m->pos.has_pos && fabs(m->pos.lat - 47.5) < 1e-6 && m->pos.battery_pct == 12);
}

int main() {
  formats();
  core();
  noPosition();
  if (g_fail) {
    printf("%d failure(s)\n", g_fail);
    return 1;
  }
  printf("team compat: all passed\n");
  return 0;
}

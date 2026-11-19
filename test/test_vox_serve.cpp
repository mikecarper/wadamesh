// SPDX-License-Identifier: GPL-3.0-or-later
//
// VoxServe (answering fetches for clips recorded on this radio) against a
// simulated mesh. The requests are the bytes MeshCore Open TAK sends
// (lib/vox/voice_wire.dart: VoxVoiceFetchRequest.encode and
// VoxVoiceTextRequest.encode), written out by hand so a change on either side
// shows up here, and the answers are reassembled with VoxWire::Assembly the
// way any listener does.
//
// Build: g++ -std=c++17 -Wall -Wextra -I src test/test_vox_serve.cpp src/ui-touch/VoxServe.cpp

#include <algorithm>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <string>
#include <initializer_list>
#include <vector>

#include "ui-touch/VoxFec.h"
#include "ui-touch/VoxServe.h"
#include "ui-touch/VoxFountain.h"

static int g_fail = 0;
#define CHECK(cond, ...)                                    \
  do {                                                      \
    if (!(cond)) {                                          \
      printf("FAIL %s:%d: ", __FILE__, __LINE__);           \
      printf(__VA_ARGS__);                                  \
      printf("\n");                                         \
      g_fail++;                                             \
    }                                                       \
  } while (0)

namespace {

struct Sent {
  uint32_t at;
  int channel;                 // -1 raw
  std::vector<uint8_t> path;   // raw only
  std::vector<uint8_t> data;
};

struct Sim : VoxServeIo {
  uint32_t now = 10000;
  uint32_t air = 500;
  uint32_t rng = 1;
  uint8_t self6[6] = {0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE};
  std::map<std::string, std::vector<uint8_t>> routes;   // key6 hex -> route
  std::map<std::string, uint32_t> route_from;           // route appears at this time
  std::map<uint32_t, std::pair<uint8_t, std::vector<uint8_t>>> clips;
  std::vector<Sent> sent;     // data packets
  std::vector<Sent> parity;   // parity packets (VoxFec.h): another session id
  int fail_sends = 0;
  // Parity for a clip we hold goes in `parity`, everything else in `sent`.
  bool isParity(const uint8_t* data, size_t n) const {
    VoxWire::Packet p;
    return VoxWire::parsePacket(data, n, &p) && clips.count(VoxFec::paritySid(p.sid));
  }
  int allocs = 0;
  bool verbose = false;

  static std::string hex(const uint8_t* k, int n) {
    std::string s;
    char b[3];
    for (int i = 0; i < n; i++) { snprintf(b, 3, "%02x", k[i]); s += b; }
    return s;
  }

  uint32_t nowMs() override { return now; }
  uint32_t random32() override { rng = rng * 1103515245u + 12345u; return rng >> 8; }
  void selfKey6(uint8_t out[6]) override { memcpy(out, self6, 6); }
  bool routeToKey6(const uint8_t key6[6], uint8_t* path, int* len) override {
    auto it = routes.find(hex(key6, 6));
    if (it == routes.end()) return false;
    auto f = route_from.find(it->first);
    if (f != route_from.end() && now < f->second) return false;
    memcpy(path, it->second.data(), it->second.size());
    *len = (int)it->second.size();
    return true;
  }
  bool sendRaw(const uint8_t* path, int len, const uint8_t* data, size_t n) override {
    if (fail_sends > 0) { fail_sends--; return false; }
    (isParity(data, n) ? parity : sent)
        .push_back(Sent{now, -1, std::vector<uint8_t>(path, path + len), std::vector<uint8_t>(data, data + n)});
    return true;
  }
  int zero_hop_sends = 0;
  bool sendChannelData(int ch, uint16_t type, const uint8_t* data, size_t n, bool zero_hop) override {
    if (type != VoxWire::kChannelDataType) return false;
    if (isParity(data, n)) {
      parity.push_back(Sent{now, ch, {}, std::vector<uint8_t>(data, data + n)});
      return true;
    }
    if (zero_hop) zero_hop_sends++;
    sent.push_back(Sent{now, ch, {}, std::vector<uint8_t>(data, data + n)});
    return true;
  }
  uint32_t packetAirMs() override { return air; }
  bool loadClip(uint32_t sid, uint8_t* mode, uint8_t** bits, size_t* len) override {
    auto it = clips.find(sid);
    if (it == clips.end()) return false;
    *mode = it->second.first;
    *len = it->second.second.size();
    *bits = (uint8_t*)malloc(*len);
    memcpy(*bits, it->second.second.data(), *len);
    allocs++;
    return true;
  }
  void freeClip(uint8_t* bits) override { free(bits); allocs--; }
  void log(const char* line) override { if (verbose) printf("  %6u %s\n", now, line); }

  void run(VoxServe& s, uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += 10) { s.tick(); now += 10; }
  }
};

const uint8_t kKeyA[6] = {0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6};
const uint8_t kKeyB[6] = {0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6};
const uint32_t kSid = 0x0A0B0C0D;

// A 10 s clip at 1200: 1500 bytes = 9 full packets of 156 and one of 96.
std::vector<uint8_t> clip1200(size_t len = 1500) {
  std::vector<uint8_t> b(len);
  for (size_t i = 0; i < len; i++) b[i] = (uint8_t)(i * 7 + 3);
  return b;
}

// Dart: VoxVoiceFetchRequest(sid, key, missing, saltOk, requesterPath).encode()
std::vector<uint8_t> dartFetch(uint32_t sid, const uint8_t key[6], const std::vector<int>* missing, bool salt_ok,
                               const std::vector<uint8_t>* path) {
  std::vector<uint8_t> b = {0x72, (uint8_t)(sid >> 24), (uint8_t)(sid >> 16), (uint8_t)(sid >> 8), (uint8_t)sid};
  b.push_back((uint8_t)((missing ? 0x01 : 0) | (salt_ok ? 0x02 : 0) | (path ? 0x04 : 0)));
  b.insert(b.end(), key, key + 6);
  b.push_back(missing ? (uint8_t)missing->size() : 0);
  if (missing) for (int v : *missing) b.push_back((uint8_t)v);
  if (path) { b.push_back((uint8_t)path->size()); b.insert(b.end(), path->begin(), path->end()); }
  return b;
}

// Reassembles everything sent for `sid` and returns it if complete.
std::vector<uint8_t> reassemble(const Sim& sim, uint32_t sid, int total, size_t from = 0) {
  std::vector<uint8_t> store((size_t)total * 156);
  VoxWire::Assembly a;
  a.begin(sid, (uint8_t)total, store.data(), store.size(), 156);
  for (size_t i = from; i < sim.sent.size(); i++) {
    VoxWire::Packet p;
    if (VoxWire::parsePacket(sim.sent[i].data.data(), sim.sent[i].data.size(), &p)) a.add(p);
  }
  if (!a.complete()) return {};
  return std::vector<uint8_t>(a.payload(), a.payload() + a.payloadLen());
}

void testRawAlongRequestPath() {
  Sim sim;
  VoxServe s(sim);
  sim.clips[kSid] = {1, clip1200()};
  const std::vector<uint8_t> path = {0x11, 0x22, 0x33};   // requester -> us
  const auto req = dartFetch(kSid, kKeyA, nullptr, true, &path);
  s.onPayload(req.data(), req.size(), -1);
  sim.run(s, 60000);
  CHECK(sim.sent.size() == 10, "sent %zu packets, want 10", sim.sent.size());
  bool rev = true, salted = true;
  for (auto& p : sim.sent) {
    rev = rev && p.channel == -1 && p.path == std::vector<uint8_t>({0x33, 0x22, 0x11});
    salted = salted && p.data[0] == 0x57;
  }
  CHECK(rev, "reply must go along the reverse of the request's path");
  CHECK(salted, "requester said salt OK, so packets are salted");
  CHECK(reassemble(sim, kSid, 10) == clip1200(), "reassembled clip differs");
  // Pacing: 3 hops -> air + 2 * 2.5 * 1.15 * air + 400 = 500 + 2875 + 400.
  const uint32_t gap = sim.sent[1].at - sim.sent[0].at;
  CHECK(gap >= 3770 && gap <= 3790, "gap %u ms", gap);
  CHECK(sim.allocs == 0, "clip buffer leaked (%d)", sim.allocs);
}

void testRawUsesContactRoute() {
  Sim sim;
  VoxServe s(sim);
  sim.clips[kSid] = {1, clip1200(300)};   // 2 packets
  sim.routes[Sim::hex(kKeyA, 6)] = {0x44};
  const auto req = dartFetch(kSid, kKeyA, nullptr, false, nullptr);
  s.onPayload(req.data(), req.size(), -1);
  sim.run(s, 20000);
  CHECK(sim.sent.size() == 2, "sent %zu", sim.sent.size());
  CHECK(!sim.sent.empty() && sim.sent[0].path == std::vector<uint8_t>({0x44}), "should use the contact's route");
  CHECK(!sim.sent.empty() && sim.sent[0].data[0] == 0x56, "no salt unless asked");
  CHECK(sim.sent.size() == 2 && sim.sent[1].data.size() == 6 + 144, "last packet is the short remainder");
}

void testZeroHopRequestPath() {
  Sim sim;
  VoxServe s(sim);
  sim.clips[kSid] = {1, clip1200(156)};
  const std::vector<uint8_t> path;   // the requester is our neighbour
  const auto req = dartFetch(kSid, kKeyA, nullptr, true, &path);
  s.onPayload(req.data(), req.size(), -1);
  sim.run(s, 5000);
  CHECK(sim.sent.size() == 1 && sim.sent[0].path.empty(), "an empty path means send zero-hop");
}

void testNoRouteGivesUp() {
  Sim sim;
  VoxServe s(sim);
  sim.clips[kSid] = {1, clip1200()};
  const auto req = dartFetch(kSid, kKeyA, nullptr, true, nullptr);
  s.onPayload(req.data(), req.size(), -1);
  sim.run(s, 4000);
  CHECK(sim.sent.empty(), "no route: nothing to send");
  CHECK(!s.busy(), "gave up after 3 s");
  CHECK(sim.allocs == 0, "leak");
}

void testVr3WaitsForRoute() {
  Sim sim;
  VoxServe s(sim);
  sim.clips[kSid] = {1, clip1200()};
  // The flooded DM teaches both radios a route ~8 s later.
  sim.routes[Sim::hex(kKeyB, 6)] = {0x55, 0x66};
  sim.route_from[Sim::hex(kKeyB, 6)] = sim.now + 8000;
  // Dart: VoxVoiceTextRequest.encode(sid 0x0a0b0c0d -> base36 "2sbggd", missing [1,2,3,7])
  const char* vr3 = "VR3:2sbggd:m1-3.7:b1b2b3b4b5b6";
  VoxWire::FetchRequest chk;
  CHECK(VoxWire::parseTextFetch(vr3, &chk) && chk.sid == kSid, "VR3 parse");
  CHECK(s.onDirectText(vr3), "VR3 is a control message");
  CHECK(!s.onDirectText("hello"), "plain text isn't");
  sim.run(s, 40000);
  CHECK(sim.sent.size() == 4, "sent %zu, want the 4 missing", sim.sent.size());
  std::vector<int> idx;
  for (auto& p : sim.sent) idx.push_back(p.data[5]);
  CHECK(idx == std::vector<int>({1, 2, 3, 7}), "wrong packets");
  CHECK(!sim.sent.empty() && sim.sent[0].at >= 18000, "sent before the route existed");
  CHECK(!sim.sent.empty() && sim.sent[0].path == std::vector<uint8_t>({0x55, 0x66}), "route");
  CHECK(!sim.sent.empty() && sim.sent[0].data[0] == 0x56, "VR3 doesn't negotiate salt");
}

void testVr3TimesOut() {
  Sim sim;
  VoxServe s(sim);
  sim.clips[kSid] = {1, clip1200()};
  s.onDirectText("VR3:2sbggd:a:b1b2b3b4b5b6");
  sim.run(s, 26000);
  CHECK(sim.sent.empty() && !s.busy(), "gives up after 25 s without a route");
}

void testChannelOwnOnly() {
  Sim sim;
  VoxServe s(sim);
  sim.clips[kSid] = {1, clip1200(400)};   // 3 packets
  const std::vector<int> miss = {0, 2};
  const auto req = dartFetch(kSid, kKeyA, &miss, true, nullptr);
  s.onPayload(req.data(), req.size(), 3);
  sim.run(s, 20000);
  CHECK(sim.sent.empty(), "not our clip: leave channel requests to its author");

  s.markOwn(kSid);
  const uint32_t t0 = sim.now;
  // A fresh request (a different listener, so not a repeat).
  const auto req2 = dartFetch(kSid, kKeyB, &miss, true, nullptr);
  s.onPayload(req2.data(), req2.size(), 3, 1);   // came through one repeater
  sim.run(s, 20000);
  CHECK(sim.sent.size() == 2, "sent %zu", sim.sent.size());
  CHECK(!sim.sent.empty() && sim.sent[0].channel == 3, "answer on the request's channel");
  // 0.15-0.4 s gathering listeners' requests, plus one relay of the request
  // (500 + 2.5 * 500 * 1.15 = 1938 ms) for its flood to clear.
  CHECK(!sim.sent.empty() && sim.sent[0].at - t0 >= 150 + 1938 && sim.sent[0].at - t0 < 400 + 1938 + 20,
        "author answers 0.15-0.4 s plus a relay after the request (%u)", sim.sent.empty() ? 0u : sim.sent[0].at - t0);
  CHECK(sim.parity.empty(), "missing packets only: no parity");
  const uint32_t gap = sim.sent.size() == 2 ? sim.sent[1].at - sim.sent[0].at : 0;
  CHECK(gap >= 2150 && gap <= 2170, "flood gap %u (4 * air + 150)", gap);
  for (auto& p : sim.sent) CHECK(p.data.size() <= 163, "GRP_DATA limit");
}

// A listener 4 hops away (the stalled MediaMesh photo, 26 Oct): wait out the
// request's flood (two relays of it) and space the flooded answers for the
// next two repeaters to clear (FloodPace's fast gap).
void testChannelFarListener() {
  Sim sim;
  VoxServe s(sim);
  sim.clips[kSid] = {1, clip1200(400)};   // 3 packets
  s.markOwn(kSid);
  const std::vector<int> miss = {0, 2};
  const auto req = dartFetch(kSid, kKeyA, &miss, true, nullptr);
  const uint32_t t0 = sim.now;
  s.onPayload(req.data(), req.size(), 3, 4);
  sim.run(s, 30000);
  CHECK(sim.sent.size() == 2, "sent %zu", sim.sent.size());
  CHECK(!sim.sent.empty() && sim.sent[0].at - t0 >= 150 + 2 * 1938 && sim.sent[0].at - t0 < 400 + 2 * 1938 + 20,
        "waits out a 4-hop request's flood (%u)", sim.sent.empty() ? 0u : sim.sent[0].at - t0);
  const uint32_t gap = sim.sent.size() == 2 ? sim.sent[1].at - sim.sent[0].at : 0;
  CHECK(gap >= 2938 && gap < 2960, "4-hop flood gap %u (2 relays to clear)", gap);

  // MediaMesh's 250 kHz: 157 ms a packet.
  Sim fast;
  fast.air = 157;
  VoxServe f(fast);
  fast.clips[kSid] = {1, clip1200(400)};
  f.markOwn(kSid);
  f.onPayload(req.data(), req.size(), 3, 4);
  fast.run(f, 30000);
  const uint32_t g2 = fast.sent.size() == 2 ? fast.sent[1].at - fast.sent[0].at : 0;
  CHECK(g2 >= 922 && g2 < 940, "radio2 4-hop flood gap %u", g2);
}

// FloodPace on its own: the same cases as the app's vox_timing_test.dart.
void testFloodPace() {
  using namespace floodpace;
  CHECK(relayMs(157) == 608, "relay %u", relayMs(157));
  Pace p(157);
  CHECK(p.gapMs(0) == 778 && p.gapMs(1) == 778 && p.gapMs(20) == 922, "start %u %u", p.gapMs(1), p.gapMs(20));
  CHECK(p.settleMs(0) == 0 && p.settleMs(1) == 608 && p.settleMs(20) == 2 * 608, "settle %u", p.settleMs(20));
  auto ans = [](std::initializer_list<int> v) { Answer a; for (int i : v) a.add(i); return a; };
  auto bits = [](std::initializer_list<int> v) {
    std::vector<uint8_t> m(32, 0);
    for (int i : v) m[i >> 3] |= (uint8_t)(1u << (i & 7));
    return m;
  };
  Answer a = ans({1, 4, 5, 7, 8, 9});
  p.feedback(a, bits({1, 4, 5, 8, 9}).data());
  CHECK(p.gapMs(4) == 1167 && p.settleMs(4) == 3 * 608, "lossy %u %u", p.gapMs(4), p.settleMs(4));
  p.feedback(a, bits({1, 4, 5, 8, 9}).data());
  CHECK(p.gapMs(4) == 1167 && p.settleMs(4) == 3 * 608, "a second report on it moved the pace");
  p.feedback(a, bits({}).data());
  CHECK(p.gapMs(4) == 922 && p.gapMs(0) == 778 && p.settleMs(4) == 2 * 608, "clean report %u %u", p.gapMs(4), p.settleMs(4));
  p.feedback(a, bits({1, 4, 5, 7, 8, 9}).data());
  CHECK(p.gapMs(4) == 922, "slowed after a clean report");
  for (int k = 0; k < 6; k++) { Answer c = ans({0, 1, 2, 3}); p.feedback(c, bits({0, 2, 3}).data()); }
  CHECK(p.gapMs(4) == 1556 && p.settleMs(4) == 4 * 608, "congested %u %u", p.gapMs(4), p.settleMs(4));
  for (int k = 0; k < 6; k++) { Answer c = ans({0, 1, 2, 3}); p.feedback(c, bits({}).data()); }
  CHECK(p.gapMs(4) == 922 && p.settleMs(4) == 2 * 608, "cleared %u %u", p.gapMs(4), p.settleMs(4));
  Pace m(500);
  CHECK(m.gapMs(1) == 2150 && m.gapMs(4) == 2938 && m.settleMs(1) == 1938 && slowGapMs(500) == 4300,
        "main mesh %u %u %u", m.gapMs(1), m.gapMs(4), m.settleMs(1));
}

// Listeners' requests after an answer teach the author's pace.
void testChannelPaceLearns() {
  Sim sim;
  sim.air = 157;
  VoxServe s(sim);
  sim.clips[kSid] = {1, clip1200(1000)};   // several packets
  s.markOwn(kSid);
  const std::vector<int> miss = {0, 1, 2, 3, 4, 5};
  const auto req = dartFetch(kSid, kKeyA, &miss, true, nullptr);
  s.onPayload(req.data(), req.size(), 3, 4);
  sim.run(s, 30000);
  const size_t first = sim.sent.size();
  CHECK(first >= 2, "first answer %zu", first);
  const uint32_t g1 = sim.sent[1].at - sim.sent[0].at;
  // Everything but packet 1 lost (and so the first): slower, and a longer wait.
  const std::vector<int> still = {0, 2, 3, 4, 5};
  const auto req2 = dartFetch(kSid, kKeyB, &still, true, nullptr);
  const uint32_t t1 = sim.now;
  s.onPayload(req2.data(), req2.size(), 3, 4);
  sim.run(s, 40000);
  CHECK(sim.sent.size() > first + 1, "second answer");
  if (sim.sent.size() > first + 1) {
    const uint32_t g2 = sim.sent[first + 1].at - sim.sent[first].at;
    CHECK(g2 > g1 + 200, "slower after a lossy answer: %u then %u", g1, g2);
    CHECK(sim.sent[first].at - t1 >= 150 + 3 * 608, "waited a relay more (%u)", sim.sent[first].at - t1);
  }
}

// The end-of-answer marker: a channel request saying it reads marked packets
// (flag 0x08) gets 0x58 packets counting down to 0; one that doesn't, or any
// raw request, gets 0x57 as before (raw 0x58 would go out on the main mesh).
void testMarkedChannelAnswer() {
  Sim sim;
  VoxServe s(sim);
  sim.clips[kSid] = {1, clip1200(1000)};   // 7 packets
  s.markOwn(kSid);
  const uint8_t miss[4] = {1, 2, 4, 6};
  uint8_t req[64];
  const int n = VoxWire::encodeFetch(kSid, kKeyA, miss, 4, req, sizeof req, true, nullptr, 0, 3, true);
  CHECK(n > 0 && (req[5] & VoxWire::kFetchFlagMarkOk), "the request says it reads marked packets");
  VoxWire::FetchRequest fr;
  CHECK(VoxWire::parseFetch(req, (size_t)n, &fr) && fr.mark_ok, "parsed back");
  s.onPayload(req, (size_t)n, 3, 1);
  sim.run(s, 30000);
  CHECK(sim.sent.size() == 4, "sent %zu", sim.sent.size());
  int expect_left = 3;
  for (auto& p : sim.sent) {
    VoxWire::Packet pk;
    CHECK(VoxWire::parsePacket(p.data.data(), p.data.size(), &pk), "parses");
    CHECK(p.data[0] == VoxWire::kPacketMarkedMagic && pk.marked, "marked (0x%02x)", p.data[0]);
    CHECK(pk.left == expect_left, "left %d, want %d", pk.left, expect_left);
    CHECK(p.data.size() <= 163, "GRP_DATA limit");
    expect_left--;
  }
  CHECK(reassemble(sim, kSid, 7).empty(), "only the 4 asked for");   // (not the whole clip)

  // Without the flag: salted 0x57, as before.
  Sim old;
  VoxServe o(old);
  old.clips[kSid] = {1, clip1200(1000)};
  o.markOwn(kSid);
  const int n2 = VoxWire::encodeFetch(kSid, kKeyB, miss, 4, req, sizeof req, true, nullptr, 0, 3, false);
  o.onPayload(req, (size_t)n2, 3, 1);
  old.run(o, 30000);
  CHECK(!old.sent.empty(), "answered");
  for (auto& p : old.sent) CHECK(p.data[0] == VoxWire::kPacketSaltedMagic, "unmarked (0x%02x)", p.data[0]);

  // A raw request that sets the flag anyway: raw stays 0x57.
  Sim raw;
  VoxServe r(raw);
  raw.clips[kSid] = {1, clip1200(1000)};
  const std::vector<uint8_t> path = {0x11};
  const int n3 = VoxWire::encodeFetch(kSid, kKeyA, miss, 4, req, sizeof req, true, path.data(), 1, 3, true);
  r.onPayload(req, (size_t)n3, -1);
  raw.run(r, 30000);
  CHECK(!raw.sent.empty(), "answered raw");
  for (auto& p : raw.sent) CHECK(p.data[0] == VoxWire::kPacketSaltedMagic, "raw unmarked (0x%02x)", p.data[0]);
}

void testDuplicateIgnored() {
  Sim sim;
  VoxServe s(sim);
  sim.clips[kSid] = {1, clip1200(156)};
  const std::vector<uint8_t> path = {0x11};
  const auto req = dartFetch(kSid, kKeyA, nullptr, true, &path);
  s.onPayload(req.data(), req.size(), -1);
  s.onPayload(req.data(), req.size(), -1);   // heard twice
  sim.run(s, 5000);
  CHECK(sim.sent.size() == 1, "duplicate served: %zu", sim.sent.size());
  s.onPayload(req.data(), req.size(), -1);   // still inside 6 s
  sim.run(s, 5000);
  CHECK(sim.sent.size() == 1, "repeat inside 6 s served");
  s.onPayload(req.data(), req.size(), -1);   // a genuine retry later
  sim.run(s, 5000);
  CHECK(sim.sent.size() == 2, "retry after 6 s not served");
}

void testNewerRequestReplaces() {
  Sim sim;
  VoxServe s(sim);
  sim.clips[kSid] = {1, clip1200()};
  const std::vector<uint8_t> path = {0x11};
  const auto all = dartFetch(kSid, kKeyA, nullptr, true, &path);
  s.onPayload(all.data(), all.size(), -1);
  sim.run(s, 5000);   // a few packets out
  const size_t before = sim.sent.size();
  CHECK(before >= 2 && before < 10, "%zu sent before the new request", before);
  const std::vector<int> miss = {8, 9};
  const auto some = dartFetch(kSid, kKeyA, &miss, true, &path);
  s.onPayload(some.data(), some.size(), -1);
  sim.run(s, 30000);
  std::vector<int> after;
  for (size_t i = before; i < sim.sent.size(); i++) after.push_back(sim.sent[i].data[5]);
  CHECK(after == std::vector<int>({8, 9}), "after the newer request: %zu packets", after.size());
  CHECK(sim.allocs == 0, "leak");
}

void testQueuedListeners() {
  Sim sim;
  VoxServe s(sim);
  sim.clips[kSid] = {1, clip1200(300)};
  const std::vector<uint8_t> pa = {0x11}, pb = {0x22};
  const auto ra = dartFetch(kSid, kKeyA, nullptr, true, &pa);
  const auto rb = dartFetch(kSid, kKeyB, nullptr, true, &pb);
  s.onPayload(ra.data(), ra.size(), -1);
  s.onPayload(rb.data(), rb.size(), -1);
  sim.run(s, 30000);
  CHECK(sim.sent.size() == 4, "%zu", sim.sent.size());
  CHECK(sim.sent.size() == 4 && sim.sent[0].path[0] == 0x11 && sim.sent[1].path[0] == 0x11 &&
            sim.sent[2].path[0] == 0x22 && sim.sent[3].path[0] == 0x22,
        "one listener at a time, in arrival order");
}

// Several listeners on a channel: one burst, each request adding what it
// lacks, never starting over; zero-hop while everyone asking is in range.
void testChannelMergeAndZeroHop() {
  Sim sim;
  VoxServe s(sim);
  sim.air = 150;
  sim.clips[kSid] = {1, clip1200()};   // 10 packets
  s.markOwn(kSid);
  const auto ra = dartFetch(kSid, kKeyA, nullptr, true, nullptr);
  s.onPayload(ra.data(), ra.size(), 2, 0);   // heard directly
  sim.run(s, 1000 + 3 * 210 + 50);           // up to 1 s gathering, then a few packets
  const size_t first = sim.sent.size();
  CHECK(first >= 2 && first < 10, "%zu sent before the second listener", first);
  CHECK(sim.zero_hop_sends == (int)first, "in range: zero-hop");
  if (first >= 2) {
    const uint32_t gap = sim.sent[1].at - sim.sent[0].at;
    CHECK(gap >= 210 && gap <= 220, "zero-hop gap %u (air + 60)", gap);
  }
  // A second listener in range asks for everything: added, not restarted.
  const auto rb = dartFetch(kSid, kKeyB, nullptr, true, nullptr);
  s.onPayload(rb.data(), rb.size(), 2, 0);
  sim.run(s, 20000);
  std::vector<int> idx;
  for (auto& p : sim.sent) idx.push_back(p.data[5]);
  CHECK(sim.sent.size() == 10 + first, "sent %zu (10 + the %zu B missed)", sim.sent.size(), first);
  // Each packet once for A, plus once more for each one B missed: 0..9 and
  // 0..first-1, nothing else.
  std::vector<int> want;
  for (int i = 0; i < 10; i++) want.push_back(i);
  for (int i = 0; i < (int)first; i++) want.push_back(i);
  std::sort(idx.begin(), idx.end());
  std::sort(want.begin(), want.end());
  CHECK(idx == want, "each packet once per listener that lacked it");
  CHECK(!reassemble(sim, kSid, 10).empty(), "complete");
  CHECK(sim.allocs == 0, "leak");

  // A listener two hops away joins: the rest goes out as spaced floods.
  Sim sim2;
  VoxServe s2(sim2);
  sim2.air = 150;
  sim2.clips[kSid] = {1, clip1200()};
  s2.markOwn(kSid);
  s2.onPayload(ra.data(), ra.size(), 2, 0);
  sim2.run(s2, 1000 + 2 * 210 + 50);
  const int zh = sim2.zero_hop_sends;
  s2.onPayload(rb.data(), rb.size(), 2, 2);
  sim2.run(s2, 40000);
  CHECK(sim2.zero_hop_sends == zh, "no zero-hop once someone is out of range");
  const size_t n = sim2.sent.size();
  CHECK(n >= 2 && sim2.sent[n - 1].at - sim2.sent[n - 2].at >= 790, "flood gap %u",
        n >= 2 ? sim2.sent[n - 1].at - sim2.sent[n - 2].at : 0);
}

// A request for the whole clip gets its parity after the data (VoxFec.h,
// byte for byte as the app makes it), and a listener that lost packets
// rebuilds them from it; a request for some packets gets none.
void testParityAfterWholeClip() {
  Sim sim;
  VoxServe s(sim);
  sim.clips[kSid] = {1, clip1200()};   // 10 voice packets: 3 parity
  const std::vector<uint8_t> path = {0x11};
  const auto req = dartFetch(kSid, kKeyA, nullptr, true, &path);
  s.onPayload(req.data(), req.size(), -1);
  sim.run(s, 120000);
  CHECK(sim.sent.size() == 10 && sim.parity.size() == 3, "%zu data, %zu parity", sim.sent.size(), sim.parity.size());
  CHECK(!sim.parity.empty() && sim.parity[0].at > sim.sent.back().at, "parity after the data");
  for (auto& p : sim.parity) CHECK(p.data.size() <= 163 && p.data[0] == 0x56, "a plain voice packet, fits GRP_DATA");
  // The same bytes VoxFec makes for the clip.
  const auto clip = clip1200();
  std::vector<uint8_t> padded(10 * 156, 0);
  memcpy(padded.data(), clip.data(), clip.size());
  for (int k = 0; k < (int)sim.parity.size(); k++) {
    int b, j;
    VoxFec::parityAt(10, false, k, &b, &j);
    uint8_t out[VoxFec::kMaxParityPacket];
    const int n = VoxFec::encodeParity(kSid, padded.data(), 10, 156, 1500 - 9 * 156, b, j, out, sizeof out);
    CHECK(sim.parity[k].data == std::vector<uint8_t>(out, out + n), "parity %d differs", k);
  }
  // A listener that lost packets 2, 5 and 9 of it gets them back.
  std::vector<uint8_t> store(10 * 156);
  VoxWire::Assembly a;
  a.begin(kSid, 10, store.data(), store.size(), 156);
  for (auto& p : sim.sent) {
    VoxWire::Packet pk;
    if (VoxWire::parsePacket(p.data.data(), p.data.size(), &pk) && pk.index != 2 && pk.index != 5 && pk.index != 9)
      a.add(pk);
  }
  std::vector<VoxFec::Parity> pv;
  for (auto& p : sim.parity) pv.push_back(VoxFec::Parity{p.data[5], (uint8_t)(p.data.size() - 6), p.data.data() + 6});
  const int got = VoxFec::rebuild(10, 156, a.payload(), [&](int i) { return a.have((uint8_t)i); }, pv.data(),
                                  (int)pv.size(), [&](int i, const uint8_t* d, int len) {
                                    a.add(VoxWire::Packet{kSid, (uint8_t)i, d, len, false, 0});
                                  });
  CHECK(got == 3 && a.complete() && std::vector<uint8_t>(a.payload(), a.payload() + a.payloadLen()) == clip,
        "rebuilt %d", got);

  Sim sim2;
  VoxServe s2(sim2);
  sim2.clips[kSid] = {1, clip1200()};
  const std::vector<int> miss = {1, 4};
  const auto some = dartFetch(kSid, kKeyA, &miss, true, &path);
  s2.onPayload(some.data(), some.size(), -1);
  sim2.run(s2, 30000);
  CHECK(sim2.sent.size() == 2 && sim2.parity.empty(), "missing packets only: %zu, %zu parity", sim2.sent.size(),
        sim2.parity.size());
}

// A zero-hop request (empty reply path) reaches everyone in range: a radio
// that isn't the author waits a moment and leaves out what it hears another
// node send meanwhile; the author answers at once.
void testZeroHopHoldsBack() {
  Sim sim;
  VoxServe s(sim);
  sim.clips[kSid] = {1, clip1200()};   // held here, recorded elsewhere
  const std::vector<uint8_t> empty;
  const std::vector<int> miss = {3, 6, 8};
  const auto req = dartFetch(kSid, kKeyA, &miss, true, &empty);
  s.onPayload(req.data(), req.size(), -1);
  sim.run(s, 500);
  CHECK(sim.sent.empty(), "not the author: waits");
  // Another holder sends packet 6 meanwhile.
  uint8_t pk[200];
  const auto clip = clip1200();
  const int n = VoxWire::encodePacket(kSid, 6, clip.data() + 6 * 156, 156, 0x42, pk, sizeof pk);
  s.onPayload(pk, (size_t)n, -1);
  sim.run(s, 30000);
  std::vector<int> idx;
  for (auto& p : sim.sent) idx.push_back(p.data[5]);
  CHECK(idx == std::vector<int>({3, 8}), "sent %zu: leaves out the one heard", idx.size());
  CHECK(!sim.sent.empty() && sim.sent[0].path.empty(), "answered zero-hop");

  Sim own;
  VoxServe so(own);
  own.clips[kSid] = {1, clip1200()};
  so.markOwn(kSid);
  so.onPayload(req.data(), req.size(), -1);
  own.run(so, 500);
  CHECK(!own.sent.empty(), "the author answers at once");
}

void testIgnores() {
  Sim sim;
  VoxServe s(sim);
  sim.clips[kSid] = {1, clip1200()};
  const std::vector<uint8_t> path = {0x11};
  const auto own = dartFetch(kSid, sim.self6, nullptr, true, &path);
  s.onPayload(own.data(), own.size(), -1);   // our own request echoed back
  const auto unknown = dartFetch(0x12345678, kKeyA, nullptr, true, &path);
  s.onPayload(unknown.data(), unknown.size(), -1);
  const uint8_t voice[] = {0x56, 0x0A, 0x0B, 0x0C, 0x0D, 0, 1, 2, 3};
  s.onPayload(voice, sizeof voice, -1);       // a data packet, not a request
  const uint8_t junk[] = {0x72, 1, 2};
  s.onPayload(junk, sizeof junk, -1);
  sim.run(s, 10000);
  CHECK(sim.sent.empty(), "sent %zu", sim.sent.size());
  CHECK(sim.allocs == 0, "leak");
  // Missing indices past the clip's end are skipped, not sent as garbage.
  const std::vector<int> miss = {200};
  const auto past = dartFetch(kSid, kKeyB, &miss, true, &path);
  s.onPayload(past.data(), past.size(), -1);
  sim.run(s, 5000);
  CHECK(sim.sent.empty() && sim.allocs == 0, "out-of-range index");
}

void testSendRetry() {
  Sim sim;
  VoxServe s(sim);
  sim.clips[kSid] = {1, clip1200(156)};
  sim.fail_sends = 3;   // the packet pool is momentarily full
  const std::vector<uint8_t> path = {0x11};
  const auto req = dartFetch(kSid, kKeyA, nullptr, false, &path);
  s.onPayload(req.data(), req.size(), -1);
  sim.run(s, 5000);
  CHECK(sim.sent.size() == 1, "should retry a refused send");
}

void testParsers() {
  VoxWire::FetchRequest r;
  // Ranges and case, as Dart writes and reads them.
  CHECK(VoxWire::parseTextFetch("VR3:2sbggd:a:A1A2A3A4A5A6", &r) && r.all && r.key6[0] == 0xA1, "upper-case key");
  CHECK(VoxWire::parseTextFetch(" VR3:1:m0.a-c:a1a2a3a4a5a6\n", &r) && r.n_missing == 4 && r.missing[3] == 12,
        "ranges");
  CHECK(!VoxWire::parseTextFetch("VR3:1:m:a1a2a3a4a5a6", &r), "empty range list");
  CHECK(!VoxWire::parseTextFetch("VR3:1:m3-1:a1a2a3a4a5a6", &r), "backwards range");
  CHECK(!VoxWire::parseTextFetch("VR3:1:m1-2-3:a1a2a3a4a5a6", &r), "double dash");
  CHECK(!VoxWire::parseTextFetch("VR3:1:m73:a1a2a3a4a5a6", &r), "index 255");
  CHECK(!VoxWire::parseTextFetch("VR3:1:x:a1a2a3a4a5a6", &r), "bad want");
  CHECK(!VoxWire::parseTextFetch("VR3:1:a:a1a2a3a4a5", &r), "short key");
  CHECK(!VoxWire::parseTextFetch("VR3:1:a:a1a2a3a4a5a6:x", &r), "extra field");
  // Round trip through the firmware's own encoder.
  const uint8_t miss[] = {9, 1, 2, 3, 40};
  char txt[128];
  CHECK(VoxWire::encodeTextFetch(kSid, kKeyA, miss, 5, txt, sizeof txt) > 0, "encode");
  CHECK(!strcmp(txt, "VR3:2sbggd:m1-3.9.14:a1a2a3a4a5a6"), "encoded %s", txt);
  CHECK(VoxWire::parseTextFetch(txt, &r) && r.n_missing == 5 && r.missing[4] == 40, "round trip");
  // Binary: Dart's bytes, and a path that's too long is ignored rather than rejected.
  const std::vector<int> m2 = {4};
  const std::vector<uint8_t> longpath(9, 0x77);
  const auto b = dartFetch(kSid, kKeyA, &m2, true, &longpath);
  CHECK(VoxWire::parseFetch(b.data(), b.size(), &r) && !r.all && r.n_missing == 1 && r.salt_ok && !r.has_path,
        "binary parse");
  // The envelope the radio posts.
  char env[48];
  CHECK(VoxWire::encodeVoiceEnvelope(kSid, 1, 10, 10, env, sizeof env) > 0, "envelope");
  CHECK(!strcmp(env, "VE3:2sbggd:1:a:a"), "envelope %s", env);
  VoxWire::Envelope e;
  CHECK(VoxWire::parseEnvelope(env, &e) && e.kind == VoxWire::kCodec2 && e.total == 10 && e.seconds() == 10 &&
            e.sid == kSid,
        "envelope parses as Codec2");
  CHECK(!VoxWire::encodeVoiceEnvelope(kSid, 32, 10, 10, env, sizeof env), "never a VE3 envelope for Mimi");
}


// Fast MediaMesh (prototype): a channel request that takes repair packets
// (flag 0x10) and lacks 4 gets fresh repair packets (VoxFountain.h), enough
// with a margin, which rebuild exactly what it lacks; none of the 4 packets
// themselves. Without the setting, the packets as before.
void testFastRepairAnswer() {
  Sim sim;
  VoxServe s(sim);
  s.setFast(true);
  const std::vector<uint8_t> bits = clip1200(1000);   // 7 packets, the last 64 bytes
  sim.clips[kSid] = {1, bits};
  s.markOwn(kSid);
  const uint8_t miss[4] = {1, 2, 4, 6};
  uint8_t req[64];
  const int n = VoxWire::encodeFetch(kSid, kKeyA, miss, 4, req, sizeof req, true, nullptr, 0, 3, true, true);
  CHECK(n > 0 && (req[5] & VoxWire::kFetchFlagFountainOk), "the request says it takes repair packets");
  VoxWire::FetchRequest fr;
  CHECK(VoxWire::parseFetch(req, (size_t)n, &fr) && fr.fountain_ok, "parsed back");
  s.onPayload(req, (size_t)n, 3, 1);
  sim.run(s, 60000);
  const int want = VoxFountain::needed(4);
  CHECK((int)sim.sent.size() == want, "sent %zu, want %d repair packets", sim.sent.size(), want);
  // A listener holding 0, 3, 5 rebuilds 1, 2, 4, 6 from them.
  std::vector<uint8_t> buf(7 * 156, 0);
  bool have[7] = {true, false, false, true, false, true, false};
  for (int i = 0; i < 7; i++)
    if (have[i]) memcpy(buf.data() + i * 156, bits.data() + i * 156, i == 6 ? 64 : 156);
  std::vector<VoxFountain::Repair> rep;
  for (auto& p : sim.sent) {
    VoxWire::Packet pk;
    CHECK(VoxWire::parsePacket(p.data.data(), p.data.size(), &pk), "parses");
    CHECK(pk.sid == VoxFountain::repairSid(kSid), "a repair packet's session id");
    CHECK(p.data.size() <= 163, "GRP_DATA limit");
    rep.push_back(VoxFountain::Repair{pk.index, (uint8_t)pk.len, pk.data});
  }
  std::vector<uint8_t> out(7 * 156, 0);
  const int got = VoxFountain::rebuild(
      7, 156, buf.data(), [&](int i) { return have[i]; }, rep.data(), (int)rep.size(),
      [&](int i, const uint8_t* d, int len) { memcpy(out.data() + i * 156, d, (size_t)len); });
  CHECK(got == 4, "rebuilt %d", got);
  for (int i : {1, 2, 4, 6})
    CHECK(memcmp(out.data() + i * 156, bits.data() + i * 156, i == 6 ? 64 : 156) == 0, "packet %d", i);
  // Seeds all differ (a radio drops a repeat).
  for (size_t a = 0; a < rep.size(); a++)
    for (size_t b = a + 1; b < rep.size(); b++) CHECK(rep[a].seed != rep[b].seed, "seed %zu/%zu", a, b);

  // Setting off: the 4 packets themselves.
  Sim old;
  VoxServe o(old);
  old.clips[kSid] = {1, bits};
  o.markOwn(kSid);
  o.onPayload(req, (size_t)n, 3, 1);
  old.run(o, 60000);
  CHECK(old.sent.size() == 4, "off: sent %zu", old.sent.size());

  // Fast pace: closer than the base gap, never under the fast floor.
  floodpace::Pace fast(338, true), slow(338);
  CHECK(fast.gapMs(1) < slow.gapMs(1), "fast %u < slow %u", fast.gapMs(1), slow.gapMs(1));
  CHECK(fast.gapMs(3) == floodpace::fastBaseGapMs(338), "fast floor for any hops");
}

// Repair packets (VoxFountain.h) and data from one sender, as the app's
// _ChannelBurst: a listener that takes repair packets lacks 1-10, one that
// doesn't lacks 1-8. 1-8 go as data for the second; the first is owed repair
// packets only for 9-10 (and a share for 1-8), not for all ten.
void testFastRepairMixedListeners() {
  Sim sim;
  sim.air = 150;
  VoxServe s(sim);
  s.setFast(true);
  const std::vector<uint8_t> bits = clip1200(20 * 156);   // 20 packets
  sim.clips[kSid] = {1, bits};
  s.markOwn(kSid);
  uint8_t a[10], b[8];
  for (int i = 0; i < 10; i++) a[i] = (uint8_t)(i + 1);
  for (int i = 0; i < 8; i++) b[i] = (uint8_t)(i + 1);
  uint8_t ra[64], rb[64];
  const int na = VoxWire::encodeFetch(kSid, kKeyA, a, 10, ra, sizeof ra, true, nullptr, 0, 3, true, true);
  const int nb = VoxWire::encodeFetch(kSid, kKeyB, b, 8, rb, sizeof rb, true, nullptr, 0, 3, true, false);
  s.onPayload(ra, (size_t)na, 3, 1);
  s.onPayload(rb, (size_t)nb, 3, 1);
  sim.run(s, 120000);
  int data = 0, repairs = 0;
  std::vector<VoxFountain::Repair> rep;
  std::vector<uint8_t> buf(20 * 156, 0);
  bool have[20];
  for (int i = 0; i < 20; i++) {
    have[i] = i < 1 || i > 10;
    if (have[i]) memcpy(buf.data() + i * 156, bits.data() + i * 156, 156);
  }
  for (auto& p : sim.sent) {
    VoxWire::Packet pk;
    if (!VoxWire::parsePacket(p.data.data(), p.data.size(), &pk)) continue;
    if (pk.sid == VoxFountain::repairSid(kSid)) {
      repairs++;
      rep.push_back(VoxFountain::Repair{pk.index, (uint8_t)pk.len, pk.data});
    } else if (pk.sid == kSid) {
      data++;
      if (pk.index < 20 && !have[pk.index]) {
        have[pk.index] = true;
        memcpy(buf.data() + pk.index * 156, pk.data, pk.len);
      }
    }
  }
  CHECK(data == 8, "data %d, want 8", data);
  const int want = VoxFountain::owed(2, 8, 0);
  CHECK(repairs == want, "repair packets %d, want %d (was %d)", repairs, want, VoxFountain::needed(10));
  const int got = VoxFountain::rebuild(
      20, 156, buf.data(), [&](int i) { return have[i]; }, rep.data(), (int)rep.size(),
      [&](int i, const uint8_t* d, int len) { memcpy(buf.data() + i * 156, d, (size_t)len); });
  CHECK(got == 2, "the listener taking repair packets rebuilt %d of 9-10", got);
  CHECK(memcmp(buf.data(), bits.data(), bits.size()) == 0, "and has the whole clip");
}

// A listener taking repair packets that lacks all 200 can list only 124: the
// rest after the last it names goes too, all as data, then a first pass's
// repair packets; not 144 repair packets that could never rebuild 200.
void testFastRepairCappedList() {
  Sim sim;
  sim.air = 150;
  VoxServe s(sim);
  s.setFast(true);
  const std::vector<uint8_t> bits = clip1200(200 * 156);
  sim.clips[kSid] = {1, bits};
  s.markOwn(kSid);
  uint8_t miss[200];
  for (int i = 0; i < 200; i++) miss[i] = (uint8_t)i;
  uint8_t req[200];
  const int n = VoxWire::encodeFetch(kSid, kKeyA, miss, 200, req, sizeof req, true, nullptr, 0, 3, true, true);
  VoxWire::FetchRequest fr;
  CHECK(VoxWire::parseFetch(req, (size_t)n, &fr) && fr.n_missing == VoxFountain::kListCap, "lists %d", fr.n_missing);
  s.onPayload(req, (size_t)n, 3, 1);
  sim.run(s, 600000);
  bool seen[200] = {false};
  int data = 0, repairs = 0;
  for (auto& p : sim.sent) {
    VoxWire::Packet pk;
    if (!VoxWire::parsePacket(p.data.data(), p.data.size(), &pk)) continue;
    if (pk.sid == VoxFountain::repairSid(kSid)) repairs++;
    else if (pk.sid == kSid && pk.index < 200) { data++; seen[pk.index] = true; }
  }
  int distinct = 0;
  for (bool b : seen) distinct += b;
  CHECK(distinct == 200 && data == 200, "data %d (%d distinct), want all 200", data, distinct);
  CHECK(repairs == VoxFountain::firstPass(200), "repair packets %d, want %d", repairs, VoxFountain::firstPass(200));

  // Setting off: the 124 it named, as before.
  Sim old;
  old.air = 150;
  VoxServe o(old);
  old.clips[kSid] = {1, bits};
  o.markOwn(kSid);
  o.onPayload(req, (size_t)n, 3, 1);
  old.run(o, 600000);
  CHECK((int)old.sent.size() == VoxFountain::kListCap, "off: sent %zu", old.sent.size());
}
}  // namespace

int main(int argc, char** argv) {
  (void)argv;
  setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc > 1) printf("(verbose)\n");
  testParsers();
  testRawAlongRequestPath();
  testRawUsesContactRoute();
  testZeroHopRequestPath();
  testNoRouteGivesUp();
  testVr3WaitsForRoute();
  testVr3TimesOut();
  testChannelMergeAndZeroHop();
  testChannelOwnOnly();
  testChannelFarListener();
  testFloodPace();
  testChannelPaceLearns();
  testMarkedChannelAnswer();
  testFastRepairAnswer();
  testFastRepairMixedListeners();
  testFastRepairCappedList();
  testDuplicateIgnored();
  testNewerRequestReplaces();
  testQueuedListeners();
  testParityAfterWholeClip();
  testZeroHopHoldsBack();
  testIgnores();
  testSendRetry();
  if (g_fail) {
    printf("%d check(s) failed\n", g_fail);
    return 1;
  }
  printf("test_vox_serve: all passed\n");
  return 0;
}

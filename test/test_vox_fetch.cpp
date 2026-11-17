// SPDX-License-Identifier: GPL-3.0-or-later
//
// VoxFetch against a simulated mesh and a simulated MeshCore Open TAK sender
// that answers the way lib/vox/voice_service.dart's _serve does: a direct
// request is answered along the reverse of the request's path when it carries
// one, else along the sender's own route; a VR3 DM makes both radios learn a
// route a few seconds later; a channel request is answered on the channel.

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <set>
#include <string>
#include <vector>

#include "ui-touch/VoxFec.h"
#include "ui-touch/VoxFetch.h"

namespace {

struct Event {
  uint32_t at;
  std::vector<uint8_t> data;
  int channel;   // -1 raw
};

struct Sim : VoxFetchIo {
  // ---- world
  uint32_t now = 1000;
  uint32_t air = 600;                 // ms per photo packet
  uint32_t rng = 1;
  double loss = 0.0;                  // per data packet
  double req_loss = 0.0;              // per raw request (a long route)
  bool we_have_route = true;          // our out_path to the sender
  bool our_route_works = true;        // requests along it arrive
  bool sender_route_works = true;     // the sender's own stored route back to us
  bool reverse_path_works = true;     // replying along our request's path
  bool flood_works = true;            // DMs and channel traffic
  bool sender_is_contact = true;
  bool sender_has_route_to_us = true;
  int hops_to_sender = 1;
  uint8_t sender_pub[32];
  uint8_t self6[6] = {1, 2, 3, 4, 5, 6};
  std::vector<uint8_t> payload;       // what the sender holds
  int pb = VoxWire::kBytesPerPacket;  // codec bytes per packet
  std::vector<Event> inbox;           // to deliver to the engine
  std::vector<int> drop_first;        // indices lost on the first burst only
  int drop_bursts = 1;                // ... or on this many bursts
  // Repeaters (and our own radio) drop a packet whose payload they have
  // already seen: an identical request never reaches the sender twice.
  bool mesh_dedupe = false;
  std::set<std::vector<uint8_t>> seen_requests;
  int duplicate_requests = 0;
  bool alreadySeen(const uint8_t* d, size_t n) {
    if (!seen_requests.insert(std::vector<uint8_t>(d, d + n)).second) {
      duplicate_requests++;
      return mesh_dedupe;
    }
    return false;
  }
  int bursts = 0;
  int channel_hops = -1;              // hops reported for channel packets
  bool send_parity = false;           // an all-packets burst ends with VoxFec parity
  bool image = true;                  // for the parity count
  std::vector<uint8_t> last_packed;   // each raw request's path length
  std::map<uint32_t, std::vector<uint8_t>> store;
  VoxFetch* fetch = nullptr;

  // ---- what happened
  int raw_requests = 0, dm_requests = 0, channel_requests = 0;
  bool last_channel_mark_ok = false;
  int served_packets = 0, changes = 0;
  std::vector<bool> last_req_had_path;
  std::vector<std::string> dm_texts;

  double rnd() { rng = rng * 1103515245u + 12345u; return ((rng >> 8) & 0xFFFF) / 65536.0; }

  uint32_t nowMs() override { return now; }
  void selfKey6(uint8_t out[6]) override { memcpy(out, self6, 6); }
  bool contactRoute(const uint8_t pub[32], uint8_t* path, uint8_t* packed) override {
    if (memcmp(pub, sender_pub, 32) != 0 || !we_have_route) return false;
    for (int i = 0; i < hops_to_sender; i++) path[i] = (uint8_t)(0xA0 + i);
    *packed = (uint8_t)hops_to_sender;
    return true;
  }
  int contactHops(const uint8_t pub[32]) override {
    return memcmp(pub, sender_pub, 32) == 0 && we_have_route ? hops_to_sender : -1;
  }
  bool contactByName(const char* name, uint8_t out[32]) override {
    if (!sender_is_contact || strcmp(name, "Jade") != 0) return false;
    memcpy(out, sender_pub, 32);
    return true;
  }

  // The sender serves `want` (all if empty) as a paced burst on a plane.
  bool mark_answers = false;   // answer channel requests that ask for it with marked (0x58) packets
  void serve(const std::vector<int>& want, bool on_channel, uint32_t start, bool lossy_link_ok, bool marked = false) {
    std::vector<int> idx = want;
    const int total = (int)((payload.size() + pb - 1) / pb);
    if (idx.empty()) for (int i = 0; i < total; i++) idx.push_back(i);
    const uint32_t a = via == VoxFetch::kViaRadio2 && r2air ? r2air : air;
    const uint32_t gap = on_channel ? 4 * a + 150 : 2 * a + 150;
    uint32_t t = start;
    const bool first = bursts++ < drop_bursts;
    if (send_parity && want.empty()) {   // after the data, as the app sends it
      for (int k = 0; k < VoxFec::totalParity(total, image); k++) {
        int b, j;
        VoxFec::parityAt(total, image, k, &b, &j);
        std::vector<uint8_t> padded((size_t)total * pb, 0);
        memcpy(padded.data(), payload.data(), payload.size());
        uint8_t out[VoxFec::kMaxParityPacket];
        const int n = VoxFec::encodeParity(0xC0FFEE01, padded.data(), total, pb,
                                           (int)(payload.size() - (size_t)(total - 1) * pb), b, j, out, sizeof out);
        Event e{start + gap * (uint32_t)(idx.size() + k + 1), std::vector<uint8_t>(out, out + n), on_channel ? 2 : -1};
        inbox.push_back(e);
      }
    }
    int left = (int)idx.size();
    for (int i : idx) {
      t += gap;
      served_packets++;
      left--;
      if (!lossy_link_ok || rnd() < loss) continue;
      bool drop = false;
      for (int d : drop_first) drop |= first && d == i;
      if (drop) continue;
      const size_t off = (size_t)i * pb;
      const size_t n = payload.size() - off < (size_t)pb ? payload.size() - off : (size_t)pb;
      Event e{t, {}, on_channel ? 2 : -1};
      e.data.push_back(marked ? VoxWire::kPacketMarkedMagic : VoxWire::kPacketSaltedMagic);
      uint8_t sid[4];
      VoxWire::writeSid(0xC0FFEE01, sid);
      e.data.insert(e.data.end(), sid, sid + 4);
      e.data.push_back((uint8_t)i);
      e.data.insert(e.data.end(), payload.begin() + off, payload.begin() + off + n);
      e.data.push_back(marked ? (uint8_t)(((bursts & 3) << 6) | (left < 63 ? left : 63)) : (uint8_t)rnd());   // salt
      inbox.push_back(e);
    }
  }

  static std::vector<int> missingOf(const uint8_t* req, size_t len) {
    // As the firmware and app parse it: the indices as a set.
    std::set<int> u;
    if (req[5] & VoxWire::kFetchFlagMissing)
      for (int i = 0; i < req[12] && 13 + i < (int)len; i++) u.insert(req[13 + i]);
    return std::vector<int>(u.begin(), u.end());
  }

  bool sendRaw(const uint8_t* path, uint8_t packed, const uint8_t* data, size_t len) override {
    (void)path;
    assert((packed >> 6) == 0);
    assert(VoxWire::isFetchRequest(data, len) && VoxWire::readSid(data + 1) == 0xC0FFEE01);
    assert(data[5] & VoxWire::kFetchFlagSaltOk);
    raw_requests++;
    last_packed.push_back(packed);
    const bool has_path = data[5] & VoxWire::kFetchFlagHasPath;
    last_req_had_path.push_back(has_path);
    if (has_path) {   // [len][path] after the indices, our route to the sender
      const int at = 13 + data[12];
      assert((int)len == at + 1 + data[at] && data[at] == packed);
    }
    if (alreadySeen(data, len)) return true;
    if (!our_route_works) return true;   // vanishes on the way
    if (req_loss > 0 && rnd() < req_loss) return true;
    const bool back = has_path ? reverse_path_works : (sender_has_route_to_us && sender_route_works);
    serve(missingOf(data, len), false, now + 2 * air, back);
    return true;
  }
  bool sendDmFlood(const uint8_t pub[32], const char* text) override {
    assert(memcmp(pub, sender_pub, 32) == 0);
    dm_requests++;
    dm_texts.push_back(text);
    we_have_route = false;   // the firmware clears it so the DM floods
    if (!flood_works) return true;
    // ACK + path exchange: both sides learn a working route ~3 s later, then
    // the sender (which waits up to 25 s for one) serves over raw data.
    we_have_route = true;
    our_route_works = true;
    sender_has_route_to_us = true;
    sender_route_works = true;
    std::vector<int> m;
    const char* w = strchr(text + 4, ':') + 1;
    if (*w == 'm') {   // "m1-3.z" ranges
      for (const char* p = w + 1; *p && *p != ':';) {
        char* e;
        long a = strtol(p, &e, 36), b = a;
        if (*e == '-') b = strtol(e + 1, &e, 36);
        for (long v = a; v <= b; v++) m.push_back((int)v);
        p = *e == '.' ? e + 1 : e;
      }
    }
    serve(m, false, now + 3000, true);
    return true;
  }
  bool sendChannelData(int channel, uint16_t type, const uint8_t* data, size_t len) override {
    assert(channel == 2 && type == VoxWire::kChannelDataType && VoxWire::isFetchRequest(data, len));
    assert(!(data[5] & VoxWire::kFetchFlagHasPath));
    channel_requests++;
    if (alreadySeen(data, len)) return true;
    last_channel_mark_ok = (data[5] & VoxWire::kFetchFlagMarkOk) != 0;
    if (flood_works)
      serve(missingOf(data, len), true, now + 3 * air + 300, true, mark_answers && last_channel_mark_ok);
    return true;
  }
  uint32_t packetAirMs() override { return air; }
  uint32_t r2air = 0;   // radio2's airtime per packet (0: radio2 off)
  uint32_t radio2PacketAirMs() override { return r2air; }
  bool hasPayload(uint32_t sid) override { return store.count(sid) > 0; }
  bool savePayload(uint32_t sid, const uint8_t* d, size_t n) override {
    store[sid].assign(d, d + n);
    return true;
  }
  void changed(uint32_t) override { changes++; }

  // The radio profile every delivered payload is tagged with (MediaMesh).
  uint8_t via = VoxFetch::kViaUnknown;
  uint8_t hop[2] = {0, 0};   // the last repeater's path hash on every packet (hop_len 0: none)
  uint8_t hop_len = 0;

  // Advance time, delivering what's due and ticking the engine.
  void run(uint32_t ms) {
    const uint32_t end = now + ms;
    while ((int32_t)(now - end) < 0) {
      now += 50;
      for (size_t i = 0; i < inbox.size();) {
        if ((int32_t)(now - inbox[i].at) >= 0) {
          Event e = inbox[i];
          inbox.erase(inbox.begin() + (long)i);
          fetch->onPayload(e.data.data(), e.data.size(), e.channel, via, e.channel >= 0 ? channel_hops : -1,
                           hop_len ? hop : nullptr, hop_len);
        } else {
          i++;
        }
      }
      fetch->tick();
    }
  }
};

constexpr uint32_t kSid = 0xC0FFEE01;

void setup(Sim& sim, size_t payload_len) {
  memset(sim.sender_pub, 0x42, 32);
  sim.payload.resize(payload_len);
  sim.payload[0] = VoxWire::kImagePayloadVersion;
  for (size_t i = 1; i < payload_len; i++) sim.payload[i] = (uint8_t)(i * 13 + 5);
}

VoxWire::Envelope envFor(const Sim& sim) {
  VoxWire::Envelope e;
  e.sid = kSid;
  e.total = (uint8_t)((sim.payload.size() + sim.pb - 1) / sim.pb);
  e.aspect = 150;
  return e;
}

bool gotIt(Sim& sim, VoxFetch& f) {
  return f.status(kSid) == VoxFetch::kComplete && sim.store.count(kSid) && sim.store[kSid] == sim.payload;
}

}  // namespace

int main() {
  // 1. Healthy direct route: one raw request, asks to be answered along its path.
  {
    Sim sim; setup(sim, 2750);
    VoxFetch f(sim); sim.fetch = &f;
    f.onEnvelope(envFor(sim), sim.sender_pub, nullptr, -1, false);
    assert(f.status(kSid) == VoxFetch::kRemote);
    assert(f.request(kSid));
    assert(f.status(kSid) == VoxFetch::kRequesting && f.lastMethod(kSid) == VoxFetch::kDirect);
    sim.run(60000);
    assert(gotIt(sim, f) && sim.raw_requests == 1 && sim.last_req_had_path[0]);
    printf("direct: %d request, %d packets\n", sim.raw_requests, sim.served_packets);
  }

  // 1b. Where the packets came from: untagged is unknown; tagged radio2 reads
  //     "Received via MediaMesh".
  {
    Sim sim; setup(sim, 2750);
    VoxFetch f(sim); sim.fetch = &f;
    f.onEnvelope(envFor(sim), sim.sender_pub, nullptr, -1, true);
    sim.run(60000);
    assert(gotIt(sim, f) && f.via(kSid) == 0 && VoxFetch::viaText(f.via(kSid)) == nullptr);
  }
  {
    Sim sim; setup(sim, 2750); sim.via = VoxFetch::kViaRadio2;
    VoxFetch f(sim); sim.fetch = &f;
    f.onEnvelope(envFor(sim), sim.sender_pub, nullptr, -1, true);
    sim.run(60000);
    assert(gotIt(sim, f) && f.via(kSid) == 2);
    assert(!strcmp(VoxFetch::viaText(f.via(kSid)), "Received via MediaMesh"));
    assert(!strcmp(VoxFetch::viaText(1), "Received via main mesh"));
    assert(!strcmp(VoxFetch::viaText(3), "Received via MediaMesh and main mesh"));
    printf("via: %s\n", VoxFetch::viaText(f.via(kSid)));
  }
  // 1c. The MediaMesh repeater a radio2 packet came through is kept (and none
  //     without radio2, or when the packets carried no repeater).
  {
    Sim sim; setup(sim, 2750); sim.via = VoxFetch::kViaRadio2;
    sim.hop[0] = 0x1f; sim.hop[1] = 0xfb; sim.hop_len = 2;
    VoxFetch f(sim); sim.fetch = &f;
    f.onEnvelope(envFor(sim), sim.sender_pub, nullptr, -1, true);
    sim.run(60000);
    uint8_t h[3] = {0, 0, 0}, n = 0;
    assert(gotIt(sim, f) && f.radio2Hop(kSid, h, &n) && n == 2 && h[0] == 0x1f && h[1] == 0xfb);
  }
  {
    Sim sim; setup(sim, 2750); sim.via = VoxFetch::kViaMain;
    sim.hop[0] = 0x1f; sim.hop[1] = 0xfb; sim.hop_len = 2;
    VoxFetch f(sim); sim.fetch = &f;
    f.onEnvelope(envFor(sim), sim.sender_pub, nullptr, -1, true);
    sim.run(60000);
    uint8_t h[3], n = 0;
    assert(gotIt(sim, f) && !f.radio2Hop(kSid, h, &n));   // main-mesh packets name no MediaMesh repeater
  }

  // 2. 25% loss: re-asks only for what's missing until it has everything.
  {
    Sim sim; setup(sim, 5500); sim.loss = 0.25; sim.rng = 7;
    VoxFetch f(sim); sim.fetch = &f;
    f.onEnvelope(envFor(sim), sim.sender_pub, nullptr, -1, true);
    sim.run(300000);
    assert(gotIt(sim, f));
    assert(sim.served_packets < 36 * 3);   // re-requests were for the gaps, not everything
    printf("lossy: %d requests, %d packets served for 36\n", sim.raw_requests, sim.served_packets);
  }

  // 3. No route to the sender yet: a flooded VR3 DM, then the transfer.
  {
    Sim sim; setup(sim, 2750); sim.we_have_route = false;
    VoxFetch f(sim); sim.fetch = &f;
    f.onEnvelope(envFor(sim), sim.sender_pub, nullptr, -1, true);
    assert(f.lastMethod(kSid) == VoxFetch::kDm && sim.dm_requests == 1);
    assert(sim.dm_texts[0].rfind("VR3:", 0) == 0 && sim.dm_texts[0].find(":a:010203040506") != std::string::npos);
    sim.run(90000);
    assert(gotIt(sim, f) && sim.raw_requests == 0);
    printf("dm: %d DM request\n", sim.dm_requests);
  }

  // 4. The sender's own route back is broken but our path reversed works: the
  //    first request already carries it, so no fallbacks are needed.
  {
    Sim sim; setup(sim, 2750); sim.sender_route_works = false;
    VoxFetch f(sim); sim.fetch = &f;
    f.onEnvelope(envFor(sim), sim.sender_pub, nullptr, -1, true);
    sim.run(60000);
    assert(gotIt(sim, f) && sim.raw_requests == 1 && sim.dm_requests == 0);
  }

  // 5. Reverse path broken, sender's route fine: the next direct request drops
  //    the path so the sender uses its own route.
  {
    Sim sim; setup(sim, 2750); sim.reverse_path_works = false;
    VoxFetch f(sim); sim.fetch = &f;
    f.onEnvelope(envFor(sim), sim.sender_pub, nullptr, -1, true);
    sim.run(200000);
    assert(gotIt(sim, f));
    assert(sim.last_req_had_path.size() >= 1 && sim.last_req_had_path[0]);
    bool tried_without = false;
    for (bool b : sim.last_req_had_path) tried_without |= !b;
    assert(tried_without || sim.dm_requests > 0);
    printf("reverse path broken: %d raw, %d DM\n", sim.raw_requests, sim.dm_requests);
  }

  // 6. Our route to the sender is dead and DMs don't get through either: a
  //    channel post falls back to the channel and completes there.
  {
    Sim sim; setup(sim, 2750); sim.our_route_works = false; sim.flood_works = false;
    VoxFetch f(sim); sim.fetch = &f;
    f.onEnvelope(envFor(sim), nullptr, "Jade", 2, true);
    sim.run(30000);
    sim.flood_works = true;   // the channel itself is fine; only the DM path was bad
    sim.run(600000);
    assert(gotIt(sim, f) && sim.channel_requests >= 1);
    printf("channel fallback: %d raw, %d DM, %d channel\n", sim.raw_requests, sim.dm_requests,
           sim.channel_requests);
  }

  // 7. Channel post from someone who isn't a contact: straight to the channel.
  {
    Sim sim; setup(sim, 2750); sim.sender_is_contact = false;
    VoxFetch f(sim); sim.fetch = &f;
    f.onEnvelope(envFor(sim), nullptr, "Stranger", 2, true);
    assert(f.lastMethod(kSid) == VoxFetch::kChannel);
    sim.run(120000);
    assert(gotIt(sim, f) && sim.raw_requests == 0 && sim.dm_requests == 0);
  }

  // 8. Nothing gets through: gives up within its attempt budget, says so, and a
  //    tap starts over.
  {
    Sim sim; setup(sim, 2750); sim.our_route_works = false; sim.flood_works = false;
    VoxFetch f(sim); sim.fetch = &f;
    f.onEnvelope(envFor(sim), sim.sender_pub, nullptr, 2, true);
    sim.run(3600000);
    assert(f.status(kSid) == VoxFetch::kUnavailable);
    assert(f.attempts(kSid) <= VoxFetch::kMaxAttempts);
    char buf[80];
    assert(strstr(f.statusText(kSid, buf, sizeof buf), "Tap to retry"));
    const int before = sim.raw_requests + sim.dm_requests + sim.channel_requests;
    sim.flood_works = true; sim.our_route_works = true;
    assert(f.request(kSid));
    sim.run(200000);
    assert(gotIt(sim, f) && sim.raw_requests + sim.dm_requests + sim.channel_requests > before);
    printf("gave up after %d attempts, then recovered on a tap\n", before);
  }

  // 9. Someone else's channel transfer arrives before we tap: kept quietly, and
  //    the photo is complete without sending anything.
  {
    Sim sim; setup(sim, 2750);
    VoxFetch f(sim); sim.fetch = &f;
    f.onEnvelope(envFor(sim), nullptr, "Jade", 2, false);
    sim.serve({}, true, sim.now, true);
    sim.run(60000);
    assert(gotIt(sim, f) && sim.raw_requests + sim.dm_requests + sim.channel_requests == 0);
  }

  // 10. Already stored: complete without a session doing anything.
  {
    Sim sim; setup(sim, 2750);
    sim.store[kSid] = sim.payload;
    VoxFetch f(sim); sim.fetch = &f;
    assert(f.status(kSid) == VoxFetch::kComplete);
    f.onEnvelope(envFor(sim), sim.sender_pub, nullptr, -1, true);
    assert(f.status(kSid) == VoxFetch::kComplete && sim.raw_requests == 0);
  }

  // 11. More envelopes than sessions: idle ones are recycled, the transfer in
  //     progress is kept, and junk payloads are ignored.
  {
    Sim sim; setup(sim, 2750);
    VoxFetch f(sim); sim.fetch = &f;
    f.onEnvelope(envFor(sim), sim.sender_pub, nullptr, -1, true);
    for (uint32_t i = 0; i < 20; i++) {
      VoxWire::Envelope e{0x1000 + i, 3, 100};
      sim.now += 10;
      f.onEnvelope(e, nullptr, "x", 1, false);
    }
    const uint8_t junk[] = {0x56, 0xC0, 0xFF, 0xEE, 0x01, 200, 1, 2, 3};   // index out of range
    f.onPayload(junk, sizeof junk, -1);
    f.onPayload(nullptr, 0, -1);
    sim.run(60000);
    assert(gotIt(sim, f));
    assert(f.status(0x1000) == VoxFetch::kUnknown && f.status(0x1000 + 19) == VoxFetch::kRemote);
  }

  // 12. A Codec2 1300 voice clip: 154-byte packets, voice wording.
  {
    Sim sim; setup(sim, 154 * 5 + 40); sim.pb = 154; sim.loss = 0.2; sim.rng = 3;
    VoxFetch f(sim); sim.fetch = &f;
    VoxWire::Envelope e;
    assert(VoxWire::parseEnvelope("VE3:1hjtjb5:3:6:9", &e) && e.sid == kSid && e.packetBytes() == 154);
    char buf[64];
    f.onEnvelope(e, sim.sender_pub, nullptr, -1, false);
    assert(!strcmp(f.statusText(kSid, buf, sizeof buf), "Voice 9s - tap to play"));
    f.request(kSid);
    sim.run(300000);
    assert(gotIt(sim, f) && !strcmp(f.statusText(kSid, buf, sizeof buf), "Voice 9s - tap to play"));
    printf("voice: %d requests, %d packets served for 6\n", sim.raw_requests, sim.served_packets);
  }

  // 13. A push lands before its announcement (the two travel on different
  //     radios): the packets are kept and the photo completes on announce.
  {
    Sim sim; setup(sim, 2750); sim.channel_hops = 0;
    VoxFetch f(sim); sim.fetch = &f;
    sim.serve({}, true, sim.now, true);
    sim.run(60000);
    assert(f.status(kSid) == VoxFetch::kUnknown);
    f.onEnvelope(envFor(sim), nullptr, "Jade", 2, false);
    assert(gotIt(sim, f) && sim.raw_requests + sim.dm_requests + sim.channel_requests == 0);
    printf("push before announcement: complete on announce\n");
  }

  // 14. A push to a listener that didn't ask loses two packets: it shows the
  //     download, then asks for just those two once the push stops.
  {
    Sim sim; setup(sim, 2750); sim.channel_hops = 0; sim.drop_first = {3, 9};
    VoxFetch f(sim); sim.fetch = &f;
    f.onEnvelope(envFor(sim), nullptr, "Jade", 2, false);
    sim.serve({}, true, sim.now, true);
    sim.run(4 * sim.air + 200);
    char buf[64];
    assert(!strcmp(f.statusText(kSid, buf, sizeof buf), "Downloading 1/18"));
    const int pushed = sim.served_packets;
    sim.run(120000);
    assert(gotIt(sim, f) && sim.served_packets == pushed + 2);
    printf("push with loss: finished with %d extra packets\n", sim.served_packets - pushed);
  }

  // 16. A Super Sharp photo (234 packets) over 7 hops where half the requests
  //     and a fifth of the packets are lost: it keeps asking for the last
  //     few instead of stopping at "Got 233/234".
  {
    Sim sim; setup(sim, (size_t)234 * VoxWire::kBytesPerPacket);
    sim.hops_to_sender = 7; sim.loss = 0.2; sim.req_loss = 0.5; sim.rng = 9;
    VoxFetch f(sim); sim.fetch = &f;
    f.onEnvelope(envFor(sim), sim.sender_pub, nullptr, -1, true);
    sim.run(3 * 3600000u);
    assert(gotIt(sim, f));
    assert(sim.raw_requests <= VoxFetch::attemptBudget(234));
    printf("234 over 7 hops: %d requests, %d DMs\n", sim.raw_requests, sim.dm_requests);
  }
  // 16b. Budgets: small transfers keep the old limits; nearly complete ones
  //      and big ones get more; each fruitless request waits longer.
  {
    assert(VoxFetch::attemptBudget(17) == VoxFetch::kMaxAttempts);
    assert(VoxFetch::attemptBudget(234) == 49);
    assert(VoxFetch::stallBudget(0, 234) == VoxFetch::kMaxStalls);
    assert(VoxFetch::stallBudget(60, 234) == VoxFetch::kMaxStalls);
    assert(VoxFetch::stallBudget(120, 234) == 5);
    assert(VoxFetch::stallBudget(233, 234) == 8);
    assert(VoxFetch::stallBudget(14, 17) == 8);   // 3 missing
    assert(VoxFetch::stallBackoff(10000, 0) == 10000);
    assert(VoxFetch::stallBackoff(10000, 1) == 15000);
    assert(VoxFetch::stallBackoff(10000, 4) == 30000);
    assert(VoxFetch::stallBackoff(10000, 5) == 45000);   // then half again each time
    assert(VoxFetch::stallBackoff(10000, 9) == 227812);  // as the app's voxStallBackoff
    assert(VoxFetch::stallBackoff(10000, 12) == VoxFetch::kStallBackoffCapMs);
  }

  // 15. A requested transfer whose last packet is lost: the stall is noticed
  //     a few packet gaps after the stream stops, not after a full request
  //     round trip (8 s and up).
  {
    Sim sim; setup(sim, 2750); sim.drop_first = {17};
    VoxFetch f(sim); sim.fetch = &f;
    f.onEnvelope(envFor(sim), sim.sender_pub, nullptr, -1, true);
    const uint32_t start = sim.now;
    while (!gotIt(sim, f) && sim.now - start < 120000) sim.run(50);
    assert(gotIt(sim, f) && sim.raw_requests == 2);
    // Burst: 2 airtimes out, 18 gaps; the stall wait (the receiver's 1-hop
    // packet gap, as voice_service.dart's _packetGap); the answer.
    const uint32_t gap = 2 * sim.air + 150;
    const uint32_t rx_gap = (uint32_t)(sim.air + 2.5 * 1.15 * sim.air + 400);
    const uint32_t bound = 2 * sim.air + 18 * gap + VoxFetch::stallWait(rx_gap) + 2 * sim.air + gap + 500;
    assert(sim.now - start <= bound);   // was ~13 s later: 3 gaps + 2 trips + 8 s
    printf("tail loss: complete after %lu ms\n", (unsigned long)(sim.now - start));
  }

  // 15b. The same over radio2 (MediaMesh), which is 6x faster here: the lost
  //      tail is asked for again by radio2's timing, not the main radio's.
  {
    Sim sim; setup(sim, 2750); sim.drop_first = {17};
    sim.via = VoxFetch::kViaRadio2; sim.r2air = 100;
    VoxFetch f(sim); sim.fetch = &f;
    f.onEnvelope(envFor(sim), sim.sender_pub, nullptr, -1, true);
    const uint32_t start = sim.now;
    while (!gotIt(sim, f) && sim.now - start < 120000) sim.run(50);
    assert(gotIt(sim, f) && sim.raw_requests == 2);
    const uint32_t gap = 2 * sim.r2air + 150;   // 350 ms between packets
    // Out and back at the main radio's pace, the burst at radio2's; the stall
    // noticed after 3 of radio2's (observed, doubled) gaps + 2.5 s.
    const uint32_t bound = 2 * sim.air + 18 * gap + VoxFetch::stallWait(2 * gap) + 2 * sim.air + gap + 500;
    assert(sim.now - start <= bound);
    printf("tail loss over radio2: complete after %lu ms\n", (unsigned long)(sim.now - start));
  }

  // 15c. Main-mesh packets keep the main radio's timing even with radio2 on,
  //      and a fast-arriving burst is timed by the gaps it actually came at.
  {
    Sim sim; setup(sim, 2750); sim.drop_first = {17};
    sim.via = VoxFetch::kViaMain; sim.r2air = 100;
    VoxFetch f(sim); sim.fetch = &f;
    f.onEnvelope(envFor(sim), sim.sender_pub, nullptr, -1, true);
    const uint32_t start = sim.now;
    while (!gotIt(sim, f) && sim.now - start < 120000) sim.run(50);
    assert(gotIt(sim, f) && sim.raw_requests == 2);
    printf("tail loss on the main mesh, radio2 on: complete after %lu ms\n", (unsigned long)(sim.now - start));
  }

  // 15d. The first packet of a photo still coming in (where a version 2
  //      payload carries its thumbnail) can be read before the rest.
  {
    Sim sim; setup(sim, 2750);
    VoxFetch f(sim); sim.fetch = &f;
    const uint8_t* first = nullptr;
    size_t n = 0;
    f.onEnvelope(envFor(sim), sim.sender_pub, nullptr, -1, true);
    assert(!f.firstPackets(kSid, &first, &n));
    int got = 0;
    while (got < 1 && sim.now < 60000) { sim.run(50); f.status(kSid, &got); }
    assert(got >= 1 && f.status(kSid) == VoxFetch::kReceiving);
    assert(f.firstPackets(kSid, &first, &n) && n >= (size_t)sim.pb && !memcmp(first, sim.payload.data(), n));
    while (got < 2 && sim.now < 60000) { sim.run(50); f.status(kSid, &got); }
    assert(f.firstPackets(kSid, &first, &n) && n == 2u * (size_t)sim.pb && !memcmp(first, sim.payload.data(), n));
    sim.run(60000);
    assert(gotIt(sim, f) && !f.firstPackets(kSid, &first, &n));   // complete: the store has it
  }

  // 15e. A channel photo nobody pushed: the thumbnail probe asks the channel
  //      for packet 0 alone, and nothing more is fetched until a tap.
  {
    Sim sim; setup(sim, 2750);
    VoxFetch f(sim); sim.fetch = &f;
    f.onEnvelope(envFor(sim), nullptr, "Jade", 2, false);
    f.probeFirst(kSid, 4000);
    sim.run(3900);
    assert(sim.channel_requests == 0);
    sim.run(4000);
    assert(sim.channel_requests == 1 && sim.served_packets == 2);   // the thumbnail's two
    sim.run(20000);
    assert(sim.channel_requests == 1);   // it came: no second ask
    const uint8_t* first = nullptr;
    size_t n = 0;
    assert(f.status(kSid) == VoxFetch::kRemote && f.firstPackets(kSid, &first, &n));
    sim.run(60000);
    assert(sim.channel_requests == 1 && sim.raw_requests == 0);   // just those two
  }
  // 15f. Its first packet already came (the push): no probe goes out.
  {
    Sim sim; setup(sim, 2750);
    VoxFetch f(sim); sim.fetch = &f;
    f.onEnvelope(envFor(sim), nullptr, "Jade", 2, false);
    f.probeFirst(kSid, 8000);
    sim.serve({0, 1}, true, sim.now + 500, true);   // the sender's push, cut short (in by ~5.6 s)
    sim.run(30000);
    assert(sim.channel_requests == 0);
  }

  // 15g. The thumbnail's answers keep getting lost: asked three times, then
  //      left (the bubble offers the download, which still works).
  {
    Sim sim; setup(sim, 2750); sim.flood_works = false;
    VoxFetch f(sim); sim.fetch = &f;
    f.onEnvelope(envFor(sim), nullptr, "Jade", 2, false);
    f.probeFirst(kSid, 4000);
    sim.run(120000);
    assert(sim.channel_requests == VoxFetch::kThumbProbeTries);
    assert(f.status(kSid) == VoxFetch::kRemote);
    sim.flood_works = true;
    assert(f.request(kSid));   // the tap
    sim.run(120000);
    assert(gotIt(sim, f));
  }

  // 15h. A Super Sharp photo (247 packets) is taken, not dropped.
  {
    Sim sim; setup(sim, 247 * 156);
    VoxFetch f(sim); sim.fetch = &f;
    assert(envFor(sim).total == 247);
    f.onEnvelope(envFor(sim), sim.sender_pub, nullptr, -1, true);
    assert(f.status(kSid) == VoxFetch::kRequesting);
    sim.run(900000);
    assert(gotIt(sim, f));
    printf("super sharp: %d requests, %d packets served for 247\n", sim.raw_requests, sim.served_packets);
  }

  // 16. The shared timing (lib/vox/vox_timing.dart).
  assert(VoxFetch::gapRequestDelay(false, 800, 210) == 610);
  assert(VoxFetch::gapRequestDelay(true, 800, 210) == 1800);
  assert(VoxFetch::stallWait(800) == 3400);
  assert(VoxFetch::markedWait(0, 900) == 300);
  assert(VoxFetch::markedWait(1, 900) == 2 * 900 + 450);
  assert(VoxFetch::markedWait(3, 600) == 4 * 600 + 400);
  // A marked answer stopped part-way is asked for again within a stall, not
  // after every gap the sender said was left.
  assert(VoxFetch::markedStallWait(100, 2100) == VoxFetch::stallWait(2100));
  assert(VoxFetch::markedWait(100, 2100) > 200000);
  assert(VoxFetch::markedStallWait(1, 900) == VoxFetch::markedWait(1, 900));
  assert(VoxFetch::markedStallWait(0, 900) == 300);

  // 18. End-of-answer marker on a channel: the listener's requests say they read
  //     marked packets, and with them a lost tail is asked for again sooner.
  {
    uint32_t took[2] = {0, 0};
    for (int marked = 0; marked < 2; marked++) {
      Sim sim; setup(sim, 2750); sim.sender_is_contact = false; sim.mark_answers = marked != 0;
      sim.drop_first = {16, 17};   // the end of the first answer is lost
      VoxFetch f(sim); sim.fetch = &f;
      f.onEnvelope(envFor(sim), nullptr, "Jade", 2, true);
      sim.run(5000);
      const int asked = sim.channel_requests;
      const uint32_t start = sim.now;
      while (!gotIt(sim, f) && sim.now - start < 600000) sim.run(50);
      assert(gotIt(sim, f));
      assert(sim.channel_requests > 0 && sim.last_channel_mark_ok);   // channel requests say they read it
      (void)asked;
      took[marked] = sim.now - start;
    }
    printf("lost tail on a channel: %lu ms unmarked, %lu ms marked\n", (unsigned long)took[0], (unsigned long)took[1]);
    assert(took[1] < took[0]);
  }
  assert(VoxFetch::shouldFinishPassive(18, 36) && !VoxFetch::shouldFinishPassive(17, 36));
  assert(!VoxFetch::shouldFinishPassive(36, 36) && !VoxFetch::shouldFinishPassive(0, 0));

  // 17. A "Sharp" Mesh photo: 11 KB, 71 packets, over a lossy channel.
  {
    Sim sim; setup(sim, 11000); sim.loss = 0.1; sim.rng = 5;
    VoxFetch f(sim); sim.fetch = &f;
    assert(envFor(sim).total == 71);
    f.onEnvelope(envFor(sim), sim.sender_pub, nullptr, -1, true);
    sim.run(600000);
    assert(gotIt(sim, f));
    printf("sharp: %d requests, %d packets served for 71\n", sim.raw_requests, sim.served_packets);
  }

  // 18. Three hops, the last two packets lost three bursts running. Each retry
  //     asks for the same two, and the repeaters drop a request they have
  //     already relayed: it only gets through because every request is salted.
  {
    Sim sim; setup(sim, 215 * 156); sim.hops_to_sender = 3; sim.mesh_dedupe = true;
    sim.drop_first = {213, 214}; sim.drop_bursts = 6;
    VoxFetch f(sim); sim.fetch = &f;
    f.onEnvelope(envFor(sim), sim.sender_pub, nullptr, -1, true);
    sim.run(1500000);
    assert(gotIt(sim, f));
    assert(sim.duplicate_requests == 0);
    printf("salted retries: %d requests, %d packets served for 215\n", sim.raw_requests + sim.channel_requests,
           sim.served_packets);
  }

  // 19. A retry before any packet arrived lists the packets (salted) rather
  //     than repeating the identical "all" request.
  {
    Sim sim; setup(sim, 30 * 156); sim.mesh_dedupe = true; sim.drop_first = {}; sim.drop_bursts = 2;
    sim.req_loss = 1.0;
    VoxFetch f(sim); sim.fetch = &f;
    f.onEnvelope(envFor(sim), sim.sender_pub, nullptr, -1, true);
    sim.run(20000);
    sim.req_loss = 0.0;
    sim.run(900000);
    assert(gotIt(sim, f));
    assert(sim.duplicate_requests == 0);
  }

  // 20. Parity (VoxFec): a push that loses 2 packets of a 30-packet photo and
  //     3 of an 11-packet voice clip is rebuilt with no request at all.
  {
    Sim sim; setup(sim, 30 * 156 - 70); sim.send_parity = true; sim.drop_first = {3, 17};
    VoxFetch f(sim); sim.fetch = &f;
    f.onEnvelope(envFor(sim), nullptr, "Jade", 2, false);
    sim.serve({}, true, sim.now, true);
    sim.run(120000);
    assert(gotIt(sim, f) && sim.raw_requests + sim.dm_requests + sim.channel_requests == 0);
    printf("parity: 2 of 30 lost, rebuilt with no request\n");
  }
  {
    Sim sim; setup(sim, 156 * 10 + 60); sim.pb = 156; sim.send_parity = true; sim.image = false;
    sim.drop_first = {0, 5, 10};
    VoxFetch f(sim); sim.fetch = &f;
    VoxWire::Envelope e;
    assert(VoxWire::parseEnvelope("VE3:1hjtjb5:1:b:b", &e) && e.sid == kSid);
    f.onEnvelope(e, nullptr, "Jade", 2, false);
    sim.serve({}, true, sim.now, true);
    sim.run(60000);
    assert(gotIt(sim, f) && sim.raw_requests + sim.dm_requests + sim.channel_requests == 0);
    printf("parity: 3 of 11 voice packets lost, rebuilt with no request\n");
  }
  // 20b. Parity that arrives before the announcement is kept and used.
  {
    Sim sim; setup(sim, 30 * 156 - 70); sim.send_parity = true; sim.drop_first = {8};
    VoxFetch f(sim); sim.fetch = &f;
    sim.serve({}, true, sim.now, true);
    sim.run(120000);   // all of it, parity included, before the announcement
    f.onEnvelope(envFor(sim), nullptr, "Jade", 2, false);
    sim.run(1000);
    assert(gotIt(sim, f) && sim.channel_requests == 0);
  }

  // 21. A push heard straight from the sender (0 hops) stops short and the
  //     sender isn't a contact: one zero-hop raw request asks everyone in
  //     range, with an empty reply path, before the channel.
  {
    Sim sim; setup(sim, 30 * 156 - 70); sim.channel_hops = 0; sim.drop_first = {28, 29}; sim.sender_is_contact = false;
    VoxFetch f(sim); sim.fetch = &f;
    f.onEnvelope(envFor(sim), nullptr, "Stranger", 2, false);
    sim.serve({}, true, sim.now, true);
    sim.run(120000);
    assert(gotIt(sim, f));
    assert(sim.raw_requests >= 1 && sim.last_packed[0] == 0 && sim.last_req_had_path[0]);
    assert(sim.channel_requests == 0);
    printf("zero-hop: %d raw request(s) in range, no channel request\n", sim.raw_requests);
  }

  printf("all VoxFetch scenarios pass\n");
  return 0;
}

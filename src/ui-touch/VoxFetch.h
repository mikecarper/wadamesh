// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Fetches MeshCore Open TAK photos for this radio: the requester side of the
// app's pull transfer (lib/vox/voice_service.dart in meshcore-open-tak), so a
// wadamesh radio with no phone attached can download a photo it saw announced.
//
// A photo is announced with a VI1 envelope; nothing is sent until someone asks.
// Each attempt picks the cheapest way that can work, as the app does:
//   direct   binary fetch over raw data along our route to the sender, asking
//            it to reply along the reverse of that route (alternating with its
//            own route when a direct attempt gets nothing back);
//   dm       a VR3 text DM, flooded (our route is cleared first), whose ACK and
//            path exchange give both radios a route; the sender answers over
//            raw data;
//   channel  the binary request as GRP_DATA (0x5643) on the channel the photo
//            was posted in; the sender, or anyone holding the photo, floods
//            the packets back to every listener.
// Attempts escalate on timeouts sized from the radio's airtime, re-asking only
// for missing packets, up to 15 attempts or 3 in a row that bring nothing.
//
// Pure C++ against VoxFetchIo, so the state machine is host-tested
// (test/test_vox_fetch.cpp). Single-threaded: call everything from one loop.

#include <stddef.h>
#include <stdint.h>

#include "VoxWire.h"

class VoxFetchIo {
 public:
  virtual ~VoxFetchIo() {}
  virtual uint32_t nowMs() = 0;
  virtual void selfKey6(uint8_t out[6]) = 0;
  // Route to a contact for raw data: its packed path length (MeshCore
  // out_path_len) and path bytes. False when there is no direct route.
  virtual bool contactRoute(const uint8_t pub[32], uint8_t* path, uint8_t* packed_len) = 0;
  // Hops to a contact, or -1 if unknown.
  virtual int contactHops(const uint8_t pub[32]) = 0;
  // A contact whose name matches (channel posts only carry a name).
  virtual bool contactByName(const char* name, uint8_t pub_out[32]) = 0;
  virtual bool sendRaw(const uint8_t* path, uint8_t packed_len, const uint8_t* data, size_t len) = 0;
  // Clear the route to the contact, then send `text` as a DM (so it floods).
  virtual bool sendDmFlood(const uint8_t pub[32], const char* text) = 0;
  virtual bool sendChannelData(int channel, uint16_t type, const uint8_t* data, size_t len) = 0;
  // Airtime of one full photo packet with the radio's current settings.
  virtual uint32_t packetAirMs() = 0;
  // The same on radio2 (MediaMesh), 0 when it's off: packets that arrive over
  // radio2 are timed by it, not by the main radio.
  virtual uint32_t radio2PacketAirMs() { return 0; }
  virtual bool hasPayload(uint32_t sid) = 0;
  virtual bool savePayload(uint32_t sid, const uint8_t* data, size_t len) = 0;
  // A session's status changed (the UI redraws its bubble).
  virtual void changed(uint32_t sid) = 0;
  // Spreads listeners' requests apart (0 is fine where nothing else asks).
  virtual uint32_t random32() { return 0; }
  // A line for the serial log (the transfer's timing).
  virtual void log(const char* line) { (void)line; }
};

class VoxFetch {
 public:
  enum Status : uint8_t { kUnknown = 0, kRemote, kRequesting, kReceiving, kComplete, kUnavailable };
  // kZeroHop: a raw request with an empty path, heard by every radio in range
  // (any holding the clip answers zero-hop); for a clip whose packets came 0
  // hops, when no route is known. As the app's _Method.zeroHop.
  enum Method : uint8_t { kNone = 0, kDirect, kDm, kChannel, kZeroHop };

  // Parity packets (VoxFec.h) kept per session.
  static constexpr int kMaxParity = 40;

  static constexpr int kMaxSessions = 6;
  // Attempt and stall budgets for small transfers; larger and nearly
  // finished ones get more (attemptBudget, stallBudget).
  static constexpr int kMaxAttempts = 15;
  static constexpr int kMaxStalls = 3;

  // Requests one transfer may make: a photo of hundreds of packets over many
  // hops loses some on most passes, so its budget grows with its size.
  static int attemptBudget(int total) {
    const int b = 10 + total / 6;
    return b > kMaxAttempts ? b : kMaxAttempts;
  }
  // Requests in a row that may bring nothing before giving up. Far fewer
  // than all the packets missing means the sender is there and the link
  // works: keep at it rather than stop a packet or two short.
  static int stallBudget(int received, int total) {
    if (received <= 0 || total <= 0) return kMaxStalls;
    if (received * 10 >= total * 9 || total - received <= 4) return 8;
    if (received * 2 >= total) return 5;
    return kMaxStalls;
  }
  // Each request in a row that brought nothing waits longer for the next
  // answer: up to 3x over the first four (a long or busy route is slow, not
  // gone), then half as long again each time, at most 4 minutes. As the app's
  // voxStallBackoff.
  static constexpr uint32_t kStallBackoffCapMs = 240000;
  static uint32_t stallBackoff(uint32_t wait, int stalls) {
    const int k = stalls < 4 ? stalls : 4;
    uint64_t d = (uint64_t)wait + (uint64_t)wait * (uint32_t)k / 2;
    for (int i = 4; i < stalls && d < kStallBackoffCapMs; i++) d = d * 3 / 2;
    return d < kStallBackoffCapMs || stalls <= 4 ? (uint32_t)d : kStallBackoffCapMs;
  }

  explicit VoxFetch(VoxFetchIo& io) : io_(io) {}
  ~VoxFetch();

  // A VI1 envelope arrived (or a stored message was shown). `sender_pub` is the
  // DM sender; for channel posts pass nullptr, the post's author name, and the
  // channel index. `start` begins fetching straight away.
  void onEnvelope(const VoxWire::Envelope& env, const uint8_t* sender_pub, const char* sender_name,
                  int channel, bool start);

  // Start, or retry after a failure (a tap on the bubble). False if unknown.
  bool request(uint32_t sid);

  // A photo not being downloaded: in `delay_ms`, unless its first packets have
  // come by then (a channel's push, or another listener's ask answered on the
  // channel), ask for those alone (VoxWire::kThumbPackets, whichever aren't
  // here). They carry the thumbnail (payload
  // version 2), so the bubble shows the picture before anyone taps. On the
  // channel for a channel post; straight to the sender over a known route for
  // a DM; never a flooded DM for this. Timing as lib/vox/vox_timing.dart.
  // Asked up to kThumbProbeTries times, kThumbProbeRetryMs (plus a spread)
  // apart; after that the bubble just offers the download, as it does all
  // along (a probe never holds up a tap).
  void probeFirst(uint32_t sid, uint32_t delay_ms);
  static constexpr int kThumbProbeTries = 3;
  static constexpr uint32_t kThumbProbeRetryMs = 8000;
  static constexpr uint32_t kThumbProbeChannelWaitMs = 4000;
  static constexpr uint32_t kThumbProbeSpreadMs = 4000;
  static constexpr uint32_t kThumbProbeDirectWaitMs = 1500;

  // Which radio profile a payload arrived on (MediaMesh), when known.
  static constexpr uint8_t kViaMain = 0;
  static constexpr uint8_t kViaRadio2 = 1;
  static constexpr uint8_t kViaUnknown = 0xFF;

  // A raw-data payload (channel < 0) or a GRP_DATA 0x5643 payload. `hops`: how
  // far a channel packet came (0 = straight from the sender), -1 if unknown.
  // `last_hop`: the path hash (1-3 bytes) of the repeater that relayed a
  // flooded packet to us last, when it had one.
  void onPayload(const uint8_t* data, size_t len, int channel, uint8_t via = kViaUnknown, int hops = -1,
                 const uint8_t* last_hop = nullptr, uint8_t last_hop_len = 0);

  // Timing shared with lib/vox/vox_timing.dart.
  // After the last packet the sender meant to send, with some missing: wait
  // for stragglers (only relayed packets can still be on their way).
  // After a marked packet saying `left` more of the answer are to come,
  // `gap` ms apart: none left, ask at once (300 ms); else after those and one
  // more gap, plus half a gap (at least 400 ms). Same as voxMarkedWaitMs.
  static uint32_t markedWait(int left, uint32_t gap) {
    if (left <= 0) return 300;
    const uint32_t half = gap / 2 > 400 ? gap / 2 : 400;
    return (uint32_t)(left + 1) * gap + half;
  }
  // As markedWait, but never longer than a stall (stallWait): the next packet
  // re-arms it, so a running answer is never cut short, and one the sender
  // stopped part-way is asked for again in seconds, not after every gap it
  // said was left (a Max clip waited 3 min 36 s). Same as voxMarkedStallWaitMs.
  static uint32_t markedStallWait(int left, uint32_t gap) {
    const uint32_t m = markedWait(left, gap);
    const uint32_t st = stallWait(gap);
    return m < st ? m : st;
  }
  static uint32_t gapRequestDelay(bool relayed, uint32_t packet_gap, uint32_t zero_hop_gap) {
    return relayed ? packet_gap + 1000 : zero_hop_gap + 400;
  }
  // Packets were flowing and stopped: the tail was lost.
  // Three gaps and 1 s (was 2.5 s). Same as voxStallWait.
  static uint32_t stallWait(uint32_t packet_gap) { return packet_gap * 3 + 1000; }
  // A holder that isn't the sender answers only once the clip's packets have
  // been off the air a while (four zero-hop gaps, at least 2 s), waiting at
  // most kHolderAirWaitMaxMs; its first packet follows about
  // kHolderAnswerLeadMs later. Same as the app's voxHolderAirWaitMaxMs,
  // voxHolderAnswerLeadMs and voxAirQuietMs.
  static constexpr uint32_t kHolderAirWaitMaxMs = 20000;
  static constexpr uint32_t kHolderAnswerLeadMs = 3000;
  static uint32_t airQuietMs(uint32_t zero_hop_gap) {
    return zero_hop_gap * 4 > 2000 ? zero_hop_gap * 4 : 2000;
  }
  // A request with no answer yet timing out while the clip's packets are
  // still on the air: holders are still waiting for quiet, so asking again
  // only adds another answer to the same air. How long to hold, or 0 to ask
  // now. Same as voxAirHoldMs.
  static uint32_t airHoldMs(uint32_t since_heard, uint32_t since_asked, uint32_t quiet) {
    if (since_asked > kHolderAirWaitMaxMs + quiet + kHolderAnswerLeadMs) return 0;
    const uint32_t want = quiet + kHolderAnswerLeadMs;
    return since_heard < want ? want - since_heard : 0;
  }
  // Unasked packets (a push) stopped with some missing: ask for the rest when
  // at least half is here.
  static bool shouldFinishPassive(int received, int total) {
    return total > 0 && received < total && received * 2 >= total;
  }

  // Packets kept for photos not announced yet (the announcement comes over
  // the main mesh, the packets over MediaMesh, and either can win).
  static constexpr int kMaxOrphans = 48;
  static constexpr int kOrphanBytes = 160;

  // Timeouts; call every loop iteration.
  void tick();

  Status status(uint32_t sid, int* received = nullptr, int* total = nullptr) const;
  Method lastMethod(uint32_t sid) const;
  int attempts(uint32_t sid) const;

  // Short human status for a bubble, e.g. "Downloading 12/36" or "Voice 7s - tap to play".
  const char* statusText(uint32_t sid, char* buf, size_t cap) const;

  // How a downloaded clip's packets arrived: bit 0 main mesh, bit 1 radio2
  // (MediaMesh); 0 when not known (radio2 off, or downloaded before a reboot).
  uint8_t via(uint32_t sid) const;
  // The start of a photo still coming in, as far as its first packets (up to
  // VoxWire::kThumbPackets, where a version 2 payload carries its thumbnail)
  // have come in order. False until packet 0 is here.
  bool firstPackets(uint32_t sid, const uint8_t** data, size_t* len) const;
  // "Received via MediaMesh", "Received via main mesh", "Received via
  // MediaMesh and main mesh", or nullptr for a via() of 0.
  static const char* viaText(uint8_t via);
  // The repeater that relayed the last radio2 (MediaMesh) packet taken for a
  // clip: its path hash (1-3 bytes). False when none did (heard straight from
  // the sender, or only direct packets, whose path is used up on arrival).
  bool radio2Hop(uint32_t sid, uint8_t out[3], uint8_t* out_len) const;

 private:
  struct Session {
    bool used = false;
    uint32_t sid = 0;
    uint8_t total = 0;
    uint16_t aspect = 0;
    int packet_bytes = VoxWire::kBytesPerPacket;
    VoxWire::Kind kind = VoxWire::kImage;
    Status st = kRemote;
    bool has_pub = false;
    uint8_t pub[32] = {0};
    char name[32] = {0};
    int channel = -1;
    uint8_t* storage = nullptr;
    VoxWire::Assembly asm_;
    int attempts = 0;
    int stalls = 0;
    int direct_failures = 0;
    bool tried_channel = false;
    bool reply_along_path = true;
    bool sent_dm = false;
    Method last = kNone;
    int at_last_attempt = 0;
    int expected_last = -1;
    bool timer_on = false;
    uint32_t deadline = 0;
    uint32_t last_touch = 0;
    const char* note = nullptr;   // why it's unavailable
    uint16_t via_count[2] = {0, 0};   // packets taken from the main mesh / radio2
    bool relayed = false;   // a packet came through a repeater, or didn't say
    uint8_t r2_hop[3] = {0, 0, 0};   // radio2Hop(): the last radio2 packet's last repeater
    uint8_t r2_hop_len = 0;
    uint32_t rx_last_ms = 0;   // when the last new packet came in this burst
    uint32_t rx_gap_ms = 0;    // the bursts' gap between packets (smoothed)
    uint8_t rx_gaps = 0;       // how many gaps that is from
    uint32_t asked_ms = 0;     // when the last request went out (the log's timing)
    bool answered = false;     // a packet came since
    uint32_t heard_ms = 0;     // when any packet of it was last heard, from anyone (copies too)
    bool air_busy_at_ask = false;   // its packets were still on the air when the last request went out
    bool probe_on = false;     // probeFirst(): ask for the thumbnail's packets at probe_at
    uint32_t probe_at = 0;
    uint8_t probe_tries = 0;   // asks made
    uint8_t salt = 0;          // the next request's salt (VoxWire::encodeFetch), counting up
    bool salt_seeded = false;
    // Parity packets heard (VoxFec.h): kMaxParity entries of [info][len][data].
    uint8_t* parity = nullptr;
    uint8_t n_parity = 0;
    uint8_t rebuilt = 0;       // packets rebuilt from parity
    uint8_t min_hops = 0xFF;   // fewest hops a channel packet of it came over
    bool tried_zero_hop = false;
    // Some packet came by raw data (a zero-hop push, a direct answer), which
    // doesn't say how far it came: the sender is likely in range, so ask
    // zero-hop first. As the app's _IncomingClip.rawSeen.
    bool raw_seen = false;
  };

  struct Orphan {
    uint32_t sid;
    uint8_t index;
    uint8_t len;
    uint8_t via;
    bool relayed;
    uint8_t hop[3];
    uint8_t hop_len;
    uint8_t data[kOrphanBytes];
  };

  // Each request's salt: from a random start, then counting up, so no two of a
  // session's requests for the same packets are alike.
  int nextSalt(Session& s);
  Session* find(uint32_t sid);
  const Session* find(uint32_t sid) const;
  Session* claim(uint32_t sid);
  void release(Session& s);
  bool ensureStorage(Session& s);
  Method choose(Session& s);
  void attempt(Session& s);
  void onTimeout(Session& s);
  void accept(Session& s, const VoxWire::Packet& p, uint8_t via = kViaUnknown, const uint8_t* hop = nullptr,
              uint8_t hop_len = 0);
  void keepOrphan(const VoxWire::Packet& p, uint8_t via, bool relayed, const uint8_t* hop, uint8_t hop_len);
  void acceptParity(Session& s, const VoxWire::Packet& p);
  // Rebuilds what the session's parity can; true if any packet came back.
  bool rebuildFromParity(Session& s);
  void afterPacket(Session& s, int index, int left = -1);
  void logf(const char* fmt, ...);
  void adoptOrphans(Session& s);
  uint32_t airMs(const Session& s);
  uint32_t zeroHopGap(uint32_t air);
  uint32_t zeroHopGap() { return zeroHopGap(io_.packetAirMs()); }
  uint32_t burstGap(Session& s);
  void noteGap(Session& s, uint32_t now);
  void complete(Session& s);
  void fail(Session& s, const char* why);
  int hops(const Session& s);
  uint32_t packetGap(bool flood, int hops, uint32_t air);
  uint32_t packetGap(bool flood, int hops) { return packetGap(flood, hops, io_.packetAirMs()); }
  uint32_t waitFor(Session& s);
  void arm(Session& s, uint32_t ms);
  void probe(Session& s);

  VoxFetchIo& io_;
  Session sessions_[kMaxSessions];
  Orphan* orphans_ = nullptr;   // kMaxOrphans, allocated on first use (PSRAM)
  int orphan_next_ = 0;
};

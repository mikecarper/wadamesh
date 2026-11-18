// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Answers fetch requests for voice clips this radio holds, so MeshCore Open TAK
// (or another wadamesh) can download a clip recorded here. Mirrors
// VoxVoiceService._serve in jade-on-mesh/meshcore-open-tak
// (lib/vox/voice_service.dart) so both ends behave the same on air:
//
//   raw request    0x72 via raw data, sent along a direct route. Answered along
//                  the reverse of the request's own path when it carries one
//                  (known to work right now), else along our stored route to
//                  the requester, waiting up to 3 s for one.
//   VR3 DM         the text request a listener floods when it has no route. The
//                  flood makes both radios learn a route a few seconds later, so
//                  we wait up to 25 s for it, then answer as raw data.
//   channel        0x72 as GRP_DATA 0x5643 on the channel the clip was posted in;
//                  answered on that channel. The clip's author answers after
//                  0.15-0.4 s, so listeners' requests that come close together are
//                  answered in one burst, plus a relay's time for each hop the
//                  request came so its flood has cleared, paced by FloodPace.h
//                  (learnt from the requests after each answer); this radio
//                  doesn't stand in for other authors.
//
// A request for the whole clip is answered with its parity packets after the
// data (VoxFec.h), so a listener mends what that pass loses by itself. A
// zero-hop request (an empty reply path) reaches every radio in range: one
// that isn't the clip's author waits 1-3 s and leaves out packets it hears
// another node send meanwhile.
//
// Packets are paced so we never talk over our own repeaters, flooded ones by
// FloodPace.h (same formula as
// the app), salted (0x57) when the requester says it takes them, one request
// served at a time, a repeat of the same request inside 6 s is ignored, and a
// newer request from the same listener for the same clip replaces one in progress.
//
// Pure C++ against VoxServeIo, host-tested in test/test_vox_serve.cpp.

#include <stddef.h>
#include <stdint.h>

#include "FloodPace.h"
#include "VoxWire.h"

class VoxServeIo {
 public:
  virtual ~VoxServeIo() {}
  virtual uint32_t nowMs() = 0;
  virtual uint32_t random32() = 0;
  virtual void selfKey6(uint8_t out[6]) = 0;
  // Our stored route to the contact whose public key starts with `key6`, for raw
  // data: 1-byte hop hashes, sender -> requester order. False when the contact
  // is unknown, has no direct route, or uses multi-byte path hashes.
  virtual bool routeToKey6(const uint8_t key6[6], uint8_t* path, int* len) = 0;
  virtual bool sendRaw(const uint8_t* path, int len, const uint8_t* data, size_t n) = 0;
  // `zero_hop`: direct with an empty path, heard by every radio in range and
  // relayed by none; otherwise flooded like any channel packet.
  virtual bool sendChannelData(int channel, uint16_t type, const uint8_t* data, size_t n, bool zero_hop) = 0;
  // Airtime of one full voice packet at the radio's current settings.
  virtual uint32_t packetAirMs() = 0;
  // A clip this radio holds (recorded here or downloaded): its VE3 wire mode and
  // codec frames, copied into a buffer from alloc(). False if we don't have it.
  virtual bool loadClip(uint32_t sid, uint8_t* mode, uint8_t** bits, size_t* len) = 0;
  virtual void freeClip(uint8_t* bits) = 0;
  virtual void log(const char* line) { (void)line; }
};

class VoxServe {
 public:
  static constexpr int kMaxQueued = 6;
  static constexpr int kMaxOwn = 16;
  static constexpr uint32_t kDupWindowMs = 6000;
  static constexpr uint32_t kRouteWaitMs = 3000;
  static constexpr uint32_t kRouteWaitDmMs = 25000;
  static constexpr uint32_t kOwnChannelDelayMs = 150;    // plus up to 0.25 s at random (gathering)
  static constexpr uint32_t kZeroHopWaitMs = 1000;       // not the author: plus up to 2 s

  explicit VoxServe(VoxServeIo& io) : io_(io) {}
  ~VoxServe();

  // A clip recorded here: answer channel requests for it as its author.
  void markOwn(uint32_t sid);
  bool isOwn(uint32_t sid) const;

  // A raw-data payload (channel < 0) or a GRP_DATA 0x5643 payload on `channel`.
  // Anything but a fetch request is ignored.
  // `hops`: how far a channel request came (0 = heard directly; -1 unknown).
  // Data packets are noted too, to leave out what another node just sent.
  void onPayload(const uint8_t* data, size_t len, int channel, int hops = -1);
  // A DM's text. Returns true if it was a VR3 request (never shown in a chat).
  bool onDirectText(const char* text);

  void tick();

  // Fast MediaMesh (prototype): answer listeners that take repair packets
  // (VoxFountain.h) with fresh repair packets instead of the packets they
  // lack, and pace flooded answers by FloodPace's fast gap.
  void setFast(bool on) { fast_ = on; }
  bool fast() const { return fast_; }

  // For the UI and tests.
  bool busy() const { return active_ >= 0 || queued_ > 0; }
  int packetsSent() const { return sent_total_; }

 private:
  struct Job {
    bool used = false;
    uint32_t sid = 0;
    uint8_t key6[6] = {0};
    int channel = -1;              // >= 0: answer on this channel
    bool all = true;
    uint8_t want[VoxWire::kMaxPackets];
    int n_want = 0;
    bool salt_ok = false;
    bool have_route = false;
    uint8_t route[64];             // sender -> requester, 1-byte hashes
    int route_len = 0;
    uint32_t seq = 0;              // arrival order (served first come, first served)
    uint32_t route_try_at = 0;
    int send_failures = 0;
    uint32_t not_before = 0;       // channel: the author's 300 ms
    uint32_t route_deadline = 0;
    uint32_t gen = 0;
    // While serving
    uint8_t mode = 0;
    uint8_t* bits = nullptr;
    size_t len = 0;
    int next = 0;                  // index into the packets being sent
    int total = 0;
    uint32_t next_at = 0;
    uint32_t gap = 0;
    // Channel jobs: one per clip per channel, whoever asks. Requests add the
    // packets they lack (pend, once started) instead of starting over; the
    // farthest requester decides zero-hop (all in range) or flood.
    uint8_t pend[32] = {0};        // bit i: packet i still to send
    int max_hops = 0;
    uint32_t asked_at = 0;         // when the request came (for heardSince)
    bool zero_hop_req = false;     // a raw request with an empty reply path
    // Parity (VoxFec.h) after the data, for a request for the whole clip.
    int par_total = 0;
    int par_next = 0;
    floodpace::Answer answer;      // flooded data packets sent, for FloodPace
    bool mark_ok = false;          // everyone asking reads marked (0x58) packets
    uint32_t first_sent_ms = 0;    // when its first packet went (the log's timing)
    // Fast MediaMesh: repair packets still to send (VoxFountain.h), after
    // the data; and a first pass ending with them instead of parity.
    int repairs = 0;
    bool repair_first = false;
  };
  struct PaceRec { bool used = false; int channel = -1; floodpace::Pace pace; };
  struct AnswerRec { uint32_t sid = 0; int channel = -1; floodpace::Answer answer; };
  PaceRec paces_[4];
  int paces_next_ = 0;
  AnswerRec answers_[6];
  int answers_next_ = 0;
  // Sends of each packet of the clip last marked (the marked packets' salt).
  uint8_t mark_sends_[256] = {0};
  uint32_t mark_sid_ = 0;

  struct Heard { uint32_t sid; uint8_t index; uint32_t at; };

  struct Recent { uint32_t sig; uint32_t at; };

  void enqueue(const VoxWire::FetchRequest& r, int channel, bool from_dm, int hops = -1);
  bool start(Job& j);
  void finish(Job& j, const char* why);
  int  packetCount(const Job& j) const;
  int  packetIndex(const Job& j, int k) const;
  uint32_t packetGap(bool flood, int hops);
  // Each channel's answer pace (FloodPace.h), and our last answer per clip.
  floodpace::Pace& paceFor(int channel);
  uint32_t zeroHopGap();
  bool mergeChannel(const VoxWire::FetchRequest& r, int channel, int hops);
  static int nextPending(const Job& j);
  uint32_t signature(const Job& j) const;
  uint32_t listenerKey(uint32_t sid, const uint8_t key6[6], int channel) const;
  void logf(const char* fmt, ...);
  bool heardSince(uint32_t sid, int index, uint32_t t) const;
  bool sendParity(Job& j, uint32_t now);
  bool sendRepair(Job& j, uint32_t now);

  bool fast_ = false;   // VoxPhotos::setFast turns it on from the setting
  // The next repair seed for the clip last answered with repair packets, so
  // no two of ours are the same (radios drop repeats).
  uint32_t repair_sid_ = 0;
  int repair_next_ = 0;

  VoxServeIo& io_;
  Job jobs_[kMaxQueued];
  int active_ = -1;
  int queued_ = 0;
  uint32_t own_[kMaxOwn] = {0};
  int own_next_ = 0;
  Recent recent_[16] = {};
  int recent_next_ = 0;
  // Generation per listener (sid + requester or channel): a newer request bumps
  // it, and a job whose generation is stale stops.
  struct Gen { uint32_t key; uint32_t gen; };
  Gen gens_[16] = {};
  int gens_next_ = 0;
  int sent_total_ = 0;
  uint32_t seq_ = 0;
  Heard heard_[48] = {};
  int heard_next_ = 0;

  uint32_t bumpGen(uint32_t key);
  uint32_t currentGen(uint32_t key) const;
};

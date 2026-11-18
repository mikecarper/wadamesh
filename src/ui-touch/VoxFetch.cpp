// SPDX-License-Identifier: GPL-3.0-or-later
#include "VoxFetch.h"

#include <stdarg.h>
#include <stdio.h>
#include "FloodPace.h"

#include "VoxFec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {
inline bool due(uint32_t now, uint32_t deadline) { return (int32_t)(now - deadline) >= 0; }
inline int imax(int a, int b) { return a > b ? a : b; }
inline int imin(int a, int b) { return a < b ? a : b; }
}  // namespace

VoxFetch::~VoxFetch() {
  for (auto& s : sessions_) release(s);
  free(orphans_);
}

VoxFetch::Session* VoxFetch::find(uint32_t sid) {
  for (auto& s : sessions_) if (s.used && s.sid == sid) return &s;
  return nullptr;
}
const VoxFetch::Session* VoxFetch::find(uint32_t sid) const {
  for (auto& s : sessions_) if (s.used && s.sid == sid) return &s;
  return nullptr;
}

// A free slot, else the least recently touched session that isn't mid-transfer,
// else the least recently touched of all.
VoxFetch::Session* VoxFetch::claim(uint32_t sid) {
  Session* pick = nullptr;
  for (auto& s : sessions_) if (!s.used) { pick = &s; break; }
  if (!pick) {
    for (int pass = 0; pass < 2 && !pick; pass++) {
      for (auto& s : sessions_) {
        const bool busy = s.st == kRequesting || s.st == kReceiving;
        if (pass == 0 && busy) continue;
        if (!pick || (int32_t)(s.last_touch - pick->last_touch) < 0) pick = &s;
      }
    }
    release(*pick);
  }
  *pick = Session{};
  pick->used = true;
  pick->sid = sid;
  pick->last_touch = io_.nowMs();
  return pick;
}

void VoxFetch::release(Session& s) {
  free(s.storage);
  s.storage = nullptr;
  free(s.parity);
  s.parity = nullptr;
  s.n_parity = 0;
  s.used = false;
}

bool VoxFetch::ensureStorage(Session& s) {
  if (s.storage) return true;
  const size_t n = (size_t)s.total * (size_t)s.packet_bytes;
  s.storage = (uint8_t*)malloc(n);   // > 4 KB: PSRAM on the device
  if (!s.storage) return false;
  if (!s.asm_.begin(s.sid, s.total, s.storage, n, s.packet_bytes)) {
    free(s.storage);
    s.storage = nullptr;
    return false;
  }
  return true;
}

void VoxFetch::onEnvelope(const VoxWire::Envelope& env, const uint8_t* sender_pub,
                          const char* sender_name, int channel, bool start) {
  if (env.total < 1 || env.total > VoxWire::kMaxClipPackets || env.packetBytes() <= 0) return;
  Session* s = find(env.sid);
  if (!s) {
    s = claim(env.sid);
    s->total = env.total;
    s->aspect = env.aspect;
    s->packet_bytes = env.packetBytes();
    s->kind = env.kind;
    s->st = io_.hasPayload(env.sid) ? kComplete : kRemote;
  }
  s->last_touch = io_.nowMs();
  if (sender_pub) {
    memcpy(s->pub, sender_pub, 32);
    s->has_pub = true;
  }
  if (sender_name && sender_name[0]) {
    strncpy(s->name, sender_name, sizeof s->name - 1);
    s->name[sizeof s->name - 1] = '\0';
  }
  if (channel >= 0) s->channel = channel;
  adoptOrphans(*s);
  if (start) request(env.sid);
}

void VoxFetch::keepOrphan(const VoxWire::Packet& p, uint8_t via, bool relayed, const uint8_t* hop,
                          uint8_t hop_len) {
  if (p.len <= 0 || p.len > kOrphanBytes) return;
  if (!orphans_) {
    orphans_ = (Orphan*)calloc(kMaxOrphans, sizeof(Orphan));   // ~8 KB: PSRAM
    if (!orphans_) return;
  }
  for (int i = 0; i < kMaxOrphans; i++) {
    const Orphan& o = orphans_[i];
    if (o.len && o.sid == p.sid && o.index == p.index) return;   // have it
  }
  Orphan& o = orphans_[orphan_next_];
  orphan_next_ = (orphan_next_ + 1) % kMaxOrphans;
  o.sid = p.sid;
  o.index = p.index;
  o.len = (uint8_t)p.len;
  o.via = via;
  o.relayed = relayed;
  o.hop_len = hop && hop_len <= 3 ? hop_len : 0;
  if (o.hop_len) memcpy(o.hop, hop, o.hop_len);
  memcpy(o.data, p.data, (size_t)p.len);
}

void VoxFetch::adoptOrphans(Session& s) {
  if (!orphans_ || s.st == kComplete) return;
  // Its parity packets (VoxFec.h) first: the data packets then rebuild as
  // they're taken.
  const uint32_t psid = VoxFec::paritySid(s.sid);
  for (int i = 0; i < kMaxOrphans; i++) {
    Orphan& o = orphans_[i];
    if (!o.len || o.sid != psid) continue;
    VoxWire::Packet p{o.sid, o.index, o.data, o.len, false, 0};
    o.len = 0;
    if (s.st != kComplete) acceptParity(s, p);
  }
  for (int i = 0; i < kMaxOrphans; i++) {
    Orphan& o = orphans_[i];
    if (!o.len || o.sid != s.sid) continue;
    if (o.relayed) s.relayed = true;
    VoxWire::Packet p{o.sid, o.index, o.data, o.len, false, 0};
    o.len = 0;
    if (s.st != kComplete) accept(s, p, o.via, o.hop, o.hop_len);
  }
}

bool VoxFetch::request(uint32_t sid) {
  Session* s = find(sid);
  if (!s) return false;
  s->last_touch = io_.nowMs();
  if (s->st == kComplete || s->st == kRequesting || s->st == kReceiving) return true;
  if (io_.hasPayload(sid)) {
    s->st = kComplete;
    io_.changed(sid);
    return true;
  }
  // A channel post names its author; look them up each time (they may have
  // been added as a contact since the photo arrived).
  if (!s->has_pub && s->name[0] && io_.contactByName(s->name, s->pub)) s->has_pub = true;
  s->attempts = 0;
  s->stalls = 0;
  s->direct_failures = 0;
  s->tried_channel = false;
  s->reply_along_path = true;
  s->note = nullptr;
  attempt(*s);
  return true;
}

// --------------------------------------------------------------- timing
//
// Same model as voice_service.dart: _clipHops, _packetGap and _waitFor.

int VoxFetch::hops(const Session& s) {
  if (!s.has_pub) return 3;
  return imax(io_.contactHops(s.pub), 0);
}

uint32_t VoxFetch::packetGap(bool flood, int h, uint32_t air) {
  // An answer on the channel may be paced as slow as FloodPace's slow gap: expect
  // that, so a slow answer isn't taken for a stall (faster ones shorten the wait).
  if (flood) return floodpace::slowGapMs(air);
  if (h <= 0) return (uint32_t)imax(400, (int)(air * 2 + 150));
  const double relay_air = air * 1.15;
  return (uint32_t)(air + imin(h, 2) * 2.5 * relay_air + 400);
}

uint32_t VoxFetch::zeroHopGap(uint32_t air) {
  const uint32_t g = air + 60;   // VoxServe::zeroHopGap
  return g > 150 ? g : 150;
}

// A clip coming in over radio2 alone is paced by radio2's airtime, which is
// usually far shorter than the main radio's.
uint32_t VoxFetch::airMs(const Session& s) {
  if (s.via_count[kViaRadio2] && !s.via_count[kViaMain]) {
    const uint32_t r2 = io_.radio2PacketAirMs();
    if (r2) return r2;
  }
  return io_.packetAirMs();
}

// The gap expected between this transfer's packets. The model's, until a few
// have come in: then twice the gap they actually came at, at most three times
// the model. Sooner: a lost tail is asked for again in seconds, not tens of
// seconds. Later: a sender whose radio is slower than ours (a Keymind Cascade
// radio sharing one chip with radio2 sends radio2 packets with a long
// preamble, over twice the airtime) is waited for. As voxBurstGap.
uint32_t VoxFetch::burstGap(Session& s) {
  const uint32_t air = airMs(s);
  const uint32_t model = packetGap(s.last == kChannel, hops(s), air);
  if (s.rx_gaps < 3) return model;
  uint32_t seen = s.rx_gap_ms * 2;
  const uint32_t low = zeroHopGap(air);
  if (seen < low) seen = low;
  return seen < model * 3 ? seen : model * 3;
}

// Packets of one burst: smooth the gap between them. The first packet after a
// request starts a new burst (its gap is the round trip, not the pacing).
void VoxFetch::noteGap(Session& s, uint32_t now) {
  if (s.rx_last_ms) {
    const uint32_t g = now - s.rx_last_ms;
    if (g < 60000) {
      s.rx_gap_ms = s.rx_gaps ? (s.rx_gap_ms * 3 + g) / 4 : g;
      if (s.rx_gaps < 0xFF) s.rx_gaps++;
    }
  }
  s.rx_last_ms = now;
}

uint32_t VoxFetch::waitFor(Session& s) {
  const int h = hops(s);
  // Once packets flow the stream is live: silence for a few packet gaps means
  // its tail was lost, not that the request is still on its way.
  if (s.st == kReceiving) return stallWait(burstGap(s));
  // voxAnswerWaitMs: on MediaMesh's fast packets (under 400 ms) a channel
  // request is the request and the answer (2 trips), and the margin 2.5 s plus
  // the sender's longest wait for the request's flood to clear, not 8 s.
  const uint32_t air = io_.packetAirMs();
  const uint32_t hop_ms = air * 2 + 700;
  const bool fast = air < 400;
  // In range: the answer is a packet away (a holder waits up to 3 s).
  if (s.last == kZeroHop) return 2 * hop_ms + (fast ? 3500 : 6000);
  uint32_t trips = s.last == kDm ? 5 : s.last == kChannel ? 3 : 2;
  if (!fast) return trips * (uint32_t)(h + 1) * hop_ms + 8000;
  if (s.last == kChannel && trips > 2) trips = 2;
  return trips * (uint32_t)(h + 1) * hop_ms + 2500 + (s.last == kChannel ? floodpace::settleMaxMs(air, h) : 0);
}

void VoxFetch::arm(Session& s, uint32_t ms) {
  s.timer_on = true;
  s.deadline = io_.nowMs() + ms;
}

void VoxFetch::tick() {
  const uint32_t now = io_.nowMs();
  for (auto& s : sessions_) {
    if (s.used && s.timer_on && due(now, s.deadline)) {
      s.timer_on = false;
      onTimeout(s);
    }
    if (s.used && s.probe_on && due(now, s.probe_at)) {
      s.probe_on = false;
      probe(s);
    }
  }
}

void VoxFetch::probeFirst(uint32_t sid, uint32_t delay_ms) {
  Session* s = find(sid);
  if (!s || s->kind != VoxWire::kImage || s->st != kRemote) return;
  s->probe_on = true;
  s->probe_at = io_.nowMs() + delay_ms;
  s->probe_tries = 0;
}

void VoxFetch::probe(Session& s) {
  // Downloading after all, done, the packet came, or asked enough: nothing
  // to ask. The bubble offers the download either way.
  if (s.st != kRemote) return;
  // The thumbnail's packets not here yet (of the first kThumbPackets).
  uint8_t want[VoxWire::kThumbPackets];
  int n_want = 0;
  for (int i = 0; i < VoxWire::kThumbPackets && i < s.total; i++)
    if (!(s.storage && s.asm_.total() && s.asm_.have((uint8_t)i))) want[n_want++] = (uint8_t)i;
  if (!n_want) return;
  if (s.probe_tries >= kThumbProbeTries) return;
  s.probe_tries++;
  // Not here by the next check: ask again (each answer may be lost too).
  s.probe_on = true;
  s.probe_at = io_.nowMs() + kThumbProbeRetryMs + io_.random32() % kThumbProbeSpreadMs;
  uint8_t key6[6];
  io_.selfKey6(key6);
  if (s.channel >= 0) {
    uint8_t req[VoxWire::kFetchMinLen + VoxWire::kThumbPackets + 16];
    const int n = VoxWire::encodeFetch(s.sid, key6, want, n_want, req, sizeof req, true, nullptr, 0,
                                       nextSalt(s), true);   // on a channel: we read marked answers
    if (n > 0) io_.sendChannelData(s.channel, VoxWire::kChannelDataType, req, (size_t)n);
    return;
  }
  if (!s.has_pub && s.name[0] && io_.contactByName(s.name, s.pub)) s.has_pub = true;
  uint8_t path[64], packed = 0;
  if (!s.has_pub || !io_.contactRoute(s.pub, path, &packed) || (packed >> 6) != 0) return;
  const int path_bytes = packed & 0x3F;
  const bool with_path = path_bytes <= VoxWire::kMaxReplyPathBytes;
  uint8_t req[VoxWire::kFetchMinLen + VoxWire::kThumbPackets + 16 + 1 + VoxWire::kMaxReplyPathBytes];
  const int n = VoxWire::encodeFetch(s.sid, key6, want, n_want, req, sizeof req, true,
                                     with_path ? path : nullptr, with_path ? path_bytes : 0,
                                     nextSalt(s));
  if (n > 0) io_.sendRaw(path, packed, req, (size_t)n);
}

// --------------------------------------------------------------- requests

int VoxFetch::nextSalt(Session& s) {
  if (!s.salt_seeded) {
    s.salt = (uint8_t)io_.random32();
    s.salt_seeded = true;
  }
  return s.salt++;
}

VoxFetch::Method VoxFetch::choose(Session& s) {
  uint8_t path[64], packed = 0;
  // Raw data reads the path length as a byte count: 1-byte hashes only.
  const bool route = s.has_pub && io_.contactRoute(s.pub, path, &packed) && (packed >> 6) == 0;
  const bool can_channel = s.channel >= 0;
  // Its packets came 0 hops: whoever sent them is in range, and so may be
  // others holding it. One zero-hop request reaches them all, no route needed.
  // Packets that came by raw data (a zero-hop push, a direct answer) say the
  // same: ask in range first, the channel's flood only if that fails.
  if ((s.min_hops == 0 || s.raw_seen) && !s.tried_zero_hop && s.storage && s.asm_.received() > 0)
    return kZeroHop;
  // Two requests in a row brought nothing: try the channel (anyone holding the
  // photo can answer) before going round the direct options again.
  if (can_channel && s.stalls >= 2 && !s.tried_channel) return kChannel;
  if (s.has_pub) {
    if (route && s.direct_failures == 0) return kDirect;
    if (!s.sent_dm) return kDm;
    if (route && s.direct_failures < 3) return kDirect;
  }
  if (can_channel) return kChannel;
  if (s.has_pub && route) return kDirect;
  if (s.has_pub) return kDm;
  return kNone;
}

void VoxFetch::attempt(Session& s) {
  const Method m = choose(s);
  if (m == kNone) {
    fail(s, s.has_pub ? "no route to the sender. Tap to retry." : "the sender isn't a contact. Tap to retry.");
    return;
  }
  s.attempts++;
  s.last = m;
  if (m == kChannel) s.tried_channel = true;
  if (m == kZeroHop) s.tried_zero_hop = true;
  s.at_last_attempt = s.asm_.total() ? s.asm_.received() : 0;
  s.rx_last_ms = 0;   // the next packet starts a new burst
  s.asked_ms = io_.nowMs();
  // Its packets still on the air (a push, someone's answer): holders other
  // than the sender answer only once it's quiet.
  s.air_busy_at_ask = s.heard_ms && s.asked_ms - s.heard_ms < airQuietMs(zeroHopGap());
  s.answered = false;

  uint8_t missing[VoxWire::kMaxPackets];
  int n_missing = 0;
  if (s.storage) n_missing = s.asm_.missing(missing);
  bool any = s.storage && s.asm_.received() > 0;
  s.expected_last = (any && n_missing) ? missing[n_missing - 1] : s.total - 1;
  // A retry before any packet came in: list the packets rather than ask for
  // "all", so the request can be salted (an unsalted repeat is dropped as
  // already seen). It lists the first kSaltedFetchIndices; the next request
  // asks for the rest.
  if (!any && s.attempts > 1 && s.total > 0 && s.total <= VoxWire::kMaxPackets) {
    n_missing = s.total;
    for (int i = 0; i < n_missing; i++) missing[i] = (uint8_t)i;
    any = true;
    s.expected_last = (n_missing < VoxWire::kSaltedFetchIndices ? n_missing : VoxWire::kSaltedFetchIndices) - 1;
  }
  const int salt = nextSalt(s);

  uint8_t key6[6];
  io_.selfKey6(key6);
  bool sent = false;
  if (m == kDirect) {
    uint8_t path[64], packed = 0;
    io_.contactRoute(s.pub, path, &packed);
    const int path_bytes = packed & 0x3F;
    // Ask the sender to answer along the reverse of the way we reach it,
    // alternating with its own route when a direct attempt got nothing back.
    const bool with_path = s.reply_along_path && path_bytes <= VoxWire::kMaxReplyPathBytes;
    uint8_t req[VoxWire::kFetchMinLen + VoxWire::kMaxFetchIndices + 1 + VoxWire::kMaxReplyPathBytes];
    const int n = VoxWire::encodeFetch(s.sid, key6, any ? missing : nullptr, n_missing, req, sizeof req,
                                       true, with_path ? path : nullptr, path_bytes, salt);
    sent = n > 0 && io_.sendRaw(path, packed, req, (size_t)n);
  } else if (m == kZeroHop) {
    // An empty reply path: holders answer zero-hop too.
    static const uint8_t kNoPath[1] = {0};
    uint8_t req[VoxWire::kFetchMinLen + VoxWire::kMaxFetchIndices + 1];
    const int n = VoxWire::encodeFetch(s.sid, key6, any ? missing : nullptr, n_missing, req, sizeof req, true,
                                       kNoPath, 0, salt);
    sent = n > 0 && io_.sendRaw(kNoPath, 0, req, (size_t)n);
  } else if (m == kDm) {
    s.sent_dm = true;
    char text[200];
    const int n = VoxWire::encodeTextFetch(s.sid, key6, any ? missing : nullptr, n_missing, text, sizeof text);
    sent = n > 0 && io_.sendDmFlood(s.pub, text);
  } else {
    uint8_t req[VoxWire::kFetchMinLen + VoxWire::kMaxFetchIndices];
    const int n = VoxWire::encodeFetch(s.sid, key6, any ? missing : nullptr, n_missing, req, sizeof req, true,
                                       nullptr, 0, salt, true);   // on a channel: we read marked answers
    sent = n > 0 && io_.sendChannelData(s.channel, VoxWire::kChannelDataType, req, (size_t)n);
  }

  if (!sent) {
    if (m != kChannel && m != kZeroHop) s.direct_failures++;
    if (s.attempts < attemptBudget(s.total)) attempt(s);
    else fail(s, "couldn't send the request. Tap to retry.");
    return;
  }
  if (s.st != kReceiving) s.st = kRequesting;
  s.note = nullptr;
  arm(s, stallBackoff(waitFor(s), s.stalls) +
             (s.air_busy_at_ask && s.st != kReceiving ? kHolderAirWaitMaxMs : 0));
  io_.changed(s.sid);
}

void VoxFetch::onTimeout(Session& s) {
  if (s.st == kRemote) {
    // Unasked packets stopped arriving: finish it if most of it is here.
    const int got = s.storage ? s.asm_.received() : 0;
    if (shouldFinishPassive(got, s.total)) request(s.sid);
    return;
  }
  if (s.st != kRequesting && s.st != kReceiving) return;
  // No answer yet, and its packets are still on the air: holders are still
  // waiting for quiet. Hold the next request rather than add another answer.
  if (!s.answered && s.asked_ms && s.heard_ms) {
    const uint32_t now = io_.nowMs();
    const uint32_t hold = airHoldMs(now - s.heard_ms, now - s.asked_ms, airQuietMs(zeroHopGap()));
    if (hold) {
      logf("[VOX] fetch %08lx: still on the air (heard %lu ms ago), holding %lu ms", (unsigned long)s.sid,
           (unsigned long)(now - s.heard_ms), (unsigned long)hold);
      arm(s, hold);
      return;
    }
  }
  const int got = s.storage ? s.asm_.received() : 0;
  const int stall_budget = stallBudget(got, s.total);
  if (s.asked_ms) {
    if (s.answered && s.rx_gaps)
      logf("[VOX] fetch %08lx: timeout, %d/%d, asked %lu ms ago, packets ~%lu ms apart", (unsigned long)s.sid,
           got, (int)s.total, (unsigned long)(io_.nowMs() - s.asked_ms), (unsigned long)s.rx_gap_ms);
    else
      logf("[VOX] fetch %08lx: timeout, %d/%d, asked %lu ms ago, %s", (unsigned long)s.sid, got, (int)s.total,
           (unsigned long)(io_.nowMs() - s.asked_ms), s.answered ? "one packet came" : "nothing came");
  }
  if (got > s.at_last_attempt) {
    s.stalls = 0;
    // A zero-hop request that brought packets: keep asking that way.
    if (s.last == kZeroHop) s.tried_zero_hop = false;
  } else {
    s.stalls++;
    if (s.last != kChannel && s.last != kZeroHop) s.direct_failures++;
    if (s.last == kDirect) s.reply_along_path = !s.reply_along_path;   // try the other way back
    // Stuck for a while: let the channel (anyone holding it can answer) and,
    // once, a fresh flooded route have another go.
    if (s.stalls < stall_budget && s.stalls % 3 == 0) {
      s.tried_channel = false;
      s.tried_zero_hop = false;
    }
    if (s.stalls == 4) s.sent_dm = false;
  }
  const bool channel_left = s.channel >= 0 && !s.tried_channel;
  if ((s.stalls < stall_budget || channel_left) && s.attempts < attemptBudget(s.total)) {
    attempt(s);
    return;
  }
  fail(s, got == 0 ? "unavailable right now. Tap to retry." : "download stalled. Tap to retry.");
}

void VoxFetch::fail(Session& s, const char* why) {
  s.timer_on = false;
  s.st = kUnavailable;
  s.note = why;
  io_.changed(s.sid);
}

// --------------------------------------------------------------- packets

void VoxFetch::onPayload(const uint8_t* data, size_t len, int channel, uint8_t via, int hops,
                         const uint8_t* last_hop, uint8_t last_hop_len) {
  if (!data || len == 0 || VoxWire::isFetchRequest(data, len)) return;   // this radio doesn't serve
  VoxWire::Packet p;
  if (!VoxWire::parsePacket(data, len, &p)) return;
  // Raw data doesn't say how far it came: count it as relayed.
  const bool relayed = channel < 0 || hops != 0;
  Session* s = find(p.sid);
  if (!s) {
    // A parity packet (VoxFec.h): its session id is its clip's, XORed.
    Session* ps = find(VoxFec::paritySid(p.sid));
    if (ps) {
      if (ps->st != kComplete) acceptParity(*ps, p);
      return;
    }
    keepOrphan(p, via, relayed, last_hop, last_hop_len);
    return;
  }
  if (s->st == kComplete) return;
  s->heard_ms = io_.nowMs();   // on the air, whoever it was for
  if (relayed) s->relayed = true;
  if (channel >= 0 && hops >= 0 && hops < s->min_hops) s->min_hops = (uint8_t)hops;
  if (channel < 0) s->raw_seen = true;
  accept(*s, p, via, last_hop, last_hop_len);
}

void VoxFetch::acceptParity(Session& s, const VoxWire::Packet& p) {
  if (p.len < 2 || p.len > 1 + VoxFec::kMaxP) return;
  constexpr int kEntry = 2 + 1 + VoxFec::kMaxP;   // [info][len][data]
  if (!s.parity) {
    s.parity = (uint8_t*)malloc((size_t)kMaxParity * kEntry);   // PSRAM on the device
    if (!s.parity) return;
  }
  for (int k = 0; k < s.n_parity; k++)
    if (s.parity[k * kEntry] == p.index) return;   // have it
  if (s.n_parity >= kMaxParity) return;
  uint8_t* e = s.parity + (size_t)s.n_parity * kEntry;
  e[0] = p.index;
  e[1] = (uint8_t)p.len;
  memcpy(e + 2, p.data, (size_t)p.len);
  s.n_parity++;
  if (!s.storage || s.asm_.received() == 0) return;
  rebuildFromParity(s);
  // Complete, or (parity is part of the burst) keep its timers going.
  afterPacket(s, -1);
}

bool VoxFetch::rebuildFromParity(Session& s) {
  if (!s.parity || !s.n_parity || !s.storage || s.asm_.complete()) return false;
  constexpr int kEntry = 2 + 1 + VoxFec::kMaxP;
  VoxFec::Parity par[kMaxParity];
  for (int k = 0; k < s.n_parity; k++) {
    const uint8_t* e = s.parity + (size_t)k * kEntry;
    par[k] = VoxFec::Parity{e[0], e[1], e + 2};
  }
  const int n = VoxFec::rebuild(
      s.total, s.packet_bytes, s.asm_.payload(), [&](int i) { return s.asm_.have((uint8_t)i); }, par, s.n_parity,
      [&](int i, const uint8_t* d, int len) { s.asm_.add(VoxWire::Packet{s.sid, (uint8_t)i, d, len, false, 0}); });
  if (n <= 0) return false;
  s.rebuilt = (uint8_t)(s.rebuilt + n > 255 ? 255 : s.rebuilt + n);
  return true;
}

void VoxFetch::accept(Session& s, const VoxWire::Packet& p, uint8_t via, const uint8_t* hop, uint8_t hop_len) {
  if (!ensureStorage(s)) return;
  if (!s.asm_.add(p)) return;
  if (via == kViaMain || via == kViaRadio2) {
    if (s.via_count[via] < 0xFFFF) s.via_count[via]++;
  }
  if (via == kViaRadio2) {   // the MediaMesh repeater it came through, if any
    s.r2_hop_len = hop && hop_len <= 3 ? hop_len : 0;
    if (s.r2_hop_len) memcpy(s.r2_hop, hop, s.r2_hop_len);
  }
  if (s.n_parity) rebuildFromParity(s);
  afterPacket(s, p.index, p.marked ? p.left : -1);
}

// After a packet (or a rebuild, index -1) went in: complete, or keep the
// session's timers going.
void VoxFetch::afterPacket(Session& s, int index, int left) {
  s.last_touch = io_.nowMs();
  if (s.st == kRequesting || s.st == kReceiving) {
    noteGap(s, s.last_touch);
    if (!s.answered && s.asked_ms && index >= 0) {
      s.answered = true;
      logf("[VOX] fetch %08lx: first packet %lu ms after asking", (unsigned long)s.sid,
           (unsigned long)(s.last_touch - s.asked_ms));
    }
  }
  if (s.asm_.complete()) {
    complete(s);
    return;
  }
  // Packets for a photo nobody here asked for (another listener's channel
  // transfer) are kept quietly; they count when we do ask.
  if (s.st == kRequesting || s.st == kReceiving) {
    s.st = kReceiving;
    if (left >= 0) {
      // A marked packet: `left` more of the answer to come. None: it's over and
      // the gaps are final. Else the rest should take that many gaps.
      const uint32_t gap = s.rx_gaps ? s.rx_gap_ms : burstGap(s);
      arm(s, markedStallWait(left, gap));
      if (left == 0)
        logf("[VOX] fetch %08lx: answer over (marked), %d missing", (unsigned long)s.sid,
             s.asm_.total() - s.asm_.received());
    } else if (index >= 0 && index == s.expected_last) {
      // The sender's burst is over, so the gaps are final: ask again soon.
      arm(s, gapRequestDelay(s.relayed, burstGap(s), zeroHopGap(airMs(s))));
    } else {
      arm(s, waitFor(s));
    }
  } else if (s.st == kRemote && s.channel >= 0) {
    // Pushed, or sent for another listener: if it stops short, ask for the
    // rest. Everyone in range hears the same push stop, so spread out.
    const uint32_t air = airMs(s);
    const uint32_t gap = s.relayed ? packetGap(true, 0, air) : zeroHopGap(air);
    arm(s, stallWait(gap) + io_.random32() % 1500);
  }
  io_.changed(s.sid);
}

void VoxFetch::complete(Session& s) {
  s.timer_on = false;
  if (io_.savePayload(s.sid, s.asm_.payload(), s.asm_.payloadLen())) {
    s.st = kComplete;
    s.note = nullptr;
  } else {
    s.st = kUnavailable;
    s.note = "couldn't store it. Tap to retry.";
  }
  free(s.storage);
  s.storage = nullptr;
  free(s.parity);
  s.parity = nullptr;
  s.n_parity = 0;
  s.asm_ = VoxWire::Assembly{};
  io_.changed(s.sid);
}

// --------------------------------------------------------------- status

VoxFetch::Status VoxFetch::status(uint32_t sid, int* received, int* total) const {
  const Session* s = find(sid);
  if (received) *received = 0;
  if (total) *total = 0;
  if (!s) return io_.hasPayload(sid) ? kComplete : kUnknown;
  if (total) *total = s->total;
  if (received) *received = s->st == kComplete ? s->total : (s->storage ? s->asm_.received() : 0);
  return s->st;
}

VoxFetch::Method VoxFetch::lastMethod(uint32_t sid) const {
  const Session* s = find(sid);
  return s ? s->last : kNone;
}

int VoxFetch::attempts(uint32_t sid) const {
  const Session* s = find(sid);
  return s ? s->attempts : 0;
}

bool VoxFetch::firstPackets(uint32_t sid, const uint8_t** data, size_t* len) const {
  const Session* s = find(sid);
  if (!s || s->st == kComplete || !s->storage || !s->asm_.total() || !s->asm_.have(0)) return false;
  int k = 0;
  while (k < VoxWire::kThumbPackets && k < s->asm_.total() && s->asm_.have((uint8_t)k)) k++;
  *data = s->storage;
  // All of it (the last packet is short): complete, so the store has it.
  *len = (size_t)k * (size_t)s->packet_bytes;
  return true;
}

uint8_t VoxFetch::via(uint32_t sid) const {
  const Session* s = find(sid);
  if (!s) return 0;
  return (uint8_t)((s->via_count[kViaMain] ? 1 : 0) | (s->via_count[kViaRadio2] ? 2 : 0));
}

bool VoxFetch::radio2Hop(uint32_t sid, uint8_t out[3], uint8_t* out_len) const {
  const Session* s = find(sid);
  if (!s || !s->r2_hop_len || !s->via_count[kViaRadio2]) return false;
  memcpy(out, s->r2_hop, s->r2_hop_len);
  if (out_len) *out_len = s->r2_hop_len;
  return true;
}

const char* VoxFetch::viaText(uint8_t via) {
  switch (via & 3) {
    case 1: return "Received via main mesh";
    case 2: return "Received via MediaMesh";
    case 3: return "Received via MediaMesh and main mesh";
    default: return nullptr;
  }
}

const char* VoxFetch::statusText(uint32_t sid, char* buf, size_t cap) const {
  if (!buf || cap == 0) return "";
  int got = 0, total = 0;
  const Status st = status(sid, &got, &total);
  const Session* s = find(sid);
  const bool voice = s && s->kind != VoxWire::kImage;
  const char* noun = voice ? "Voice" : "Photo";
  char ready[32];
  if (voice) snprintf(ready, sizeof ready, "Voice %us - tap to play", (unsigned)s->aspect);
  switch (st) {
    case kUnknown:
    case kRemote:
      // Pushed packets arriving unasked show as a download in progress.
      if (got > 0) snprintf(buf, cap, "Downloading %d/%d", got, total);
      else snprintf(buf, cap, "%s", voice ? ready : "Photo - tap to download");
      break;
    case kRequesting:
      snprintf(buf, cap, "%s", !s ? "Requesting..."
                             : s->last == kDm      ? "Finding a route to the sender..."
                             : s->last == kChannel ? "Requesting on the channel..."
                                                   : "Requesting from the sender...");
      break;
    case kReceiving:
      snprintf(buf, cap, "Downloading %d/%d", got, total);
      break;
    case kComplete:
      snprintf(buf, cap, "%s", voice ? ready : "Photo - tap to view");
      break;
    case kUnavailable:
      snprintf(buf, cap, "%s %s", noun, s && s->note ? s->note : "unavailable. Tap to retry.");
      break;
  }
  return buf;
}

void VoxFetch::logf(const char* fmt, ...) {
  char line[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  io_.log(line);
}

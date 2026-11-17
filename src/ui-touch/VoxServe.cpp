// SPDX-License-Identifier: GPL-3.0-or-later
#include "VoxServe.h"

#include "VoxFec.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace {

uint32_t fnv(uint32_t h, const void* p, size_t n) {
  const uint8_t* b = static_cast<const uint8_t*>(p);
  for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 16777619u;
  return h;
}

bool after(uint32_t now, uint32_t t) { return (int32_t)(now - t) >= 0; }

}  // namespace

VoxServe::~VoxServe() {
  for (Job& j : jobs_)
    if (j.bits) io_.freeClip(j.bits);
}

void VoxServe::logf(const char* fmt, ...) {
  char line[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  io_.log(line);
}

void VoxServe::markOwn(uint32_t sid) {
  if (isOwn(sid)) return;
  own_[own_next_] = sid;
  own_next_ = (own_next_ + 1) % kMaxOwn;
}

bool VoxServe::isOwn(uint32_t sid) const {
  if (!sid) return false;
  for (uint32_t s : own_) if (s == sid) return true;
  return false;
}

uint32_t VoxServe::listenerKey(uint32_t sid, const uint8_t key6[6], int channel) const {
  uint32_t h = fnv(2166136261u, &sid, sizeof sid);
  if (channel >= 0) {
    const uint8_t tag = 0xC0;
    h = fnv(h, &tag, 1);
    return fnv(h, &channel, sizeof channel);
  }
  return fnv(h, key6, 6);
}

uint32_t VoxServe::signature(const Job& j) const {
  uint32_t h = listenerKey(j.sid, j.key6, j.channel);
  h = fnv(h, j.key6, 6);   // on a channel too: another listener is not a repeat
  const uint8_t all = j.all ? 1 : 0;
  h = fnv(h, &all, 1);
  return fnv(h, j.want, (size_t)j.n_want);
}

uint32_t VoxServe::bumpGen(uint32_t key) {
  for (Gen& g : gens_)
    if (g.key == key && g.gen) return ++g.gen;
  gens_[gens_next_] = Gen{key, 1};
  gens_next_ = (gens_next_ + 1) % (int)(sizeof gens_ / sizeof gens_[0]);
  return 1;
}

uint32_t VoxServe::currentGen(uint32_t key) const {
  for (const Gen& g : gens_)
    if (g.key == key && g.gen) return g.gen;
  return 0;
}

bool VoxServe::heardSince(uint32_t sid, int index, uint32_t t) const {
  for (const Heard& h : heard_)
    if (h.at && h.sid == sid && h.index == index && (int32_t)(h.at - t) >= 0) return true;
  return false;
}

void VoxServe::onPayload(const uint8_t* data, size_t len, int channel, int hops) {
  VoxWire::Packet p;
  if (VoxWire::parsePacket(data, len, &p)) {   // another node sending: note it
    const uint32_t now = io_.nowMs();
    heard_[heard_next_] = Heard{p.sid, p.index, now ? now : 1};
    heard_next_ = (heard_next_ + 1) % (int)(sizeof heard_ / sizeof heard_[0]);
    return;
  }
  VoxWire::FetchRequest r;
  if (!VoxWire::parseFetch(data, len, &r)) return;
  uint8_t self[6];
  io_.selfKey6(self);
  if (memcmp(self, r.key6, 6) == 0) return;   // our own request, heard back from a repeater
  // On a channel the clip's author answers; anyone else holding it (the app's
  // 5-8 s stand-in) is left to the apps, so a radio and the phone connected to
  // it never both answer.
  if (channel >= 0 && !isOwn(r.sid)) return;
  enqueue(r, channel, false, hops);
}

int VoxServe::nextPending(const Job& j) {
  for (int i = 0; i < j.total && i < 256; i++)
    if (j.pend[i >> 3] & (1u << (i & 7))) return i;
  return -1;
}

// A channel request for a clip already queued or being sent on that channel:
// add what it lacks to that job rather than starting over (each listener's
// request used to replace the burst and begin again from packet 1).
bool VoxServe::mergeChannel(const VoxWire::FetchRequest& r, int channel, int hops) {
  for (int i = 0; i < kMaxQueued; i++) {
    Job& q = jobs_[i];
    if (!q.used || q.sid != r.sid || q.channel != channel) continue;
    const int h = hops < 0 ? 3 : hops;
    if (h > q.max_hops) q.max_hops = h;
    q.mark_ok = q.mark_ok && r.mark_ok;   // everyone it answers must read marked packets
    if (q.bits) {   // started: mark the packets it lacks
      for (int k = 0; k < q.total; k++) {
        bool want = r.all;
        for (int m = 0; !want && m < r.n_missing; m++) want = r.missing[m] == k;
        if (want) q.pend[k >> 3] |= (uint8_t)(1u << (k & 7));
      }
    } else if (r.all) {
      q.all = true;
    } else if (!q.all) {
      for (int m = 0; m < r.n_missing; m++) {
        bool have = false;
        for (int k = 0; k < q.n_want && !have; k++) have = q.want[k] == r.missing[m];
        if (!have && q.n_want < VoxWire::kMaxPackets) q.want[q.n_want++] = r.missing[m];
      }
    }
    logf("[VOX] serve %08lx: another listener (%d hops) added to the burst", (unsigned long)r.sid, h);
    return true;
  }
  return false;
}

bool VoxServe::onDirectText(const char* text) {
  if (!VoxWire::isControlText(text)) return false;
  VoxWire::FetchRequest r;
  if (!VoxWire::parseTextFetch(text, &r)) {
    logf("[VOX] VR3 DM could not be parsed");
    return true;
  }
  uint8_t self[6];
  io_.selfKey6(self);
  if (memcmp(self, r.key6, 6) != 0) enqueue(r, -1, true);
  return true;
}

void VoxServe::enqueue(const VoxWire::FetchRequest& r, int channel, bool from_dm, int hops) {
  const uint32_t now = io_.nowMs();
  Job t;
  t.used = true;
  t.sid = r.sid;
  memcpy(t.key6, r.key6, 6);
  t.channel = channel;
  t.all = r.all;
  t.n_want = r.all ? 0 : r.n_missing;
  if (t.n_want) memcpy(t.want, r.missing, (size_t)t.n_want);
  t.salt_ok = r.salt_ok;
  t.mark_ok = channel >= 0 && r.mark_ok;   // marked packets go on a channel only
  if (channel < 0 && r.has_path) {
    // The way the request just reached us, reversed: known to work right now.
    t.have_route = true;
    t.route_len = r.path_len;
    for (int i = 0; i < r.path_len; i++) t.route[i] = r.path[r.path_len - 1 - i];
  }
  t.asked_at = now ? now : 1;
  t.zero_hop_req = channel < 0 && r.has_path && r.path_len == 0;
  // On a channel, gather briefly: listeners who tap the same clip almost
  // together share one burst; a later tap gets its own. Short, as in the app:
  // the one asking is watching a spinner.
  // A zero-hop raw request reached everyone in range: the author goes first.
  if (channel >= 0 && !r.all) {   // a report on our last answer for this clip
    for (AnswerRec& a : answers_) {
      if (a.sid != r.sid || a.channel != channel) continue;
      uint8_t miss[32] = {0};
      for (int m = 0; m < r.n_missing; m++) miss[r.missing[m] >> 3] |= (uint8_t)(1u << (r.missing[m] & 7));
      paceFor(channel).feedback(a.answer, miss);
    }
  }
  // On a channel, not before the request's own flood has died down near us: its
  // relays (hidden from us, heard by our next repeater) took out the first
  // packets of every answer to a listener 4 hops away (#177 cut the wait to
  // 0.5-1 s). FloodPace: a relay per hop it came, learnt up or down from there.
  t.not_before = channel >= 0 ? now + kOwnChannelDelayMs + io_.random32() % 250 +
                                    paceFor(channel).settleMs(hops < 0 ? 3 : hops)
               : t.zero_hop_req && !isOwn(r.sid) ? now + kZeroHopWaitMs + io_.random32() % 2000
               : now;
  t.route_deadline = now + (from_dm ? kRouteWaitDmMs : kRouteWaitMs);

  // The same request again (heard twice, or a quick retry): ignore it.
  const uint32_t sig = signature(t);
  for (const Recent& e : recent_) {
    if (e.sig == sig && e.at && !after(now, e.at + kDupWindowMs)) {
      logf("[VOX] serve %08lx: ignoring a repeated request", (unsigned long)r.sid);
      return;
    }
  }
  recent_[recent_next_] = Recent{sig, now ? now : 1};
  recent_next_ = (recent_next_ + 1) % (int)(sizeof recent_ / sizeof recent_[0]);
  if (channel >= 0) {
    t.max_hops = hops < 0 ? 3 : hops;
    if (mergeChannel(r, channel, hops)) return;
  }

  // A newer request from the same listener replaces anything still queued or
  // in progress for it, so we never keep sending stale packets.
  const uint32_t lk = listenerKey(t.sid, t.key6, t.channel);
  t.gen = bumpGen(lk);
  for (int i = 0; i < kMaxQueued; i++) {
    Job& q = jobs_[i];
    if (!q.used || i == active_) continue;
    if (listenerKey(q.sid, q.key6, q.channel) == lk) {
      q.used = false;
      queued_--;
    }
  }

  int slot = -1;
  for (int i = 0; i < kMaxQueued; i++)
    if (!jobs_[i].used && i != active_) { slot = i; break; }
  if (slot < 0) {
    logf("[VOX] serve %08lx: too many requests queued, dropping this one", (unsigned long)r.sid);
    return;
  }
  t.seq = ++seq_;
  jobs_[slot] = t;
  queued_++;
  char key[13];
  for (int i = 0; i < 6; i++) snprintf(key + 2 * i, 3, "%02x", t.key6[i]);
  logf("[VOX] serve %08lx: %s request from %s for %s%s", (unsigned long)t.sid,
       channel >= 0 ? "channel" : from_dm ? "VR3 DM" : "raw", key, t.all ? "all packets" : "missing packets",
       t.have_route ? " (reply along its path)" : "");
}

int VoxServe::packetCount(const Job& j) const {
  if (j.channel >= 0 && j.bits) {
    int n = 0;
    for (int i = 0; i < j.total && i < 256; i++) n += (j.pend[i >> 3] >> (i & 7)) & 1;
    return n;
  }
  if (j.all) return j.total;
  int n = 0;
  for (int i = 0; i < j.n_want; i++) if (j.want[i] < j.total) n++;
  return n;
}

int VoxServe::packetIndex(const Job& j, int k) const {
  if (j.all) return k;
  int n = 0;
  for (int i = 0; i < j.n_want; i++) {
    if (j.want[i] >= j.total) continue;
    if (n == k) return j.want[i];
    n++;
  }
  return -1;
}

// Pause between packets so we don't talk over our own relays: a stock repeater
// waits a random 0..1.5 airtimes before relaying, then a full airtime relaying,
// maybe at a slower coding rate (the 1.15). Same as VoxVoiceService._packetGap.
// Zero-hop: nobody relays, so the next packet follows its own airtime, plus a
// little for the radio to turn round and the listeners to read it out.
uint32_t VoxServe::zeroHopGap() {
  const uint32_t g = io_.packetAirMs() + 60;
  return g > 150 ? g : 150;
}

floodpace::Pace& VoxServe::paceFor(int channel) {
  const uint32_t air = io_.packetAirMs();
  for (PaceRec& p : paces_)
    if (p.used && p.channel == channel) { p.pace.air = air; return p.pace; }
  PaceRec& p = paces_[paces_next_];
  paces_next_ = (paces_next_ + 1) % (int)(sizeof paces_ / sizeof paces_[0]);
  p.used = true;
  p.channel = channel;
  p.pace = floodpace::Pace(air);
  return p.pace;
}

uint32_t VoxServe::packetGap(bool flood, int hops) {
  const uint32_t air = io_.packetAirMs();
  if (flood) return floodpace::baseGapMs(air);
  if (hops <= 0) return air * 2 + 150 > 400 ? air * 2 + 150 : 400;
  const int h = hops < 2 ? hops : 2;
  return (uint32_t)(air + h * 2.5 * (air * 1.15) + 400 + 0.5);
}

bool VoxServe::start(Job& j) {
  if (!io_.loadClip(j.sid, &j.mode, &j.bits, &j.len) || !j.bits) {
    j.bits = nullptr;
    finish(j, "asked for a clip we do not have");
    return false;
  }
  const int pb = VoxWire::packetBytes(j.mode);
  if (pb <= 0 || j.len == 0) {
    finish(j, "stored clip is unreadable");
    return false;
  }
  j.total = (int)((j.len + (size_t)pb - 1) / (size_t)pb);
  if (j.total > VoxWire::kMaxPackets) j.total = VoxWire::kMaxPackets;
  if (j.channel >= 0) {
    memset(j.pend, 0, sizeof j.pend);
    for (int k = 0; k < j.total; k++) {
      bool want = j.all;
      for (int m = 0; !want && m < j.n_want; m++) want = j.want[m] == k;
      if (want) j.pend[k >> 3] |= (uint8_t)(1u << (k & 7));
    }
  }
  if (packetCount(j) == 0) {
    finish(j, "nothing it asked for exists");
    return false;
  }
  if (j.all) j.par_total = VoxFec::totalParity(j.total, j.mode == VoxWire::kImageModeId);
  return true;
}

// The next parity packet of a whole-clip answer. False when there are none left.
bool VoxServe::sendParity(Job& j, uint32_t now) {
  if (j.par_next >= j.par_total) return false;
  const int pb = VoxWire::packetBytes(j.mode);
  int b = 0, k = 0;
  if (!VoxFec::parityAt(j.total, j.mode == VoxWire::kImageModeId, j.par_next, &b, &k)) return false;
  // encodeParity reads whole packets: the last one is short in j.bits, so
  // the stride stays pb and its length is passed.
  const int last_len = (int)(j.len - (size_t)(j.total - 1) * (size_t)pb);
  uint8_t pkt[VoxFec::kMaxParityPacket];
  const int n = VoxFec::encodeParity(j.sid, j.bits, j.total, pb, last_len, b, k, pkt, sizeof pkt);
  const bool zero_hop = j.channel >= 0 && j.max_hops == 0;
  const bool ok = n > 0 && (j.channel >= 0 ? io_.sendChannelData(j.channel, VoxWire::kChannelDataType, pkt, (size_t)n, zero_hop)
                                           : io_.sendRaw(j.route, j.route_len, pkt, (size_t)n));
  if (!ok) {
    if (++j.send_failures > 8) { j.par_next = j.par_total; return false; }
    j.next_at = now + 250;
    return true;
  }
  j.send_failures = 0;
  sent_total_++;
  j.par_next++;
  j.next_at = now + j.gap;
  return true;
}

void VoxServe::finish(Job& j, const char* why) {
  logf("[VOX] serve %08lx: %s (%d/%d sent)", (unsigned long)j.sid, why, j.next, j.bits ? packetCount(j) : 0);
  if (j.first_sent_ms && j.asked_at) {   // where the time went
    const uint32_t now = io_.nowMs();
    logf("[VOX] serve %08lx: first packet %lu ms after the request, all of it in %lu ms, %lu ms apart%s",
         (unsigned long)j.sid, (unsigned long)(j.first_sent_ms - j.asked_at), (unsigned long)(now - j.asked_at),
         (unsigned long)j.gap, j.mark_ok && j.channel >= 0 ? ", marked" : "");
  }
  if (j.channel >= 0 && j.answer.n > 0) {   // what listeners still lack after it tells the pace how it did
    AnswerRec& a = answers_[answers_next_];
    answers_next_ = (answers_next_ + 1) % (int)(sizeof answers_ / sizeof answers_[0]);
    for (AnswerRec& o : answers_) if (o.sid == j.sid && o.channel == j.channel) o.sid = 0;
    a.sid = j.sid;
    a.channel = j.channel;
    a.answer = j.answer;
    const floodpace::Pace& p = paceFor(j.channel);
    logf("[VOX] serve %08lx: pace %lu ms apart, %lu ms first", (unsigned long)j.sid,
         (unsigned long)p.gapMs(j.max_hops), (unsigned long)p.settleMs(j.max_hops));
  }
  if (j.bits) io_.freeClip(j.bits);
  j = Job();
  active_ = -1;
}

void VoxServe::tick() {
  const uint32_t now = io_.nowMs();
  if (active_ < 0) {
    int pick = -1;
    for (int i = 0; i < kMaxQueued; i++)
      if (jobs_[i].used && (pick < 0 || jobs_[i].seq < jobs_[pick].seq)) pick = i;
    if (pick < 0 || !after(now, jobs_[pick].not_before)) return;
    active_ = pick;
    queued_--;
  }
  Job& j = jobs_[active_];
  if (currentGen(listenerKey(j.sid, j.key6, j.channel)) != j.gen) {
    finish(j, "stopped: a newer request replaced this one");
    return;
  }
  if (!j.bits && !start(j)) return;

  // Direct answers need a route back: the request's own path, or our route to
  // the requester (which a VR3 flood teaches both radios a few seconds later).
  if (j.channel < 0 && !j.have_route) {
    if (!after(now, j.route_try_at)) return;
    j.route_try_at = now + 500;
    int len = 0;
    if (io_.routeToKey6(j.key6, j.route, &len) && len >= 0 && len <= (int)sizeof j.route) {
      j.have_route = true;
      j.route_len = len;
    } else if (after(now, j.route_deadline)) {
      finish(j, "no direct route back to the requester");
      return;
    } else {
      return;
    }
  }
  if (!j.gap) {
    j.gap = j.channel >= 0 ? (j.max_hops == 0 ? zeroHopGap() : paceFor(j.channel).gapMs(j.max_hops)) : packetGap(false, j.route_len);
    j.next_at = now;
    logf("[VOX] serve %08lx: sending %d packets %s, %lu ms apart", (unsigned long)j.sid, packetCount(j),
         j.channel < 0 ? "direct" : j.max_hops == 0 ? "zero-hop on the channel" : "on the channel",
         (unsigned long)j.gap);
  }
  if (!after(now, j.next_at)) return;

  int idx = j.channel >= 0 ? nextPending(j) : j.next < packetCount(j) ? packetIndex(j, j.next) : -1;
  // Not the author: leave out what another node sent since the request.
  while (idx >= 0 && !isOwn(j.sid) && heardSince(j.sid, idx, j.asked_at)) {
    if (j.channel >= 0) {
      j.pend[idx >> 3] &= (uint8_t)~(1u << (idx & 7));
      idx = nextPending(j);
    } else {
      j.next++;
      idx = j.next < packetCount(j) ? packetIndex(j, j.next) : -1;
    }
  }
  if (idx < 0) {
    // The data's done: the parity of a whole-clip answer follows.
    if (sendParity(j, now)) return;
    finish(j, "served");
    return;
  }
  const bool zero_hop = j.channel >= 0 && j.max_hops == 0;
  const int pb = VoxWire::packetBytes(j.mode);
  const size_t off = (size_t)idx * (size_t)pb;
  const int n = (int)(j.len - off < (size_t)pb ? j.len - off : (size_t)pb);
  uint8_t pkt[VoxWire::kPacketHeaderLen + VoxWire::kBytesPerPacket + 1];
  // Salt each send so a resend is never dropped as a duplicate by a radio that
  // saw an earlier copy (channel data must still fit GRP_DATA's 163 bytes).
  const bool salt = j.salt_ok && (j.channel < 0 || VoxWire::kPacketHeaderLen + n + 1 <= 163);
  // Marked (0x58) on a channel when everyone asking reads it: how many of this
  // answer are still to come, so they know when it's over. The salt then counts
  // this packet's sends, so a resend differs from copies repeaters have seen.
  const bool mark = salt && j.channel >= 0 && j.mark_ok;
  int left = -1;
  if (mark) {
    left = j.par_total - j.par_next;
    for (int i = 0; i < j.total && i < 256; i++)
      if (i != idx && (j.pend[i >> 3] & (1u << (i & 7)))) left++;
    if (mark_sid_ != j.sid) { memset(mark_sends_, 0, sizeof mark_sends_); mark_sid_ = j.sid; }
  }
  const int plen = VoxWire::encodePacket(j.sid, (uint8_t)idx, j.bits + off, n,
                                         !salt ? -1 : mark ? (int)(mark_sends_[idx]++) : (int)(io_.random32() & 0xFF),
                                         pkt, sizeof pkt, left);
  if (!j.first_sent_ms) j.first_sent_ms = now ? now : 1;
  const bool ok = plen > 0 && (j.channel >= 0 ? io_.sendChannelData(j.channel, VoxWire::kChannelDataType, pkt, (size_t)plen, zero_hop)
                                              : io_.sendRaw(j.route, j.route_len, pkt, (size_t)plen));
  if (!ok) {
    // The mesh's packet pool can be momentarily full; try again shortly.
    if (++j.send_failures > 8) { finish(j, "the radio wouldn't take the packets"); return; }
    j.next_at = now + 250;
    return;
  }
  // Failures count in a row: a long answer isn't ended by a few scattered
  // moments of a full packet pool (as the app's voxSlowConfirmsMax).
  j.send_failures = 0;
  sent_total_++;
  j.next++;
  if (j.channel >= 0 && !zero_hop) j.answer.add(idx);
  if (j.channel >= 0) {
    j.pend[idx >> 3] &= (uint8_t)~(1u << (idx & 7));
    // A farther listener may have joined: flood from here on, spaced for relays.
    j.gap = j.max_hops == 0 ? zeroHopGap() : paceFor(j.channel).gapMs(j.max_hops);
    j.next_at = now + j.gap;
    if (nextPending(j) < 0 && j.par_next >= j.par_total) finish(j, "served");
    return;
  }
  j.next_at = now + j.gap;
  if (j.next >= packetCount(j) && j.par_next >= j.par_total) finish(j, "served");
}

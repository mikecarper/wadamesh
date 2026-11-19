// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// How an answer on a channel (photo or voice packets flooded to whoever asked)
// is paced, learnt from what the requests after it still lack: fast where the
// mesh is clear, slower where it isn't, for a listener 0 hops away or 20.
// Mirrors VoxFloodPace in jade-on-mesh/meshcore-open-tak (lib/vox/vox_timing.dart)
// number for number; test/test_vox_serve.cpp checks the same cases.
//
// - The gap between packets starts at the base gap (4 airtimes + 150 ms, at
//   least 400 ms; for a listener 2+ hops away at least the average time for
//   our packet to clear the next two repeaters), grows by half when an answer
//   lost a third or more, shrinks by a fifth when it lost almost none, between
//   the base gap and twice it.
// - Before answering: a relay's time per hop the request came, up to two (its
//   own flood is still being relayed around us, by repeaters we may not hear
//   but our next hop does). An answer whose first packet was lost while later
//   ones came adds a relay, up to four; one whose first packet came takes one off.
// - Many listeners may report on one answer. One who got nearly all of it
//   speeds the pace up (undoing a slowing that answer caused), and after it no
//   report slows it: a pace that's too fast loses packets for everyone, a poor
//   link only for its listener. Each answer moves the pace at most once each way.

#include <stdint.h>
#include <string.h>

namespace floodpace {

inline uint32_t relayMs(uint32_t air) { return (uint32_t)(air + 2.5 * air * 1.15 + 0.5); }
// At least 400 ms (it was 800, from the main mesh's slow packets).
inline uint32_t baseGapMs(uint32_t air) { return air * 4 + 150 > 400 ? air * 4 + 150 : 400; }
// The longest wait before answering a request that came `hops` repeaters.
inline uint32_t settleMaxMs(uint32_t air, int hops) { return hops <= 0 ? 0 : 4 * relayMs(air); }
inline uint32_t slowGapMs(uint32_t air) { return 2 * baseGapMs(air); }
// Fast MediaMesh: our airtime, one repeater's average relay wait
// and its relay (about 2.4 airtimes) and a little, as the app's
// voxFloodFastBaseGapMs.
inline uint32_t fastBaseGapMs(uint32_t air) {
  const uint32_t g = (uint32_t)(air * 2.4 + 0.5) + 150;
  return g > 400 ? g : 400;
}

// Adaptive spacing: the tightest an answer's gap may learn (1.6 airtimes and a
// little, at least 300 ms). As voxFloodAdaptiveFloorMs.
inline uint32_t adaptiveFloorMs(uint32_t air) {
  const uint32_t g = (uint32_t)(air * 1.6 + 0.5) + 150;
  return g > 300 ? g : 300;
}
// After an answer lost packets because of the spacing, the gap stays 10% above
// it this long.
constexpr uint32_t kAdaptiveHoldMs = 30u * 60u * 1000u;
// A listener's loss rising this many points over what it lost at a wider gap
// is the spacing's doing.
constexpr int kAdaptiveRisePct = 7;

// One answer: the data packets it sent (bit i: packet i), the first of them,
// and what reports on it have done. gap: the gap it went at (adaptive
// spacing).
struct Answer {
  uint8_t sent[32];
  int n;
  int first;
  bool clean, slowed, first_came, settle_up, loss_seen;
  uint32_t gap;
  bool adapt_down, adapt_up;
  Answer()
      : n(0), first(-1), clean(false), slowed(false), first_came(false), settle_up(false), loss_seen(false),
        gap(0), adapt_down(false), adapt_up(false) {
    memset(sent, 0, sizeof sent);
  }
  void add(int i) {
    if (i < 0 || i >= 256 || (sent[i >> 3] & (1u << (i & 7)))) return;
    sent[i >> 3] |= (uint8_t)(1u << (i & 7));
    if (first < 0) first = i;
    n++;
  }
  bool has(int i) const { return i >= 0 && i < 256 && (sent[i >> 3] & (1u << (i & 7))); }
};

struct Pace {
  uint32_t air;
  uint32_t gap;
  int extra;   // relays of waiting learnt, 0..2
  // Fast MediaMesh: start at, and learn down to, fastBaseGapMs.
  bool fast;
  // Fast MediaMesh: the share of an answer's packets (percent, 0-40) the
  // worst-placed listener reporting on it lost lately; repair packets are sized
  // for it (VoxFountain::passShare). More loss raises it at once; the first
  // report on each answer that saw less brings it a quarter down. As the app's
  // VoxFloodPace.lossPct.
  int loss_pct;
  // Adaptive spacing (with fast): one learnt gap, from the fast base gap down
  // to adaptiveFloorMs, moved by each listener's own losses, not hop counts (a
  // listener next door can be 3 hops away in a dense mesh). Under 3% lost:
  // down a tenth (once per answer, not if the spacing already cost a listener
  // on it). kAdaptiveRisePct points more than that listener lost at its widest
  // recent gap: up a quarter (by half at a third), held 10% above the gap that
  // lost for kAdaptiveHoldMs. As lossy at a wider gap: its own link, no move.
  // Not heard from before and 10% lost: up a quarter, with no hold. As the app's
  // VoxFloodPace.adaptive. `now` is set by the caller (VoxServe::paceFor).
  // Not kept across reboots.
  bool adaptive = false;
  uint32_t now = 0;
  struct Listener { uint8_t key[6]; uint32_t gap; int pct; bool used; };
  mutable uint32_t ad_air = 0xFFFFFFFFu, ad_gap = 0;
  uint32_t ad_failed = 0, ad_hold_until = 0;
  bool ad_held = false;
  mutable Listener listeners[8] = {};
  int listener_next = 0;

  explicit Pace(uint32_t air_ms = 0, bool fast_ = false)
      : air(air_ms), gap(fast_ ? fastBaseGapMs(air_ms) : baseGapMs(air_ms)), extra(0), fast(fast_), loss_pct(0) {}

  uint32_t base() const { return fast ? fastBaseGapMs(air) : baseGapMs(air); }
  bool adaptiveOn() const { return fast && adaptive; }
  void adaptiveInit() const {
    if (ad_air == air) return;
    ad_air = air;
    ad_gap = base();
    for (Listener& l : listeners) l.used = false;
  }
  bool holding() const { return ad_held && (int32_t)(now - ad_hold_until) < 0; }
  uint32_t adFloor() const {
    adaptiveInit();
    uint32_t f = adaptiveFloorMs(air);
    if (holding() && ad_failed * 11 / 10 > f) f = ad_failed * 11 / 10;
    return f < slowGapMs(air) ? f : slowGapMs(air);
  }
  uint32_t fastGapMs(int hops) const {
    const uint32_t b = base();
    if (hops < 2 || fast) return b;
    const uint32_t f = (uint32_t)(air * (3 + 2 * 1.25 * 1.15) + 0.5);
    return f > b ? f : b;
  }
  uint32_t gapMs(int hops) const {
    if (adaptiveOn()) {
      adaptiveInit();
      const uint32_t lo = adFloor(), hi = slowGapMs(air);
      return ad_gap < lo ? lo : ad_gap > hi ? hi : ad_gap;
    }
    const uint32_t lo = fastGapMs(hops), hi = slowGapMs(air);
    return gap < lo ? lo : gap > hi ? hi : gap;
  }
  const char* adaptiveNote() const { return adaptiveOn() ? " (adaptive)" : ""; }
  // Adaptive spacing: `lost` of `a`'s packets were missing for listener `key`
  // (nullptr: not known, so no baseline). True when the gap moved.
  bool adapt(Answer& a, int lost, const uint8_t* key) {
    adaptiveInit();
    const int pct = lost * 100 / a.n;
    const uint32_t at = a.gap ? a.gap : ad_gap;
    Listener* base = nullptr;
    for (Listener& l : listeners)
      if (key && l.used && memcmp(l.key, key, 6) == 0) base = &l;
    const bool base_known = base != nullptr;
    bool spacing;
    if (!base) spacing = pct >= 10;
    else if (at < base->gap) spacing = pct - base->pct >= kAdaptiveRisePct && pct >= 3;
    else spacing = false;   // as wide as its baseline: a poor link if it loses here too
    if (key && (!base || at >= base->gap)) {
      if (!base) { base = &listeners[listener_next]; listener_next = (listener_next + 1) % 8; }
      memcpy(base->key, key, 6);
      base->gap = at;
      base->pct = pct;
      base->used = true;
    }
    if (spacing) {
      if (a.adapt_up) return false;
      a.adapt_up = true;
      if (base_known) {   // proven the spacing's: hold above it (a first report proves nothing)
        ad_failed = holding() && ad_failed > at ? ad_failed : at;
        ad_hold_until = now + kAdaptiveHoldMs;
        ad_held = true;
      }
      uint32_t up = pct * 3 >= 100 ? ad_gap * 3 / 2 : ad_gap * 5 / 4;
      const uint32_t lo = adFloor();
      if (up < lo) up = lo;
      ad_gap = up < slowGapMs(air) ? up : slowGapMs(air);
      return true;
    }
    if (pct < 3 && !a.adapt_down && !a.adapt_up) {
      a.adapt_down = true;
      uint32_t down = ad_gap * 9 / 10;
      const uint32_t lo = adFloor();
      if (down < lo) down = lo;
      if (down == ad_gap) return false;
      ad_gap = down;
      return true;
    }
    return false;
  }

  uint32_t settleMs(int hops) const {
    if (hops <= 0) return 0;
    int r = (hops < 2 ? hops : 2) + extra;
    if (r < 0) r = 0;
    if (r > 4) r = 4;
    return (uint32_t)r * relayMs(air);
  }
  // A listener's request after `a`: `missing` (bit i) is what it still lacks;
  // `key` is the listener's (adaptive spacing). True when the adaptive gap moved.
  bool feedback(Answer& a, const uint8_t missing[32], const uint8_t* key = nullptr) {
    if (a.n <= 0) return false;
    auto miss = [&](int i) { return (missing[i >> 3] & (1u << (i & 7))) != 0; };
    int lost = 0;
    for (int i = 0; i < 256; i++) if (a.has(i) && miss(i)) lost++;
    const bool moved = adaptiveOn() && adapt(a, lost, key);
    int pct = lost * 100 / a.n;
    if (pct > 40) pct = 40;
    if (pct > loss_pct) loss_pct = pct;
    else if (!a.loss_seen) loss_pct = (loss_pct * 3 + pct) / 4;
    a.loss_seen = true;
    if (lost * 10 < a.n) {
      if (!a.clean) {
        a.clean = true;
        if (a.slowed) gap = gap * 2 / 3;
        const uint32_t g = gap * 4 / 5;
        gap = g > base() ? g : base();
      }
    } else if (lost * 3 >= a.n && !a.clean && !a.slowed) {
      a.slowed = true;
      const uint32_t g = gap * 3 / 2;
      gap = g < slowGapMs(air) ? g : slowGapMs(air);
    }
    if (a.n < 2) return moved;
    const bool first_lost = miss(a.first);
    if (!first_lost) {
      if (!a.first_came) {
        a.first_came = true;
        if (a.settle_up) extra--;
        extra = extra > 0 ? extra - 1 : 0;
      }
    } else if (!a.first_came && !a.settle_up) {
      bool later = false;
      for (int i = 0; i < 256 && !later; i++) later = i != a.first && a.has(i) && !miss(i);
      if (later) {
        a.settle_up = true;
        extra = extra < 2 ? extra + 1 : 2;
      }
    }
    return moved;
  }
};

}  // namespace floodpace

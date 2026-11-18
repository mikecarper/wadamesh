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
// Fast MediaMesh (prototype): our airtime, one repeater's average relay wait
// and its relay (about 2.4 airtimes) and a little, as the app's
// voxFloodFastBaseGapMs.
inline uint32_t fastBaseGapMs(uint32_t air) {
  const uint32_t g = (uint32_t)(air * 2.4 + 0.5) + 150;
  return g > 400 ? g : 400;
}

// One answer: the data packets it sent (bit i: packet i), the first of them,
// and what reports on it have done.
struct Answer {
  uint8_t sent[32];
  int n;
  int first;
  bool clean, slowed, first_came, settle_up;
  Answer() : n(0), first(-1), clean(false), slowed(false), first_came(false), settle_up(false) {
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
  // Fast MediaMesh (prototype): start at, and learn down to, fastBaseGapMs.
  bool fast;
  explicit Pace(uint32_t air_ms = 0, bool fast_ = false)
      : air(air_ms), gap(fast_ ? fastBaseGapMs(air_ms) : baseGapMs(air_ms)), extra(0), fast(fast_) {}

  uint32_t base() const { return fast ? fastBaseGapMs(air) : baseGapMs(air); }
  uint32_t fastGapMs(int hops) const {
    const uint32_t b = base();
    if (hops < 2 || fast) return b;
    const uint32_t f = (uint32_t)(air * (3 + 2 * 1.25 * 1.15) + 0.5);
    return f > b ? f : b;
  }
  uint32_t gapMs(int hops) const {
    const uint32_t lo = fastGapMs(hops), hi = slowGapMs(air);
    return gap < lo ? lo : gap > hi ? hi : gap;
  }
  uint32_t settleMs(int hops) const {
    if (hops <= 0) return 0;
    int r = (hops < 2 ? hops : 2) + extra;
    if (r < 0) r = 0;
    if (r > 4) r = 4;
    return (uint32_t)r * relayMs(air);
  }
  // A listener's request after `a`: `missing` (bit i) is what it still lacks.
  void feedback(Answer& a, const uint8_t missing[32]) {
    if (a.n <= 0) return;
    auto miss = [&](int i) { return (missing[i >> 3] & (1u << (i & 7))) != 0; };
    int lost = 0;
    for (int i = 0; i < 256; i++) if (a.has(i) && miss(i)) lost++;
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
    if (a.n < 2) return;
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
  }
};

}  // namespace floodpace

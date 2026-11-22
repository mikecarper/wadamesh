// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>
namespace bike {
struct Point { int32_t lat, lon; };  // GPS microdegrees
struct Boundary { Point points[9]{}; uint32_t radius = 250; uint8_t count = 0; };
bool valid(const Boundary&);
bool contains(const Boundary&, Point);
enum Side : uint8_t { Unknown, Inside, Outside };
struct Fence {
  Side side = Unknown, candidate = Unknown;
  uint32_t previous = 0;
  bool observe(const Boundary& b, Point p, uint32_t utc) {
    if (!b.count || !utc || utc <= previous) return false;
    const Side next = contains(b,p) ? Inside : Outside;
    const bool confirmed = previous && utc - previous <= 15 && candidate == next;
    previous = utc; candidate = next;
    if (!confirmed || side == next) return false;
    const bool crossing = side != Unknown;
    side = next; return crossing;
  }
};
struct Absence {
  uint32_t last = 0;
  bool lost = false;
  void reset(uint32_t now) { last = now; lost = false; }
  bool update(uint32_t now, bool present, unsigned seconds) {
    if (present) last = now;
    const bool next = !present && now - last >= seconds * 1000UL;
    const bool change = next != lost;
    lost = next; return change;
  }
};
}

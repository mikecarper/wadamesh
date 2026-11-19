// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Rateless repair packets for MediaMesh transfers, as the app
// does them (lib/vox/vox_fountain.dart in jade-on-mesh/meshcore-open-tak; its
// scripts/vox/fountain_ref.py is the reference both are checked against).
//
// A repair packet is a random mix of *all* of a clip's packets over GF(256),
// so a listener missing m packets gets them back from any m (nearly always;
// m + 1 practically always) repair packets, whichever packets it lacks. One
// answer of "N fresh repair packets" mends every listener at once.
//
// Wire: an ordinary VE3 voice packet
//     [0x56][rsid:4][seed][lastLen][repair: P bytes]
// - rsid = the clip's session id XOR 0x0F0DEC0D (not parity's 0xFEC0FEC0).
// - seed 0..254 picks the mix: repair = sum_i c(seed, i) * d_i, c never 0.
// - lastLen = the clip's last packet's length; P = the full packet length.
//
// Header-only; host-tested (test/test_vox_fountain.cpp).

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "VoxFec.h"
#include "VoxWire.h"

namespace VoxFountain {

constexpr uint32_t kSidXor = 0x0F0DEC0Du;
constexpr int kSeeds = 255;
constexpr int kMaxP = VoxFec::kMaxP;
constexpr int kHeader = 7;                         // magic, rsid, seed, lastLen
constexpr int kMaxPacket = kHeader + kMaxP;        // 163: fits GRP_DATA

inline uint32_t repairSid(uint32_t sid) { return sid ^ kSidXor; }

// Repair packets for a listener missing `missing`: those and a margin.
inline int needed(int missing) { return missing <= 0 ? 0 : missing + (missing * 15 + 99) / 100 + 1; }

// After a whole clip's first pass: about a tenth, at least 2.
inline int firstPass(int total) { return total < 2 ? 0 : (total + 9) / 10 + 1; }

// Repair packets still owed a listener that rebuilds from them: `uncovered` of
// the packets it lacks go out only as repair packets, `covered` go out (or went
// out since it asked) as data packets anyway (for a listener that can't use
// repair packets, say), and `sent_since` repair packets went since it asked. A
// covered packet is lost now and then like any other (radios scanning radio2
// miss some while on the main mesh), so it costs a pass's share. Both sized for
// the loss the channel's answers have seen lately. As the app's voxFountainOwed.
// The most loss repair packets are sized for (percent): a listener losing more
// is on a poor link of its own, and asks again. As voxFountainMaxLossPct.
constexpr int kMaxLossPct = 40;
inline int lossOf(int pct) { return pct < 0 ? 0 : pct > kMaxLossPct ? kMaxLossPct : pct; }

// Repair packets for a listener missing `missing` when `loss_pct`% of what's
// sent is lost on the way (floodpace::Pace::loss_pct): enough that as many as
// it lacks still arrive, and the margin. As voxFountainNeededAt.
inline int neededAt(int missing, int loss_pct) {
  if (missing <= 0) return 0;
  const int keep = 100 - lossOf(loss_pct);
  return needed((missing * 100 + keep - 1) / keep);
}

// Repair packets after a pass of `count` data packets of which `loss_pct`% are
// lost: enough to mend what the worst-placed listener lost without asking
// again (a request round through repeaters costs a minute or more), never under
// firstPass. As voxFountainPassShare.
inline int passShare(int count, int loss_pct) {
  if (count < 2) return 0;
  const int l = lossOf(loss_pct), keep = 100 - l;
  const int sized = (count * l * 110 + 100 * keep - 1) / (100 * keep) + 2;
  const int first = firstPass(count);
  return sized > first ? sized : first;
}

inline int owed(int uncovered, int covered, int sent_since, int loss_pct = 0) {
  const int o = neededAt(uncovered, loss_pct) + passShare(covered, loss_pct) - sent_since;
  return o < 0 ? 0 : o;
}

// Whether a packet of session `heard` belongs to the same transfer as `sid`:
// its data, parity (VoxFec.h) or repair packets, or another part of the same
// SuperMax clip (part k's id is the first's XOR 0x7A000000 + k * 0x010101), or
// theirs. The sender answers requests one after another: a listener hearing
// these is waiting on its sender. As the app's voxSameTransfer.
inline bool sameTransfer(uint32_t sid, uint32_t heard) {
  const uint32_t forms[3] = {heard, VoxFec::paritySid(heard), repairSid(heard)};
  for (uint32_t b : forms) {
    const uint32_t d = sid ^ b;
    for (uint32_t j = 0; j <= 3; j++)
      for (uint32_t k = 0; k <= 3; k++) {
        const uint32_t oj = j ? 0x7A000000u + j * 0x00010101u : 0, ok = k ? 0x7A000000u + k * 0x00010101u : 0;
        if (d == (oj ^ ok)) return true;
      }
  }
  return false;
}

// A request lists at most this many packets: one listing this many may lack
// more after the last it names (the app's voxFountainListCap).
constexpr int kListCap = VoxWire::kSaltedFetchIndices;

// The coefficient of data packet i in repair packet `seed`: never 0. The
// same arithmetic as the app's voxFountainCoef (multiplies mod 2^16).
inline uint8_t coef(int seed, int i) {
  uint32_t x = ((uint32_t)seed * 0x1F1u + (uint32_t)i * 0x3Bu + 0x5A17u) & 0xFFFFu;
  for (int r = 0; r < 4; r++) {
    x = (x * 0x9E37u + 0x7F4Bu) & 0xFFFFu;
    x ^= x >> 7;
  }
  const uint32_t c = (x >> 3) & 0xFFu;
  return (uint8_t)(c == 0 ? 1 + ((x >> 8) % 255) : c);
}

// Repair packet `seed` of a `total`-packet clip whose packets are at
// bits + i * p (the last `last_len` bytes long). Returns its length, or 0.
inline int encode(uint32_t sid, const uint8_t* bits, int total, int p, int last_len, int seed, uint8_t* out,
                  size_t cap) {
  if (total < 1 || p < 1 || p > kMaxP || cap < (size_t)(kHeader + p)) return 0;
  const VoxFec::Gf& g = VoxFec::gf();
  const uint32_t rsid = repairSid(sid);
  out[0] = 0x56;
  out[1] = (uint8_t)(rsid >> 24);
  out[2] = (uint8_t)(rsid >> 16);
  out[3] = (uint8_t)(rsid >> 8);
  out[4] = (uint8_t)rsid;
  out[5] = (uint8_t)(seed % kSeeds);
  out[6] = (uint8_t)last_len;
  uint8_t* rep = out + kHeader;
  memset(rep, 0, (size_t)p);
  for (int i = 0; i < total; i++)
    g.addScaled(rep, p, coef(seed % kSeeds, i), bits + (size_t)i * (size_t)p, i == total - 1 ? last_len : p);
  return kHeader + p;
}

// One repair packet heard: its seed and codec data ([lastLen][P bytes]).
struct Repair {
  uint8_t seed;
  uint8_t len;   // 1 + P
  const uint8_t* data;
};

// Rebuilds every missing packet of a `total`-packet clip held at buf (packet i
// at buf + i * p, have(i) saying which are here) once there are as many
// repairs as packets missing and they solve. Calls put(index, bytes, len) for
// each; returns how many (0: not yet).
template <class Have, class Put>
int rebuild(int total, int p, const uint8_t* buf, Have have, const Repair* rep, int n_rep, Put put) {
  if (p < 1 || p > kMaxP || n_rep <= 0 || total < 1 || total > 255) return 0;
  uint8_t miss[255];
  int m = 0;
  for (int i = 0; i < total; i++) if (!have(i)) miss[m++] = (uint8_t)i;
  int n = 0;
  for (int k = 0; k < n_rep; k++) if (rep[k].len == 1 + p) n++;
  if (m == 0 || n < m) return 0;
  const VoxFec::Gf& g = VoxFec::gf();
  // Heap (PSRAM on the boards that have it): n x m coefficients, n rows of p.
  uint8_t* mem = (uint8_t*)malloc((size_t)n * (size_t)m + (size_t)(n + 1) * (size_t)p + (size_t)n * sizeof(uint8_t*) * 2);
  if (!mem) return 0;
  uint8_t** a = (uint8_t**)mem;
  uint8_t** rhs = a + n;
  uint8_t* cells = (uint8_t*)(rhs + n);
  uint8_t* rows = cells + (size_t)n * (size_t)m;
  uint8_t* scratch = rows + (size_t)n * (size_t)p;
  int last_len = p;
  int r = 0;
  for (int k = 0; k < n_rep; k++) {
    if (rep[k].len != 1 + p) continue;
    last_len = rep[k].data[0];
    a[r] = cells + (size_t)r * (size_t)m;
    rhs[r] = rows + (size_t)r * (size_t)p;
    memcpy(rhs[r], rep[k].data + 1, (size_t)p);
    for (int i = 0; i < total; i++)
      if (have(i)) g.addScaled(rhs[r], p, coef(rep[k].seed, i), buf + (size_t)i * (size_t)p, i == total - 1 && last_len >= 1 && last_len <= p ? last_len : p);
    for (int c = 0; c < m; c++) a[r][c] = coef(rep[k].seed, miss[c]);
    r++;
  }
  bool ok = true;
  for (int c = 0; c < m && ok; c++) {
    int piv = -1;
    for (int q = c; q < n; q++) if (a[q][c]) { piv = q; break; }
    if (piv < 0) { ok = false; break; }
    if (piv != c) {
      uint8_t* t = a[c]; a[c] = a[piv]; a[piv] = t;
      t = rhs[c]; rhs[c] = rhs[piv]; rhs[piv] = t;
    }
    const uint8_t iv = g.inv(a[c][c]);
    for (int k = 0; k < m; k++) a[c][k] = g.mul(a[c][k], iv);
    memset(scratch, 0, (size_t)p);
    g.addScaled(scratch, p, iv, rhs[c], p);
    memcpy(rhs[c], scratch, (size_t)p);
    for (int q = 0; q < n; q++) {
      if (q == c || !a[q][c]) continue;
      const uint8_t f = a[q][c];
      for (int k = 0; k < m; k++) a[q][k] ^= g.mul(f, a[c][k]);
      g.addScaled(rhs[q], p, f, rhs[c], p);
    }
  }
  int got = 0;
  if (ok) {
    for (int k = 0; k < m; k++) {
      const int i = miss[k];
      put(i, rhs[k], i == total - 1 && last_len >= 1 && last_len <= p ? last_len : p);
      got++;
    }
  }
  free(mem);
  return got;
}

}  // namespace VoxFountain

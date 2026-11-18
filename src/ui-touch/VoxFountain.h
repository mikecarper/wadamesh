// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Rateless repair packets for MediaMesh transfers (prototype), as the app
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

// SPDX-License-Identifier: GPL-3.0-or-later
//
// MediaMesh coverage mapping: where the MediaMesh repeaters can be reached,
// recorded as you walk or drive (MEDIAMESH.md, "Coverage map").
//
// While recording, each time the radio has moved 150 m (and at most every
// 40 s, so the repeaters' answers per two minutes go round) it asks the
// repeaters in direct range, as Find MediaMesh does (DISCOVER_REQ with the
// radio2 flag), and keeps only the ones that answer with radio2 on: MediaMesh
// repeaters, nothing else.
//
// The ask goes out on the main mesh, so radio2's reach is estimated from it:
// the weaker of the two directions' SNR, less the extra noise of radio2's wider
// bandwidth, against the SNR radio2's spreading factor still decodes. The same
// sums as MeshCore Open TAK's mediaMeshMargin
// (lib/services/mediamesh_coverage.dart); the golden values are in both tests.
//
// No Arduino or LVGL here: test/test_mediamesh_coverage.cpp runs it on the host.

#pragma once

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

namespace mmcov {

static constexpr uint32_t kMoveMetres = 150;      // a new sample after moving this far
static constexpr uint32_t kMinGapMs = 40000;      // and never asking more often than this
static constexpr uint32_t kTickMs = 5000;         // how often the recorder looks
static constexpr int kMaxRepeaters = 6;           // kept per sample, strongest first

/** The lowest SNR (dB) LoRa decodes at each spreading factor. */
inline float snrFloor(uint8_t sf) {
  switch (sf) {
    case 5:  return -2.5f;
    case 6:  return -5.0f;
    case 7:  return -7.5f;
    case 8:  return -10.0f;
    case 9:  return -12.5f;
    case 10: return -15.0f;
    case 11: return -17.5f;
    default: return -20.0f;
  }
}

/** Radio2's estimated margin (dB) over what it decodes, from an SNR measured on
 *  the main mesh at main_bw_hz. */
inline float margin(float snr, uint32_t main_bw_hz, uint32_t r2_bw_hz, uint8_t r2_sf) {
  const float widen = (main_bw_hz && r2_bw_hz) ? 10.0f * log10f((float)r2_bw_hz / (float)main_bw_hz) : 0.0f;
  return snr - widen - snrFloor(r2_sf);
}

/** The weaker direction: ours of the reply, the repeater's of the ask. */
inline float linkSnr(int8_t our_snr_q4, int8_t their_snr_q4) {
  const float a = our_snr_q4 / 4.0f, b = their_snr_q4 / 4.0f;
  return a < b ? a : b;
}

enum Reach : uint8_t { Good = 0, Marginal = 1, Out = 2, None = 3 };

inline Reach reachFor(bool any, float best) {
  if (!any) return None;
  if (best >= 5.0f) return Good;
  if (best >= 0.0f) return Marginal;
  return Out;
}

inline const char* reachName(Reach r) {
  switch (r) {
    case Good:     return "good";
    case Marginal: return "marginal";
    case Out:      return "out";
    default:       return "none";
  }
}

/** The map colours, the same as the app's (RGB888). */
inline uint32_t reachRgb(Reach r) {
  switch (r) {
    case Good:     return 0x22C55E;
    case Marginal: return 0xF59E0B;
    case Out:      return 0xEF4444;
    default:       return 0x6B7280;
  }
}

/** Metres between two points (equirectangular: plenty for 150 m steps). */
inline double metresBetween(int32_t lat1_e6, int32_t lon1_e6, int32_t lat2_e6, int32_t lon2_e6) {
  const double k = 3.14159265358979323846 / 180.0 / 1e6;
  const double x = (lon2_e6 - lon1_e6) * k * cos((lat1_e6 + lat2_e6) * 0.5 * k);
  const double y = (lat2_e6 - lat1_e6) * k;
  return sqrt(x * x + y * y) * 6371000.0;
}

/** One sample: where, and the best MediaMesh repeater's margin there. */
struct Sample {
  int32_t lat_e6 = 0, lon_e6 = 0;
  int8_t best_q4 = 0;   // best margin x4 (dB), clamped
  uint8_t count = 0;    // MediaMesh repeaters that answered
  Reach reach() const { return reachFor(count != 0, best_q4 / 4.0f); }
};

inline int8_t toQ4(float db) {
  float q = db * 4.0f;
  if (q > 127) q = 127;
  if (q < -128) q = -128;
  return (int8_t)lroundf(q);
}

/** Whether to ask again: moved far enough since the last sample, and long enough after it. */
inline bool due(bool have_last, int32_t last_lat, int32_t last_lon, uint32_t last_ms, int32_t lat, int32_t lon,
                uint32_t now_ms) {
  if (!have_last) return true;
  if ((uint32_t)(now_ms - last_ms) < kMinGapMs) return false;
  return metresBetween(last_lat, last_lon, lat, lon) >= kMoveMetres;
}

// key: the repeater's first 4 key bytes (hex), so the points can be sent to an
// analyzer (MediaMeshCoverageShare.h); files from before it have no such column.
static constexpr const char* kCsvHeader = "epoch,lat,lon,reach,best_margin,repeater,snr,remote_snr,margin,key\n";

/** One CSV line per repeater (or one with blanks when none answered). */
inline int csvLine(char* out, size_t cap, uint32_t epoch, int32_t lat_e6, int32_t lon_e6, Reach reach, float best,
                   const char* repeater, float snr, float remote_snr, float m, const char* key = nullptr) {
  char lat[16], lon[16];
  snprintf(lat, sizeof lat, "%.6f", lat_e6 / 1e6);
  snprintf(lon, sizeof lon, "%.6f", lon_e6 / 1e6);
  if (!repeater) return snprintf(out, cap, "%lu,%s,%s,%s,,,,,\n", (unsigned long)epoch, lat, lon, reachName(reach));
  return snprintf(out, cap, "%lu,%s,%s,%s,%.1f,%s,%.1f,%.1f,%.1f%s%s\n", (unsigned long)epoch, lat, lon, reachName(reach),
                  best, repeater, snr, remote_snr, m, key ? "," : "", key ? key : "");
}

}  // namespace mmcov

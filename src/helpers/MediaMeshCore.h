// SPDX-License-Identifier: GPL-3.0-or-later
//
// MediaMesh: a second LoRa profile (`radio2`) on the same transceiver. By
// default MeshCore Open TAK photos and voice clips go out on it as a media-only
// side channel, but it is the full two-profile model of mikecarper/MeshCore's
// radio_profiles.md (docs/radio_profiles.md there), less Meshtastic chat:
//
//   radio2 / tempradio2 / radioat2 / tempradioat2   the second profile, saved,
//                                                   temporary and scheduled
//   tempradio / normalradio                         a temporary primary profile
//   radio2.cross                                    copying traffic between them
//   tx.media / tx.user / tx.channel                 per-traffic TX choices
//   mediamesh.share                                 also serve main-mesh nodes
//   radio2.repeat                                   relay radio2 traffic on radio2
//   radio2.status / radio2.scan / radio2.timing     diagnostics
//   #r2- channel names                              channels bound to radio2
//
// One radio time-shares the two profiles, so this is a scan, not a second
// receiver.
//
// This header is the portable part: settings, validation, the CLI, routing
// rules, schedules, scan timing and the packet markers. No Arduino or radio
// code, so the host test builds it directly (test/test_mediamesh_core.cpp).
// The radio side is MediaMeshRadio.h; the glue is MediaMesh.cpp.
//
// Scan timing follows mikecarper/MeshCore's RadioProfiles: listen 4.6 symbols on
// the slower profile, then fill the rest of half the slow preamble on the faster
// one, so a packet on either profile is still in its preamble when the radio
// comes back. The main mesh's preamble is fixed by stock nodes; radio2 senders
// lengthen their own preamble to cover the receiver's time on the main profile.

#pragma once

#include <ctype.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 2.4 GHz LoRa for radio2: the LR1121 and LR2021 also tune 2400-2500 MHz
// (the LR1110 and the SX126x don't). A host test can define it to cover both.
#if defined(USE_LR1121) || defined(USE_LR2021)
#ifndef MEDIAMESH_HAS_24GHZ
#define MEDIAMESH_HAS_24GHZ 1
#endif
#endif

namespace mediamesh {

enum Mode : uint8_t { MODE_OFF = 0, MODE_RX = 1, MODE_RXTX = 2 };

// A transmit choice, as mikecarper/MeshCore's RadioTxPolicy. AUTO follows the
// radio2.cross rules; the others pick profiles regardless of crossing.
enum Policy : uint8_t { TX_AUTO = 0, TX_RADIO = 1, TX_RADIO2 = 2, TX_BOTH = 3, TX_OFF = 4 };

// Whether traffic is copied between the two profiles (radio2.cross).
enum Cross : uint8_t { CROSS_AUTO = 0, CROSS_ON = 1, CROSS_OFF = 2 };

struct Params {
  float freq = 0;          // MHz
  float bw = 0;            // kHz
  uint8_t sf = 0;
  uint8_t cr = 0;          // 5..8 = coding rate 4/5..4/8
  uint16_t preamble = 0;   // symbols; 0 = automatic
};

// A second profile: its mode and radio settings.
struct Profile2 {
  uint8_t mode = MODE_OFF;
  Params p;
};

// What is saved.
struct Config {
  uint8_t mode = MODE_OFF;       // radio2: off, rx (listen only) or rxtx
  uint8_t media = TX_AUTO;       // tx.media: where photos and voice are sent
  uint8_t cross = CROSS_OFF;     // radio2.cross: off unless set in the Terminal
  // mediamesh.share: 0 keeps photo/voice transfers on tx.media's route (with
  // MediaMesh, radio2 only: requests and answers, as keymindCascade does). 1
  // also serves nodes only on the main mesh: requests go out on both radios,
  // and a transfer asked for only on the main radio is answered there.
  uint8_t share = 0;
  // radio2.repeat ("Repeat MediaMesh"): relay what is heard on radio2, on
  // radio2, leaving the main mesh alone. A standalone becomes a MediaMesh
  // repeater (on 2.4 GHz too). Client Repeat holds radio2 off, so not then.
  uint8_t relay = 0;
  // Client Repeat holds radio2 (off, or beside the off-grid channel): the mode
  // it had (off, rx or rxtx), put back when Client Repeat is turned off.
  // kNotHeld when Client Repeat is off. held_p and held_cross are its radio
  // settings and radio2.cross then (held_p.freq 0: not recorded, kept as is).
  uint8_t held = 0xFF;
  uint8_t held_cross = CROSS_OFF;
  Params held_p;
  // Was the developer switch (`mediamesh.main`) letting photos and voice use
  // the main mesh. Removed: it is always 0 and only keeps the saved layout.
  // Photos and voice go on radio2 alone, whatever tx.media and
  // mediamesh.share say, and nowhere without radio2: a photo burst on the
  // main mesh is minutes of everyone's airtime.
  uint8_t main_ok = 0;
  Params p;
};

// Where photos and voice go: radio2 alone, whatever tx.media says (off stays
// off). Never the main mesh.
inline uint8_t effectiveMedia(const Config& c) {
  return c.media == TX_OFF ? (uint8_t)TX_OFF : (uint8_t)TX_RADIO2;
}
// mediamesh.share is off for good: transfers never go to the main mesh.
inline bool effectiveShare(const Config&) { return false; }

static constexpr uint8_t kNotHeld = 0xFF;

// Defaults for a new radio2 (the fast preset): 911.150 MHz, 250 kHz SF7, about
// 4x faster than SF7/62.5 for ~6 dB less range. The range preset is the same
// at 125 kHz (half the speed, ~40% farther). 911.150, a quiet spot found on
// site, is inside a BayMesh 3 cavity filter tuned for 910.525 (flat about
// 908.6-912.8 MHz), 625 kHz above the main channel.
static constexpr float kDefaultFreq = 911.150f;
static constexpr float kDefaultBw = 250.0f;
static constexpr float kRangeBw = 125.0f;

// The 2.4 GHz preset (LR1121, LR2021), balanced for speed and distance:
// 2424.5 MHz sits in the gap between Wi-Fi channels 1 and 6; 406.25 kHz SF8
// moves about 10 kb/s, the same as the 900 MHz 250 kHz preset, and hears
// about 5.5 dB deeper than the fastest 2.4 GHz setting (812.5 kHz SF7: 3 dB
// from the width, 2.5 from the SF), nearly twice the distance in the open.
// At 2.4 GHz the same distance loses about 8.5 dB more than at 915 MHz, and
// the 2.4 GHz PA gives about 9 dB less than the sub-GHz one at +22 dBm, so
// it still reaches less far than the 900 MHz presets.
static constexpr float k24Freq = 2424.5f;
static constexpr float k24Bw = 406.25f;
static constexpr uint8_t k24Sf = 8;
inline bool is24GHz(float freq) { return freq >= 2400.0f && freq <= 2500.0f; }
// Whether this radio does 2.4 GHz LoRa: built for one that can
// (MEDIAMESH_HAS_24GHZ) and, on the device, the chip itself said so at start-
// up (MediaMesh.cpp begin() asks it: an LR1121, LR1120 or LR2021, not an
// LR1110 fitted where an LR1121 was expected). Until it has been asked, the
// build decides.
inline bool& has24GHzChip() {
  static bool v = true;
  return v;
}
inline void setHas24GHz(bool chip_can) { has24GHzChip() = chip_can; }
inline bool has24GHz() {
#ifdef MEDIAMESH_HAS_24GHZ
  return has24GHzChip();
#else
  return false;
#endif
}

// ---- Find MediaMesh -------------------------------------------------------------
// Which preset a MediaMesh nearby uses. radio2 listens (rx, tempradio2) on each
// preset in turn while a repeater discovery request goes out on the main mesh:
// MediaMesh repeaters answer on both radios (keymindCascade's tx.reply both),
// so their radio2 copy is only heard on the preset they run. Shared with
// MeshCore Open TAK (lib/services/media_mesh_find.dart).
static constexpr uint32_t kFindSettleMs = 1500;   // after the retune, before asking
static constexpr uint32_t kFindListenMs = 9000;   // repeaters answer after a random delay (x4)
// 0: 250 kHz fast, 1: 125 kHz range, -1: nothing heard on either. A tie goes
// to the fast preset.
inline int findPick(uint32_t heard_fast, uint32_t heard_range) {
  if (!heard_fast && !heard_range) return -1;
  return heard_range > heard_fast ? 1 : 0;
}

// ---- Find MediaMesh by asking ------------------------------------------------------
// The first step, like Discover Repeaters: a zero-hop DISCOVER_REQ with the
// radio2 flag, and repeaters that support it (keymindCascade, proposed) append
// their radio2 profile to the DISCOVER_RESP after their key:
//   [ver=1][flags][freq u32 LE, MHz x1000][bw u32 LE, kHz x1000][sf][cr]
//   flags: bit 0 configured, bits 1-2 mode (0 off, 1 rx, 2 rxtx), bit 3 temporary
//   radio2 off: just [1][0]
// Stock firmware ignores the flag (it reads the top four bits of byte 0) and
// answers without the trailer. Same layout as MeshCore Open TAK's
// DiscoverRadio2 (lib/connector/meshcore_protocol.dart); the golden bytes are
// in both test suites.
static constexpr uint8_t kDiscoverWantRadio2 = 0x02;   // DISCOVER_REQ byte 0 flag
static constexpr uint32_t kAskListenMs = 12000;        // collect answers this long
static constexpr uint8_t kDiscoverRadio2Ver = 1;
static constexpr uint8_t kDiscoverRadio2Len = 12;

struct DiscoverRadio2 {
  bool configured = false;
  uint8_t mode = 0;          // 0 off, 1 rx, 2 rxtx
  bool temporary = false;
  uint32_t freq_khz = 0;     // MHz x1000
  uint32_t bw_hz = 0;        // kHz x1000
  uint8_t sf = 0, cr = 0;
  bool active() const { return configured && mode != 0; }
};

/** The trailer in `b` (what follows the key), or false when it isn't one. A
 *  later version may append; the first twelve bytes keep their meaning. */
inline bool parseDiscoverRadio2(const uint8_t* b, size_t n, DiscoverRadio2& out) {
  out = DiscoverRadio2{};
  if (!b || n < 2 || b[0] < kDiscoverRadio2Ver) return false;
  if (!(b[1] & 0x01)) return true;   // radio2 not configured
  if (n < kDiscoverRadio2Len) return false;
  out.configured = true;
  out.mode = (uint8_t)((b[1] >> 1) & 0x03);
  out.temporary = (b[1] & 0x08) != 0;
  out.freq_khz = (uint32_t)b[2] | ((uint32_t)b[3] << 8) | ((uint32_t)b[4] << 16) | ((uint32_t)b[5] << 24);
  out.bw_hz = (uint32_t)b[6] | ((uint32_t)b[7] << 8) | ((uint32_t)b[8] << 16) | ((uint32_t)b[9] << 24);
  out.sf = b[10];
  out.cr = b[11];
  return true;
}

/** Writes the trailer; returns its length (2 or 12). */
inline size_t encodeDiscoverRadio2(const DiscoverRadio2& p, uint8_t out[kDiscoverRadio2Len]) {
  out[0] = kDiscoverRadio2Ver;
  if (!p.configured) { out[1] = 0; return 2; }
  out[1] = (uint8_t)(0x01 | ((p.mode & 0x03) << 1) | (p.temporary ? 0x08 : 0));
  for (int i = 0; i < 4; i++) out[2 + i] = (uint8_t)(p.freq_khz >> (8 * i));
  for (int i = 0; i < 4; i++) out[6 + i] = (uint8_t)(p.bw_hz >> (8 * i));
  out[10] = p.sf;
  out[11] = p.cr;
  return kDiscoverRadio2Len;
}

/** A profile the app's settings can hold (MeshCore Open TAK's enums). */
inline bool settableProfile(const DiscoverRadio2& p) {
  static const uint32_t kBw[] = {7800, 10400, 15600, 20800, 31250, 41700, 62500, 125000, 250000, 500000};
  bool bw = false;
  for (uint32_t v : kBw) bw |= p.bw_hz == v;
  return bw && p.freq_khz != 0 && p.sf >= 5 && p.sf <= 12 && p.cr >= 5 && p.cr <= 8;
}

/** One answer for profilePick. */
struct Radio2Report {
  DiscoverRadio2 p;
  float snr;
};

/** The profile most reporting repeaters run (radio2 on: rx or rxtx), the
 *  stronger signal breaking a tie. Writes it, how many run it and the best
 *  SNR; false when none reports one. Same rule as MeshCore Open TAK's
 *  mediaMeshProfilePick. */
inline bool profilePick(const Radio2Report* r, size_t n, DiscoverRadio2& out, uint8_t& count, float& best_snr) {
  bool found = false;
  count = 0;
  best_snr = 0;
  for (size_t i = 0; i < n; i++) {
    const DiscoverRadio2& a = r[i].p;
    if (!a.active() || !settableProfile(a)) continue;
    bool counted = false;   // the first of its group does the counting
    for (size_t k = 0; k < i && !counted; k++) {
      const DiscoverRadio2& b = r[k].p;
      counted = b.active() && settableProfile(b) && b.freq_khz == a.freq_khz && b.bw_hz == a.bw_hz && b.sf == a.sf
                && b.cr == a.cr;
    }
    if (counted) continue;
    uint8_t c = 0;
    float snr = -1000;
    for (size_t k = i; k < n; k++) {
      const DiscoverRadio2& b = r[k].p;
      if (b.active() && settableProfile(b) && b.freq_khz == a.freq_khz && b.bw_hz == a.bw_hz && b.sf == a.sf
          && b.cr == a.cr) {
        if (c < 255) c++;
        if (r[k].snr > snr) snr = r[k].snr;
      }
    }
    if (!found || c > count || (c == count && snr > best_snr)) {
      found = true;
      out = a;
      out.temporary = false;
      count = c;
      best_snr = snr;
    }
  }
  return found;
}
static constexpr uint8_t kDefaultSf = 7;
static constexpr uint8_t kDefaultCr = 5;

static constexpr double kListenSymbols = 4.6;       // minimum visit per profile
static constexpr uint8_t kAcquireSymbols = 16;      // fast-profile acquisition margin
static constexpr uint16_t kMinPreamble = 32;
static constexpr uint16_t kMaxAutoPreamble = 128;
static constexpr uint16_t kMaxPreamble = 1024;
static constexpr uint32_t kDefaultSwitchUs = 1500;  // until the first hops are measured
static constexpr uint8_t kSwitchSamples = 4;        // retunes measured before the timing is trusted
static constexpr uint32_t kMarkTtlMs = 10UL * 60UL * 1000UL;
static constexpr uint32_t kMaxTempMinutes = 10080;  // a week, as tempradio2
static constexpr uint32_t kMaxScheduleSeconds = 24UL * 24UL * 3600UL;   // 24 days ahead

inline void defaults(Config& c) {
  c.mode = MODE_OFF;
  c.media = TX_AUTO;
  c.cross = CROSS_OFF;
  c.share = 0;
  c.relay = 0;
  c.held = kNotHeld;
  c.p.freq = kDefaultFreq;
  c.p.bw = kDefaultBw;
  c.p.sf = kDefaultSf;
  c.p.cr = kDefaultCr;
  c.p.preamble = 0;
}

// A radio that has never saved MediaMesh settings starts with radio2 on
// (rxtx at the defaults above) when its main radio is in the US 902-928 MHz
// band, where the default radio2 frequency belongs; elsewhere it starts off.
inline bool defaultOnFor(float main_freq) { return main_freq >= 902.0f && main_freq <= 928.0f; }

// What's sent on radio2 starts as the settings page's "Photos & voice": photos
// and voice go on radio2, nothing else crosses. A radio that never chose (its
// media still automatic) moves to it once. True if `c` changed.
inline bool photoVoiceDefault(Config& c) {
  if (c.media != TX_AUTO) return false;
  c.media = TX_RADIO2;
  c.cross = CROSS_OFF;
  return true;
}

inline bool validBw(float bw) {
  static const float bws[] = {7.8f, 10.4f, 15.6f, 20.8f, 31.25f, 41.7f, 62.5f, 125.0f, 250.0f, 500.0f};
  for (float b : bws) if (fabsf(bw - b) < 0.01f) return true;
  return false;
}

// LoRa widths at 2.4 GHz (SX128x-compatible; the LR1121's only ones there).
inline bool validBw24(float bw) {
  static const float bws[] = {203.125f, 406.25f, 812.5f};
  for (float b : bws) if (fabsf(bw - b) < 0.01f) return true;
  return false;
}

inline bool validShape(const Params& p) {
  return p.sf >= 5 && p.sf <= 12 && p.cr >= 5 && p.cr <= 8
      && (p.preamble == 0 || (p.preamble >= 8 && p.preamble <= kMaxPreamble));
}

// A sub-GHz profile: what the main radio (and tempradio) can use.
inline bool validSubGhz(const Params& p) {
  return isfinite(p.freq) && p.freq >= 150.0f && p.freq <= 960.0f && validBw(p.bw) && validShape(p);
}

// A radio2 profile: sub-GHz, or 2.4 GHz on a radio that has it.
inline bool validParams(const Params& p) {
  if (validSubGhz(p)) return true;
  return has24GHz() && isfinite(p.freq) && is24GHz(p.freq) && validBw24(p.bw) && validShape(p);
}

inline bool sameParams(const Params& a, const Params& b) {
  return a.freq == b.freq && a.bw == b.bw && a.sf == b.sf && a.cr == b.cr && a.preamble == b.preamble;
}

inline bool sameChannel(const Params& a, const Params& b) {
  return fabsf(a.freq - b.freq) < 0.0005f && fabsf(a.bw - b.bw) < 0.01f && a.sf == b.sf;
}

inline Profile2 savedProfile(const Config& c) {
  Profile2 s;
  s.mode = c.mode;
  s.p = c.p;
  return s;
}

// The radio scans both profiles whenever a second profile is on.
inline bool scanning(const Profile2& s) { return s.mode != MODE_OFF && validParams(s.p); }
inline bool scanning(const Config& c) { return scanning(savedProfile(c)); }

// The settings page's "Photos & voice on MediaMesh": radio2 can send, and
// photos and voice are sent there.
inline bool mediaOnRadio2(const Config& c) {
  return c.mode == MODE_RXTX && (c.media == TX_RADIO2 || c.media == TX_BOTH) && validParams(c.p);
}

// ---- policy names -----------------------------------------------------------

inline const char* modeName(uint8_t m) { return m == MODE_RXTX ? "rxtx" : m == MODE_RX ? "rx" : "off"; }
inline const char* policyName(uint8_t p) {
  switch (p) {
    case TX_RADIO: return "radio";
    case TX_RADIO2: return "radio2";
    case TX_BOTH: return "both";
    case TX_OFF: return "off";
    default: return "auto";
  }
}
inline bool parsePolicy(const char* s, uint8_t& out) {
  for (uint8_t v = TX_AUTO; v <= TX_OFF; ++v) {
    if (!strcmp(s, policyName(v))) { out = v; return true; }
  }
  return false;
}
inline const char* crossName(uint8_t c) { return c == CROSS_ON ? "on" : c == CROSS_OFF ? "off" : "auto"; }
// The profiles a mask names: "radio", "radio2", "both" or "off".
inline const char* maskName(uint8_t mask) {
  return mask == 3 ? "both" : mask == 2 ? "radio2" : mask == 1 ? "radio" : "off";
}

// ---- persisted form -------------------------------------------------------
// v1 (the first MediaMesh): byte 4 was the media route, 0 radio / 1 radio2, and
// nothing ever crossed. v2: byte 4 is a Policy and byte 7 the cross mode. A v1
// blob keeps its behaviour: media where it was, crossing off. v3 adds byte 18,
// mediamesh.share; older blobs read as share off. v4 adds byte 19, the radio2
// mode Client Repeat is holding (0xFF none). v5 adds byte 20, the developer
// switch (mediamesh.main). v6 adds byte 21, radio2.repeat. v7 adds bytes
// 22-34, the radio2 settings and cross mode Client Repeat is holding. v8 is
// v7 with bridging off by default: an older blob's cross modes (saved and
// held) read as off, once, and the next save writes v8.

static constexpr uint8_t kBlobVersion = 8;
static constexpr size_t kBlobSize = 35;
static constexpr size_t kBlobSizeV6 = 22;
static constexpr size_t kBlobSizeV5 = 21;
static constexpr size_t kBlobSizeV4 = 20;
static constexpr size_t kBlobSizeV2 = 18;
static constexpr size_t kBlobSizeV3 = 19;

inline size_t encode(const Config& c, uint8_t out[kBlobSize]) {
  out[0] = 'M'; out[1] = 'M'; out[2] = kBlobVersion;
  out[3] = c.mode; out[4] = c.media; out[5] = c.p.sf; out[6] = c.p.cr; out[7] = c.cross;
  memcpy(&out[8], &c.p.freq, 4);
  memcpy(&out[12], &c.p.bw, 4);
  out[16] = (uint8_t)(c.p.preamble & 0xFF);
  out[17] = (uint8_t)(c.p.preamble >> 8);
  out[18] = c.share ? 1 : 0;
  out[19] = c.held;
  out[20] = c.main_ok ? 1 : 0;
  out[21] = c.relay ? 1 : 0;
  memcpy(&out[22], &c.held_p.freq, 4);
  memcpy(&out[26], &c.held_p.bw, 4);
  out[30] = c.held_p.sf;
  out[31] = c.held_p.cr;
  out[32] = (uint8_t)(c.held_p.preamble & 0xFF);
  out[33] = (uint8_t)(c.held_p.preamble >> 8);
  out[34] = c.held_cross;
  return kBlobSize;
}

inline bool decode(const uint8_t* in, size_t len, Config& c) {
  if (!in || len < kBlobSizeV2 || in[0] != 'M' || in[1] != 'M' || in[2] < 1 || in[2] > kBlobVersion) return false;
  if ((in[2] == 3 && len < kBlobSizeV3) || (in[2] == 4 && len < kBlobSizeV4) || (in[2] == 5 && len < kBlobSizeV5)
      || (in[2] == 6 && len < kBlobSizeV6) || (in[2] >= 7 && len < kBlobSize))
    return false;
  Config d;
  d.mode = in[3] <= MODE_RXTX ? in[3] : (uint8_t)MODE_OFF;
  if (in[2] == 1) {
    d.media = in[4] == 1 ? TX_RADIO2 : TX_RADIO;
    d.cross = CROSS_OFF;
  } else {
    d.media = in[4] <= TX_OFF ? in[4] : (uint8_t)TX_AUTO;
    d.cross = in[7] <= CROSS_OFF ? in[7] : (uint8_t)CROSS_OFF;
  }
  d.p.sf = in[5]; d.p.cr = in[6];
  memcpy(&d.p.freq, &in[8], 4);
  memcpy(&d.p.bw, &in[12], 4);
  d.p.preamble = (uint16_t)(in[16] | (in[17] << 8));
  d.share = 0;   // mediamesh.share was removed: a saved "on" reads as off
  d.held = in[2] >= 4 && in[19] <= MODE_RXTX ? in[19] : kNotHeld;
  d.main_ok = 0;   // the developer switch was removed: a saved "on" reads as off
  d.relay = in[2] >= 6 && in[21] == 1 ? 1 : 0;
  if (in[2] >= 7) {
    Params hp;
    memcpy(&hp.freq, &in[22], 4);
    memcpy(&hp.bw, &in[26], 4);
    hp.sf = in[30];
    hp.cr = in[31];
    hp.preamble = (uint16_t)(in[32] | (in[33] << 8));
    if (validParams(hp)) d.held_p = hp;
    d.held_cross = in[34] <= CROSS_OFF ? in[34] : (uint8_t)CROSS_OFF;
  }
  // Bridging is off by default since v8: a radio updated from older firmware
  // turns it off (saved and held), whatever it was.
  if (in[2] < 8) {
    d.cross = CROSS_OFF;
    d.held_cross = CROSS_OFF;
  }
  if (!validParams(d.p)) return false;
  c = d;
  return true;
}

// ---- timing ---------------------------------------------------------------

inline double symbolUs(float bw_khz, uint8_t sf) {
  return bw_khz > 0 && sf <= 12 ? (double)(1UL << sf) * 1000.0 / bw_khz : 0;
}

inline uint16_t roundPreamble(double symbols) {
  if (symbols < kMinPreamble) symbols = kMinPreamble;
  double r = ceil(symbols / 8.0) * 8.0;
  return r > 65528 ? 65528 : (uint16_t)r;
}

// Semtech LoRa time on air in microseconds (explicit header, CRC on).
inline uint32_t timeOnAirUs(const Params& p, uint16_t preamble, int payload_len) {
  const double tsym = symbolUs(p.bw, p.sf);
  if (tsym <= 0) return 0;
  const int de = tsym >= 16000.0 ? 1 : 0;
  const double pre = (preamble + (p.sf <= 6 ? 6.25 : 4.25)) * tsym;
  const int num = 8 * payload_len - 4 * p.sf + (p.sf <= 6 ? 20 : 28) + 16;
  const int den = 4 * (p.sf - 2 * de);
  double symbols = den > 0 ? ceil((double)num / den) * p.cr : 0;
  if (symbols < 0) symbols = 0;
  return (uint32_t)(pre + (8 + symbols) * tsym);
}

struct Timing {
  bool valid = false;
  uint8_t slow = 0;                // profile with the longer symbol (0 = main)
  uint32_t dwell_us[2] = {};       // scheduled visit on each profile
  uint16_t preamble[2] = {};       // preamble each profile transmits with
  uint16_t need[2] = {};           // preamble a scanning receiver needs (rounded)
  double need_raw[2] = {};         // the same before the 128 cap, for warnings
  uint32_t switch_us = 0;          // guarded per-retune allowance
  bool main_ok = true;             // stock main-mesh preamble is long enough
  bool media_capped = false;       // automatic radio2 preamble hit its cap
};

// `main_preamble` is what stock nodes on the main mesh send (MeshCore: 32 at
// SF<=8, 16 above). `switch_us` is the measured retune time with its guard.
inline Timing calcTiming(const Params& main, uint16_t main_preamble, const Params& media, uint32_t switch_us) {
  Timing t;
  const double T[2] = { symbolUs(main.bw, main.sf), symbolUs(media.bw, media.sf) };
  if (T[0] <= 0 || T[1] <= 0 || main_preamble == 0) return t;
  t.switch_us = switch_us;
  t.slow = T[0] >= T[1] ? 0 : 1;
  const uint8_t slow = t.slow, fast = slow ^ 1;
  const double overhead = 2.0 * switch_us;
  const uint32_t slow_dwell = (uint32_t)ceil(kListenSymbols * T[slow]);
  const uint32_t fast_min = (uint32_t)ceil(kListenSymbols * T[fast]);

  uint16_t pre[2] = { main_preamble, media.preamble };
  // A slow radio2 sets the cycle with its own (adjustable) preamble.
  if (slow == 1 && pre[1] == 0) {
    pre[1] = roundPreamble(2.0 * (slow_dwell + fast_min + overhead) / T[1]);
    if (pre[1] > kMaxAutoPreamble) { pre[1] = kMaxAutoPreamble; t.media_capped = true; }
  }
  const double half_slow = (double)pre[slow] * T[slow] / 2.0;
  const double fill = half_slow - slow_dwell - overhead;
  const uint32_t fast_dwell = fill > fast_min ? (uint32_t)floor(fill) : fast_min;
  t.dwell_us[slow] = slow_dwell;
  t.dwell_us[fast] = fast_dwell;

  const double cycle = (double)slow_dwell + fast_dwell + overhead;
  t.need_raw[slow] = ceil(fmax(kMinPreamble, 2.0 * cycle / T[slow]) / 8.0) * 8.0;
  t.need_raw[fast] = ceil(fmax(kMinPreamble, (slow_dwell + overhead) / T[fast] + kAcquireSymbols) / 8.0) * 8.0;
  t.need[slow] = roundPreamble(2.0 * cycle / T[slow]);
  t.need[fast] = roundPreamble((slow_dwell + overhead) / T[fast] + kAcquireSymbols);

  if (fast == 1 && pre[1] == 0) {
    pre[1] = t.need[1];
    if (pre[1] > kMaxAutoPreamble) { pre[1] = kMaxAutoPreamble; t.media_capped = true; }
  }
  t.preamble[0] = pre[0];
  t.preamble[1] = pre[1];
  t.main_ok = t.need[0] <= pre[0];
  t.valid = true;
  return t;
}

// What a getter or an OK reply appends after the preambles: the warnings of
// radio_profiles.md's "Chirp calculator and warnings".
inline void appendWarnings(const Timing& t, const Params& media, char* out, size_t cap) {
  if (!t.valid) return;
  size_t used = strlen(out);
  if (used < cap && (t.need_raw[0] > 128 || t.need_raw[1] > 128)) {
    snprintf(out + used, cap - used, "; WARN need >128: %s",
             t.need_raw[0] > 128 && t.need_raw[1] > 128 ? "radio,radio2" : t.need_raw[0] > 128 ? "radio" : "radio2");
  }
  const bool a = t.need[0] > t.preamble[0];
  const bool b = t.need[1] > t.preamble[1];
  used = strlen(out);
  if (used < cap && a && b) {
    snprintf(out + used, cap - used, "; WARN recommended preamble: radio=%u,radio2=%u", t.need[0], t.need[1]);
  } else if (used < cap && (a || b)) {
    snprintf(out + used, cap - used, "; WARN recommended preamble: %s=%u", a ? "radio" : "radio2", a ? t.need[0] : t.need[1]);
  }
  used = strlen(out);
  if (used < cap && media.preamble && media.preamble < t.need[1]) {
    snprintf(out + used, cap - used, "; short override: radio2");
  }
}

// ---- the running state ----------------------------------------------------
// What is saved (Config) plus the RAM-only parts: a temporary second profile,
// a temporary primary profile and the schedules. Deadlines are monotonic
// milliseconds, so setting the clock cannot stretch a temporary session.

struct Schedule {
  Profile2 prof;
  uint32_t start = 0, end = 0;          // UTC seconds, as entered (end: tempradioat2)
  uint32_t start_ms = 0, end_ms = 0;    // the same on the millisecond clock
  bool active = false, temporary = false, started = false;
};

struct Runtime {
  Config saved;
  bool temp2 = false;            // tempradio2 running
  Profile2 temp2_prof;
  uint32_t temp2_end_ms = 0;
  uint8_t temp2_slot = 0xFF;     // the tempradioat2 slot that started it, if any
  bool temp1 = false;            // tempradio running
  Params temp1_p;
  uint32_t temp1_end_ms = 0;
  Schedule sched[8];             // 0-3 radioat2, 4-7 tempradioat2
  Profile2 active2() const { return temp2 ? temp2_prof : savedProfile(saved); }
  bool enabled() const { return scanning(active2()); }
  bool hasTempSchedule() const {
    for (int i = 4; i < 8; ++i) if (sched[i].active) return true;
    return false;
  }
};

inline bool deadlinePassed(uint32_t now_ms, uint32_t deadline_ms) { return (int32_t)(now_ms - deadline_ms) >= 0; }
inline uint32_t msLeft(uint32_t now_ms, uint32_t deadline_ms) {
  return deadlinePassed(now_ms, deadline_ms) ? 0 : deadline_ms - now_ms;
}

// ---- routing --------------------------------------------------------------
// Masks: bit 0 = the primary profile, bit 1 = radio2.

inline bool canTransmit(const Runtime& r, uint8_t profile) {
  return profile == 0 || (profile == 1 && r.enabled() && r.active2().mode == MODE_RXTX);
}

/** radio2.repeat: whether a packet heard on radio2 is relayed (on radio2). */
inline bool relaysRadio2(const Runtime& r) { return r.saved.relay && canTransmit(r, 1); }

// radio2.cross: `auto` crosses between two permanent or two temporary profiles
// and isolates a permanent one from a temporary one.
inline bool canCross(const Runtime& r) {
  return r.saved.cross == CROSS_ON || (r.saved.cross == CROSS_AUTO && r.temp1 == r.temp2);
}

// Where traffic that arrived on (or, origin 0, was made on) a profile goes
// under the global rules.
inline uint8_t transmitMask(const Runtime& r, uint8_t origin) {
  if (origin > 1) origin = 0;
  uint8_t m = canTransmit(r, origin) ? (uint8_t)(1U << origin) : 0;
  if (r.enabled() && canCross(r) && canTransmit(r, origin ^ 1)) m |= (uint8_t)(1U << (origin ^ 1));
  return m;
}

// Where a packet with an explicit choice goes. `radio2` never falls back to
// the primary; `both` sends on radio2 only when it can.
inline uint8_t policyMask(const Runtime& r, uint8_t policy, uint8_t origin = 0) {
  if (policy == TX_AUTO) return transmitMask(r, origin);
  if (policy == TX_OFF || policy > TX_OFF) return 0;
  return (uint8_t)(policy & (canTransmit(r, 1) ? 3 : 1));
}

// ---- Open TAK transfers (mediamesh.share) -------------------------------------
// Which profiles each transfer was asked for on, by session id: a request
// heard only on the main radio came from a node that may have no radio2.

struct FetchLog {
  static constexpr int kSize = 8;
  static constexpr uint32_t kTtlMs = 5UL * 60UL * 1000UL;
  struct Entry { uint32_t sid = 0; uint8_t mask = 0; uint32_t at_ms = 0; };
  Entry e[kSize];
  uint8_t next = 0;

  void note(uint32_t sid, uint8_t origin, uint32_t now_ms) {
    const uint8_t bit = (uint8_t)(1U << (origin ? 1 : 0));
    for (Entry& x : e) {
      if (x.mask && x.sid == sid && now_ms - x.at_ms < kTtlMs) { x.mask |= bit; x.at_ms = now_ms; return; }
    }
    Entry& x = e[next];
    next = (uint8_t)((next + 1) % kSize);
    x.sid = sid; x.mask = bit; x.at_ms = now_ms;
  }
  /** Bit 0 main, bit 1 radio2; 0 when nobody asked (recently). */
  uint8_t mask(uint32_t sid, uint32_t now_ms) const {
    for (const Entry& x : e) {
      if (x.mask && x.sid == sid && now_ms - x.at_ms < kTtlMs) return x.mask;
    }
    return 0;
  }
};

/** The TX choice for an Open TAK transfer packet. `media` is where photos and
 *  voice go (tx.media, or the channel's choice when that is auto); `asked` is
 *  the FetchLog mask for the packet's session; `channel` that it floods on a
 *  channel (other listeners there may be on either radio). */
inline uint8_t transferPolicy(bool share, bool request, uint8_t asked, bool channel, uint8_t media) {
  if (!share) return media;                  // MediaMesh only: requests and answers alike
  if (request) return TX_BOTH;               // a sender on either radio hears it
  if (asked == 1) return channel ? TX_BOTH : TX_RADIO;   // asked for on the main radio only
  return media;
}

// ---- packet markers ---------------------------------------------------------
// The firmware marks a packet with its chosen mask by MeshCore packet hash when
// it creates it; the radio looks the hash up when the packet reaches the
// transmitter. Retries keep the same hash, so they keep the choice.

static constexpr int kHashSize = 8;   // MAX_HASH_SIZE
static constexpr uint8_t kNoMark = 0xFF;

/** Client Repeat: the profiles a forward of a packet that arrived on `origin`
 *  goes out on: radio2.cross's rules from that profile (with MediaMesh's
 *  cross off, the same one), less those it was already forwarded on (`done`,
 *  kNoMark for none) when its sender put it on both radios. */
inline uint8_t forwardMask(const Runtime& r, uint8_t origin, uint8_t done) {
  uint8_t m = transmitMask(r, origin);
  if (done != kNoMark) m &= (uint8_t)~done;
  return m;
}

struct MarkRing {
  static constexpr int kSlots = 128;   // more than a full outbound queue of photo packets
  uint8_t hash[kSlots][kHashSize] = {};
  uint32_t at[kSlots] = {};
  uint8_t mask[kSlots] = {};
  bool used[kSlots] = {};
  uint8_t next = 0;

  void add(const uint8_t* h, uint32_t now_ms, uint8_t m) {
    for (int i = 0; i < kSlots; ++i) {
      if (used[i] && memcmp(hash[i], h, kHashSize) == 0) { at[i] = now_ms; mask[i] = m; return; }
    }
    memcpy(hash[next], h, kHashSize);
    at[next] = now_ms;
    mask[next] = m;
    used[next] = true;
    next = (uint8_t)((next + 1) % kSlots);
  }
  // The mask a packet was marked with, or kNoMark.
  uint8_t find(const uint8_t* h, uint32_t now_ms) const {
    for (int i = 0; i < kSlots; ++i) {
      if (used[i] && now_ms - at[i] <= kMarkTtlMs && memcmp(hash[i], h, kHashSize) == 0) return mask[i];
    }
    return kNoMark;
  }
  bool has(const uint8_t* h, uint32_t now_ms) const { return find(h, now_ms) != kNoMark; }
  void clear() { memset(used, 0, sizeof(used)); next = 0; }
};

// Which profile each received packet arrived on, by hash, from the moment the
// radio hands it over until the mesh processes it (which can be delayed). Each
// arrival is one entry and processing takes the oldest one for its hash, so the
// same packet heard on both profiles is credited correctly to each copy.
struct OriginRing {
  static constexpr int kSlots = 64;
  uint8_t hash[kSlots][kHashSize] = {};
  uint32_t at[kSlots] = {};
  uint8_t origin[kSlots] = {};
  bool used[kSlots] = {};
  uint8_t next = 0;
  static constexpr uint32_t kTtlMs = 120000;

  void add(const uint8_t* h, uint32_t now_ms, uint8_t o) {
    memcpy(hash[next], h, kHashSize);
    at[next] = now_ms;
    origin[next] = o;
    used[next] = true;
    next = (uint8_t)((next + 1) % kSlots);
  }
  // Takes the oldest unprocessed arrival of this packet: 0 or 1, or kNoMark.
  uint8_t take(const uint8_t* h, uint32_t now_ms) {
    for (int k = kSlots; k >= 1; --k) {
      const int i = (next - k + kSlots) % kSlots;
      if (used[i] && now_ms - at[i] <= kTtlMs && memcmp(hash[i], h, kHashSize) == 0) {
        used[i] = false;
        return origin[i];
      }
    }
    return kNoMark;
  }
};

// Each queued packet's profiles, by packet object (two copies of one packet
// share a hash), with the generation of each profile when it was queued. A
// profile that has changed since (new settings, a temporary session starting
// or ending, radio2 off) drops its bit: stale packets and retries are not
// sent there, as in mikecarper/MeshCore.
struct QueueMarks {
  static constexpr int kSlots = 48;   // more than the packet pool
  const void* pkt[kSlots] = {};
  uint8_t mask[kSlots] = {};
  uint32_t gen[kSlots][2] = {};

  int find(const void* p) const {
    for (int i = 0; i < kSlots; ++i) if (pkt[i] == p) return i;
    return -1;
  }
  bool has(const void* p) const { return p && find(p) >= 0; }
  // False when full (the packet then follows the global rules).
  bool set(const void* p, uint8_t m, const uint32_t g[2]) {
    if (!p) return false;
    int i = find(p);
    if (i < 0) i = find(nullptr);
    if (i < 0) return false;
    pkt[i] = p;
    mask[i] = m;
    gen[i][0] = g[0];
    gen[i][1] = g[1];
    return true;
  }
  void erase(const void* p) {
    const int i = p ? find(p) : -1;
    if (i >= 0) pkt[i] = nullptr;
  }
  // The profiles still valid for this packet now, or kNoMark when unknown.
  uint8_t live(const void* p, const uint32_t now_gen[2]) const {
    const int i = p ? find(p) : -1;
    if (i < 0) return kNoMark;
    uint8_t m = mask[i];
    if ((m & 1) && gen[i][0] != now_gen[0]) m &= (uint8_t)~1;
    if ((m & 2) && gen[i][1] != now_gen[1]) m &= (uint8_t)~2;
    return m;
  }
};

// ---- per-contact and per-channel TX choices (tx.user / tx.channel) ----------
// Saved by key, not by slot or name: a contact by its public-key prefix, a
// channel by a hash of its secret, so a renamed or moved entry keeps its choice
// and a replaced key loses it.

struct Routes {
  static constexpr int kMax = 48;
  static constexpr int kIdSize = 8;
  enum Kind : uint8_t { NONE = 0, USER = 1, CHANNEL = 2 };
  uint8_t kind[kMax] = {};
  uint8_t id[kMax][kIdSize] = {};
  uint8_t policy[kMax] = {};

  int find(uint8_t k, const uint8_t* ident) const {
    for (int i = 0; i < kMax; ++i) if (kind[i] == k && memcmp(id[i], ident, kIdSize) == 0) return i;
    return -1;
  }
  uint8_t get(uint8_t k, const uint8_t* ident) const {
    const int i = find(k, ident);
    return i >= 0 ? policy[i] : (uint8_t)TX_AUTO;
  }
  // False when the table is full.
  bool set(uint8_t k, const uint8_t* ident, uint8_t p) {
    int i = find(k, ident);
    if (p == TX_AUTO) { if (i >= 0) kind[i] = NONE; return true; }
    if (i < 0) for (int j = 0; j < kMax && i < 0; ++j) if (kind[j] == NONE) i = j;
    if (i < 0) return false;
    kind[i] = k;
    memcpy(id[i], ident, kIdSize);
    policy[i] = p;
    return true;
  }
  int count(uint8_t k) const {
    int n = 0;
    for (int i = 0; i < kMax; ++i) if (kind[i] == k) ++n;
    return n;
  }
  // Blob: "MR" 1, then (kind, id, policy) per entry in use.
  static constexpr size_t kBlobMax = 3 + kMax * (2 + kIdSize);
  size_t encode(uint8_t* out) const {
    size_t n = 0;
    out[n++] = 'M'; out[n++] = 'R'; out[n++] = 1;
    for (int i = 0; i < kMax; ++i) {
      if (kind[i] == NONE) continue;
      out[n++] = kind[i];
      memcpy(out + n, id[i], kIdSize); n += kIdSize;
      out[n++] = policy[i];
    }
    return n;
  }
  bool decode(const uint8_t* in, size_t len) {
    if (!in || len < 3 || in[0] != 'M' || in[1] != 'R' || in[2] != 1) return false;
    Routes r;
    int slot = 0;
    for (size_t n = 3; n + 2 + kIdSize <= len && slot < kMax; n += 2 + kIdSize) {
      const uint8_t k = in[n], p = in[n + 1 + kIdSize];
      if ((k != USER && k != CHANNEL) || p == TX_AUTO || p > TX_OFF) continue;
      r.kind[slot] = k;
      memcpy(r.id[slot], in + n + 1, kIdSize);
      r.policy[slot] = p;
      ++slot;
    }
    *this = r;
    return true;
  }
};

// `get/set tx.user|tx.channel <target> [policy]`, parsed as mikecarper/MeshCore's
// CompanionTxRoutingCLI: a quoted target is always a literal name.
struct RouteCommand {
  bool matched = false, valid = false, set = false, channel = false, quoted = false;
  char target[48] = {};
  uint8_t policy = TX_AUTO;
};

inline bool routeSpace(char c) { return c == ' ' || c == '\t'; }

inline RouteCommand parseRouteCommand(const char* text) {
  RouteCommand r;
  if (!text) return r;
  if (!strncmp(text, "get ", 4)) r.set = false;
  else if (!strncmp(text, "set ", 4)) r.set = true;
  else return r;
  text += 4;
  const char* family = nullptr;
  if (!strncmp(text, "tx.user", 7)) family = "tx.user";
  else if (!strncmp(text, "tx.channel", 10)) { family = "tx.channel"; r.channel = true; }
  else return r;
  text += strlen(family);
  if (*text && !routeSpace(*text)) return r;
  r.matched = true;
  while (routeSpace(*text)) ++text;
  const char* end = text + strlen(text);
  while (end > text && routeSpace(end[-1])) --end;
  if (r.set) {
    const char* mode = end;
    while (mode > text && !routeSpace(mode[-1])) --mode;
    char value[8];
    const size_t length = (size_t)(end - mode);
    if (mode == text || length >= sizeof(value)) return r;
    memcpy(value, mode, length); value[length] = 0;
    if (!parsePolicy(value, r.policy)) return r;
    end = mode;
    while (end > text && routeSpace(end[-1])) --end;
  }
  if (end > text && *text == '"') {
    if (end - text < 2 || end[-1] != '"') return r;
    r.quoted = true;
    ++text; --end;
  }
  const size_t length = (size_t)(end - text);
  if (length >= sizeof(r.target) || ((r.set || r.quoted) && !length)) return r;
  if (memchr(text, '"', length) || memchr(text, '\r', length) || memchr(text, '\n', length)) return r;
  memcpy(r.target, text, length);
  r.target[length] = 0;
  r.valid = true;
  return r;
}

// `key:` + 12-64 hex digits (whole bytes).
inline bool parseRouteKey(const char* text, uint8_t key[32], size_t& bytes) {
  const size_t length = strlen(text);
  if (length < 12 || length > 64 || (length & 1)) return false;
  bytes = length / 2;
  for (size_t i = 0; i < bytes; ++i) {
    uint8_t value = 0;
    for (unsigned n = 0; n < 2; ++n) {
      char c = text[2 * i + n];
      if (c >= 'A' && c <= 'F') c = (char)(c + ('a' - 'A'));
      if (c >= '0' && c <= '9') value = (uint8_t)((value << 4) | (c - '0'));
      else if (c >= 'a' && c <= 'f') value = (uint8_t)((value << 4) | (c - 'a' + 10));
      else return false;
    }
    key[i] = value;
  }
  return true;
}

// ---- channel-name aliases (#r2-) --------------------------------------------
// As mikecarper/MeshCore's CompanionChannelAlias, less `mt-` (Meshtastic):
// `#r2-name` uses the hashtag key of `#name`, and while radio2 is configured it
// is bound to radio2 for sending and receiving.

enum AliasKind : uint8_t { ALIAS_NONE = 0, ALIAS_R2 = 1, ALIAS_INVALID = 2 };

struct Alias {
  uint8_t kind;
  bool hashtag;
  const char* tail;
};

inline Alias parseAlias(const char* name) {
  if (!name) return {ALIAS_INVALID, false, ""};
  const bool hashtag = name[0] == '#';
  const char* tail = name + (hashtag ? 1 : 0);
  uint8_t kind = ALIAS_NONE;
  if (!strncmp(tail, "r2-", 3)) {
    kind = ALIAS_R2;
    tail += 3;
    if (!tail[0] || tail[0] == '#' || !strncmp(tail, "r2-", 3) || !strncmp(tail, "mt-", 3)) kind = ALIAS_INVALID;
  }
  return {kind, hashtag, tail};
}

// `hash(out, out_len, text, text_len)` is SHA-256 truncated to out_len. Replaces
// the 128-bit app-derived key of `#r2-x` with `#x`'s; any other key is kept.
template <typename Hash>
bool resolveAliasKey(const char* name, uint8_t secret[32], Hash hash) {
  const Alias a = parseAlias(name);
  if (a.kind == ALIAS_INVALID) return false;
  if (a.kind == ALIAS_NONE || !a.hashtag) return true;
  for (size_t i = 16; i < 32; ++i) if (secret[i] != 0) return true;   // a 256-bit PSK
  uint8_t app_key[16];
  hash(app_key, sizeof app_key, (const uint8_t*)name, (int)strlen(name));
  if (memcmp(secret, app_key, sizeof app_key) != 0) return true;      // a custom PSK
  char canonical[40];
  const size_t len = strlen(a.tail);
  if (len + 2 > sizeof canonical) return false;
  canonical[0] = '#';
  memcpy(canonical + 1, a.tail, len + 1);
  hash(secret, 16, (const uint8_t*)canonical, (int)strlen(canonical));
  return true;
}

// The order channels are tried for a packet received on `origin` (lower first),
// or -1 when this channel must not take it.
inline int aliasReceivePriority(const char* name, uint8_t origin, bool radio2_defined) {
  const Alias a = parseAlias(name);
  if (a.kind == ALIAS_INVALID) return -1;
  if (radio2_defined && a.kind == ALIAS_R2) return origin == 1 ? 0 : -1;
  return origin == 1 ? 1 : 0;
}

// ---- CLI ------------------------------------------------------------------
// The radio2 family of mikecarper/MeshCore's RadioProfileCLI, plus tempradio
// and tx.media from its Companion. tx.user / tx.channel need the contact and
// channel tables, so MediaMesh.cpp handles them with the helpers above.

inline bool parseMode(const char* s, uint8_t& mode) {
  if (!strcmp(s, "rx")) { mode = MODE_RX; return true; }
  if (!strcmp(s, "rxtx") || !strcmp(s, "rx&tx")) { mode = MODE_RXTX; return true; }
  if (!strcmp(s, "off")) { mode = MODE_OFF; return true; }
  return false;
}

inline int splitFields(char* buf, char* fields[], int max) {
  int count = 0;
  for (char* tok = strtok(buf, ","); tok && count < max; tok = strtok(nullptr, ",")) {
    while (*tok == ' ') ++tok;
    char* end = tok + strlen(tok);
    while (end > tok && end[-1] == ' ') *--end = 0;
    fields[count++] = tok;
  }
  return count;
}

inline bool parseUint(const char* s, uint32_t& out) {
  if (!s || !*s) return false;
  char* e;
  const unsigned long v = strtoul(s, &e, 10);
  if (*e || s[0] == '-' || s[0] == '+') return false;
  out = (uint32_t)v;
  return true;
}

inline bool parsePreambleField(const char* s, uint16_t& out) {
  if (!strcmp(s, "auto")) { out = 0; return true; }
  uint32_t v;
  if (!parseUint(s, v) || (v != 0 && (v < 8 || v > kMaxPreamble))) return false;
  out = (uint16_t)v;
  return true;
}

// UTC seconds, or +N for N whole minutes from `now` (radio_profiles.md).
inline bool parseScheduleTime(const char* s, uint32_t now, uint32_t& out) {
  if (!s) return false;
  const bool rel = s[0] == '+';
  uint32_t v;
  if (!parseUint(rel ? s + 1 : s, v)) return false;
  if (rel) {
    if (!v || v > (UINT32_MAX - now) / 60U) return false;
    v = now + v * 60U;
  }
  out = v;
  return true;
}

// Parses "<MHz>,<kHz>,<sf>,<cr>,<mode>[,extra...][,preamble]": `extra` fields
// (minutes, start, end) are returned for the caller. `allow_off` accepts no
// tuple at all, only "off".
inline bool parseTuple(const char* text, Profile2& out, const char* extra[], int n_extra, char* buf,
                       size_t cap, const char** why) {
  const size_t n = strlen(text);
  if (n >= cap) { *why = "too long"; return false; }
  memcpy(buf, text, n + 1);
  char* f[9] = {};
  const int count = splitFields(buf, f, 9);
  if (count != 5 + n_extra && count != 6 + n_extra) { *why = "fields"; return false; }
  Profile2 d;
  char* e;
  d.p.freq = strtof(f[0], &e); if (*e || !*f[0]) { *why = "bad frequency"; return false; }
  d.p.bw = strtof(f[1], &e);   if (*e || !*f[1]) { *why = "bad bandwidth"; return false; }
  uint32_t sf, cr;
  if (!parseUint(f[2], sf) || sf > 255) { *why = "bad SF"; return false; }
  if (!parseUint(f[3], cr) || cr > 255) { *why = "bad CR"; return false; }
  d.p.sf = (uint8_t)sf;
  d.p.cr = (uint8_t)cr;
  if (!parseMode(f[4], d.mode) || d.mode == MODE_OFF) { *why = "mode must be rx or rxtx"; return false; }
  for (int i = 0; i < n_extra; ++i) extra[i] = f[5 + i];
  d.p.preamble = 0;
  if (count == 6 + n_extra && !parsePreambleField(f[5 + n_extra], d.p.preamble)) { *why = "bad preamble"; return false; }
  if (!validParams(d.p)) { *why = "invalid radio settings"; return false; }
  out = d;
  return true;
}

// Backwards-compatible single-purpose parser, as before (set radio2).
inline bool parseRadio2(const char* text, Config& c, const char** why) {
  Profile2 p;
  char buf[96];
  if (!parseTuple(text, p, nullptr, 0, buf, sizeof buf, why)) {
    if (!strcmp(*why, "fields")) *why = "use <MHz>,<kHz>,<sf>,<cr>,rx|rxtx[,preamble]";
    return false;
  }
  c.mode = p.mode;
  c.p = p.p;
  return true;
}

// Measured retune times, for the per-switch budget. The budget used to be the
// longest retune seen (under the stall cutoff). On a radio alone that is close
// to every retune (~0.9 ms on a repeater), but on the T-Deck the screen shares
// the SPI bus: most retunes take ~1 ms and a few wait for a redraw to let go
// of the bus, so the longest crept up towards the cutoff and the budget sat at
// ~8.7 ms. That sizes every cycle for the rare slow retune: radio2 gets a
// fraction of the listening time and needs a longer preamble. Now the budget
// follows the 95th percentile of the recent retunes; the slow few just make
// that one cycle a little longer, which the preambles' rounding absorbs.
struct SwitchTimes {
  static constexpr uint32_t kBinUs = 125;
  static constexpr uint8_t kBins = 64;              // 0-8 ms; past that is a stall
  static constexpr uint16_t kForPercentile = 32;    // until then: the longest seen
  static constexpr uint16_t kAgeAt = 4096;          // halve the counts: follow recent load
  static constexpr uint8_t kBudgetPercentile = 95;
  uint16_t bins[kBins] = {};
  uint16_t total = 0;
  uint32_t max_us = 0;   // longest since the last reset

  void reset() { *this = SwitchTimes(); }

  void add(uint32_t us) {
    uint32_t b = us / kBinUs;
    if (b >= kBins) b = kBins - 1;
    ++bins[b];
    if (us > max_us) max_us = us;
    if (++total >= kAgeAt) {
      total = 0;
      for (uint8_t i = 0; i < kBins; ++i) { bins[i] /= 2; total += bins[i]; }
    }
  }

  // The upper edge of the bin `pct` percent of the retunes fall in, never past
  // the longest seen. 0 with nothing measured.
  uint32_t percentileUs(uint8_t pct) const {
    if (!total) return max_us;
    const uint32_t want = ((uint32_t)total * pct + 99) / 100;
    uint32_t seen = 0;
    for (uint8_t i = 0; i < kBins; ++i) {
      seen += bins[i];
      if (seen >= want) {
        const uint32_t edge = (uint32_t)(i + 1) * kBinUs;
        return edge < max_us ? edge : max_us;
      }
    }
    return max_us;
  }

  uint32_t typicalUs() const { return percentileUs(50); }

  // What the guarded budget is built on.
  uint32_t baseUs() const { return total < kForPercentile ? max_us : percentileUs(kBudgetPercentile); }
};

// Live figures a getter reports; the glue fills them from the radio.
struct Stats {
  uint32_t rx[2] = {};
  uint32_t tx[2] = {};
  uint32_t switches = 0;
  uint32_t errors = 0;
  uint32_t max_us = 0;            // longest retune seen (lifetime diagnostic)
  uint32_t stalls = 0;            // retunes held up waiting for the SPI bus (left out of the timing)
  uint32_t typ_us = 0;            // typical (median) retune since the last profile change
  uint32_t heard_s = 0xFFFFFFFF;  // seconds since a valid radio2 packet; none since boot: 0xFFFFFFFF
  uint8_t samples = 0;            // retunes measured since the last profile change
  uint32_t dropped = 0;           // radio2 packets never sent: refused (scan off) or dropped from the queue
  uint32_t waits = 0;             // radio2 packets held back because radio2 was busy (listen before talk)
};

// Listen before talk on radio2. A time-shared radio can't sense radio2 while
// it is on the main channel, so "busy" is "a radio2 packet was heard lately":
// a packet bound for radio2 alone waits until radio2 has been quiet for one
// packet's airtime plus a random part of another (so two that both waited
// don't then collide), at most kLbtMaxWaitMs in all, after which it goes
// anyway rather than never.
constexpr uint32_t kLbtMaxWaitMs = 20000;
inline uint32_t lbtQuietMs(uint32_t packet_air_ms, uint32_t rnd) {
  const uint32_t air = packet_air_ms ? packet_air_ms : 100;
  return air + 50 + rnd % (air / 2 + 1);
}
// Whether to hold a radio2 packet now: radio2 heard `since_heard_ms` ago (or
// never: `heard` false), a wait under way since `waiting_ms` (0 when none).
inline bool lbtHold(bool heard, uint32_t since_heard_ms, uint32_t quiet_ms, uint32_t waiting_ms) {
  if (!heard || since_heard_ms >= quiet_ms) return false;
  return waiting_ms < kLbtMaxWaitMs;
}

struct Env {
  Params main;                    // the primary profile running now (tempradio or saved)
  Params main_saved;              // the saved primary (normal radio)
  uint16_t main_preamble = 32;    // what the primary transmits with
  uint32_t switch_us = kDefaultSwitchUs;   // guarded retune allowance in use
  bool supported = true;          // this radio can run a second profile
  uint32_t now_ms = 0;
  uint32_t epoch = 0;             // UTC seconds, 0 when the clock is unset
  Stats stats;
};

struct CliResult {
  bool handled = false;
  bool save = false;              // persist Runtime::saved
  bool apply = false;             // the second profile changed: retune / restart the scan
  bool apply_primary = false;     // the primary profile changed (tempradio)
};

// The timing a getter shows for a second profile, with the primary in env.
inline Timing previewTiming(const Env& env, const Params& p) {
  return calcTiming(env.main, env.main_preamble, p, env.switch_us);
}

inline void formatTuple(const Profile2& s, uint16_t preamble, char* out, size_t cap) {
  snprintf(out, cap, "%.3f,%.3f,%u,%u,%s,%u%s", s.p.freq, s.p.bw, s.p.sf, s.p.cr, modeName(s.mode), preamble,
           s.p.preamble ? "" : " (auto)");
}

inline void appendLeft(uint32_t ms, char* out, size_t cap) {
  const uint32_t minutes = (ms + 59999UL) / 60000UL;
  const size_t used = strlen(out);
  if (used < cap) {
    snprintf(out + used, cap - used, "; %lud%luh%lum (%lu min) left", (unsigned long)(minutes / 1440),
             (unsigned long)(minutes / 60 % 24), (unsigned long)(minutes % 60), (unsigned long)minutes);
  }
}

inline void formatProfileGet(const Env& env, const Profile2& s, char* out, size_t cap) {
  if (s.mode == MODE_OFF) { snprintf(out, cap, "> off"); return; }
  const Timing t = previewTiming(env, s.p);
  char tuple[64];
  formatTuple(s, t.valid ? t.preamble[1] : (s.p.preamble ? s.p.preamble : kMinPreamble), tuple, sizeof tuple);
  snprintf(out, cap, "> %s", tuple);
}

// Ends a temporary second profile and every tempradioat2 schedule.
inline void endTemp2(Runtime& r) {
  r.temp2 = false;
  r.temp2_slot = 0xFF;
  for (int i = 4; i < 8; ++i) r.sched[i] = Schedule();
}

/** Client Repeat holds radio2: with `beside` (the US band; ClientRepeat.h's
 *  radio2For) radio2 runs MediaMesh there, listening and sending, with
 *  radio2.cross off so ordinary traffic stays on the off-grid main channel;
 *  without it radio2 is off, as choosing Off is, a 2.4 GHz radio2 included.
 *  The mode, settings and cross mode it had are remembered and put back when
 *  Client Repeat is turned off. Call with the current setting every loop:
 *  radio2 changed meanwhile (from the app, the Terminal or a schedule) is held
 *  again, and that newer choice is the one restored. True when the saved
 *  settings changed (save and apply them). */
inline bool holdForClientRepeat(Runtime& r, bool on, const Params* beside = nullptr) {
  Config& c = r.saved;
  if (!on) {
    if (c.held == kNotHeld) return false;
    c.mode = c.held;
    if (c.held_p.freq > 0 && validParams(c.held_p)) c.p = c.held_p;
    c.cross = c.held_cross;
    c.held = kNotHeld;
    c.held_p = Params();
    c.held_cross = CROSS_OFF;
    return true;
  }
  // What Client Repeat wants radio2 to be.
  const uint8_t want_mode = beside ? (uint8_t)MODE_RXTX : (uint8_t)MODE_OFF;
  const bool holding = c.mode == want_mode && (!beside || (sameParams(c.p, *beside) && c.cross == CROSS_OFF));
  // A radio2 Client Repeat itself set (beside one it set before, the main
  // radio having moved since) isn't a choice to remember.
  const bool ours = c.held != kNotHeld && c.mode == MODE_RXTX && c.cross == CROSS_OFF && c.p.bw == 250.0f &&
                    c.p.sf == 7 && c.p.cr == 5 && c.p.preamble == 0;
  bool changed = false;
  if (c.held == kNotHeld || !holding) {   // the first call always records
    if (c.held == kNotHeld || !ours) {
      c.held = c.mode;
      // `set radio2 off` keeps the settings (and cross) Client Repeat put
      // there: those aren't a choice to remember.
      if (c.held_p.freq == 0 || !beside || !sameParams(c.p, *beside)) {
        c.held_p = c.p;
        c.held_cross = c.cross;
      }
    }
    c.mode = want_mode;
    if (beside) {
      c.p = *beside;
      c.cross = CROSS_OFF;
    }
    changed = true;
  }
  if (r.temp2) {
    endTemp2(r);
    changed = true;
  }
  return changed;
}

inline CliResult handleCli(const char* cmd, Runtime& r, const Env& env, char* reply, size_t cap) {
  CliResult res;
  while (*cmd == ' ') ++cmd;
  enum { GET, SET, DEL, BARE } verb = BARE;
  const char* text = cmd;
  if (!strncmp(text, "get ", 4)) { verb = GET; text += 4; }
  else if (!strncmp(text, "set ", 4)) { verb = SET; text += 4; }
  else if (!strncmp(text, "del ", 4)) { verb = DEL; text += 4; }
  while (*text == ' ') ++text;
  char key[24];
  size_t k = 0;
  while (text[k] && text[k] != ' ' && k + 1 < sizeof(key)) { key[k] = text[k]; ++k; }
  key[k] = 0;
  if (text[k] && text[k] != ' ') return res;   // key too long: not ours
  const char* arg = text + k;
  while (*arg == ' ') ++arg;

  const bool base = !strcmp(key, "radio2");
  const bool temp = !strcmp(key, "tempradio2");
  const bool sched = !strcmp(key, "radioat2") || !strcmp(key, "tempradioat2");
  const bool sched_temp = !strcmp(key, "tempradioat2");
  const bool media = !strcmp(key, "tx.media");
  const bool cross = !strcmp(key, "radio2.cross");
  const bool timing_key = !strcmp(key, "radio2.timing") || !strcmp(key, "radio.timing");
  const bool scan_key = !strcmp(key, "radio2.scan");
  const bool status_key = !strcmp(key, "radio2.status");
  const bool summary = !strcmp(key, "mediamesh");
  const bool share = !strcmp(key, "mediamesh.share");
  const bool repeat = !strcmp(key, "radio2.repeat");
  const bool main_key = !strcmp(key, "mediamesh.main");   // developer switch, not in the help
  const bool temp1 = !strcmp(key, "tempradio");
  const bool normal = !strcmp(key, "normalradio");
  if (verb == BARE && !temp1 && !normal) return res;
  if (verb != BARE && (normal || (temp1 && verb == DEL))) return res;
  if (!base && !temp && !sched && !media && !cross && !timing_key && !scan_key && !status_key && !summary
      && !share && !repeat && !main_key && !temp1 && !normal) {
    return res;
  }
  if (verb == DEL && !sched) return res;
  res.handled = true;
  const Profile2 active = r.active2();

  // ---- read-only diagnostics ----
  if (timing_key || scan_key || status_key || summary) {
    if (verb != GET || *arg) { snprintf(reply, cap, "Error: %s is read-only", key); return res; }
    const Timing t = active.mode != MODE_OFF ? previewTiming(env, active.p) : Timing();
    if (summary) {
      char p[64];
      if (r.saved.mode == MODE_OFF) snprintf(p, sizeof p, "off");
      else formatTuple(savedProfile(r.saved), t.valid ? t.preamble[1] : 0, p, sizeof p);
      snprintf(reply, cap, "> radio2=%s; tx.media=%s; cross=%s; share=%s; repeat=%s%s; %s", p,
               policyName(r.saved.media), crossName(r.saved.cross), r.saved.share ? "on" : "off",
               r.saved.relay ? "on" : "off", r.temp2 ? "; tempradio2 on" : "",
               !env.supported ? "unsupported on this radio"
               : r.saved.held != kNotHeld
                   ? (r.saved.mode == MODE_OFF ? "radio2 off for client repeat" : "radio2 set by client repeat")
               : (policyMask(r, r.saved.media) & 2) ? "media on radio2" : "media on radio");
      return res;
    }
    if (status_key) {
      snprintf(reply, cap, "> %s; RX=%lu,%lu TX=%lu,%lu switches=%lu errors=%lu max=%luus preamble=%u,%u",
               modeName(active.mode), (unsigned long)env.stats.rx[0], (unsigned long)env.stats.rx[1],
               (unsigned long)env.stats.tx[0], (unsigned long)env.stats.tx[1], (unsigned long)env.stats.switches,
               (unsigned long)env.stats.errors, (unsigned long)env.stats.max_us, env.main_preamble,
               t.valid ? t.preamble[1] : 0);
      if (env.stats.typ_us) {
        const size_t used = strlen(reply);
        if (used < cap) snprintf(reply + used, cap - used, " typ=%luus", (unsigned long)env.stats.typ_us);
      }
      if (env.stats.heard_s != 0xFFFFFFFF) {   // the app's "MediaMesh nearby" indicator reads it
        const size_t used = strlen(reply);
        if (used < cap) snprintf(reply + used, cap - used, " heard=%lus", (unsigned long)env.stats.heard_s);
      }
      if (env.stats.stalls) {
        const size_t used = strlen(reply);
        if (used < cap) snprintf(reply + used, cap - used, " stalls=%lu", (unsigned long)env.stats.stalls);
      }
      if (env.stats.waits) {   // radio2 packets that waited for radio2 to go quiet
        const size_t used = strlen(reply);
        if (used < cap) snprintf(reply + used, cap - used, " lbt=%lu", (unsigned long)env.stats.waits);
      }
      if (env.stats.dropped) {   // packets meant for radio2 that never went out
        const size_t used = strlen(reply);
        if (used < cap) snprintf(reply + used, cap - used, " drop=%lu", (unsigned long)env.stats.dropped);
      }
      {   // radio2's settings: two radios that hear nothing from each other on radio2 differ here
        const size_t used = strlen(reply);
        if (used < cap)
          snprintf(reply + used, cap - used, " r2=%.3f/%g/SF%u/CR%u", (double)active.p.freq, (double)active.p.bw,
                   (unsigned)active.p.sf, (unsigned)active.p.cr);
      }
      if (t.valid) {
        const size_t used = strlen(reply);
        const uint8_t s = env.stats.samples > kSwitchSamples ? kSwitchSamples : env.stats.samples;
        if (used < cap) {
          snprintf(reply + used, cap - used, "; test=%u/%u%s budget=%luus", s, kSwitchSamples,
                   s >= kSwitchSamples ? " ready" : " pending", (unsigned long)env.switch_us);
        }
        appendWarnings(t, active.p, reply, cap);
      }
      if (r.temp2) appendLeft(msLeft(env.now_ms, r.temp2_end_ms), reply, cap);
      return res;
    }
    if (!t.valid) { snprintf(reply, cap, "> off"); return res; }
    if (scan_key) {
      snprintf(reply, cap, "> slow=%s; listen_us=%lu,%lu; preamble=%u,%u", t.slow ? "radio2" : "radio",
               (unsigned long)t.dwell_us[0], (unsigned long)t.dwell_us[1], t.preamble[0], t.preamble[1]);
      appendWarnings(t, active.p, reply, cap);
      return res;
    }
    const double T0 = symbolUs(env.main.bw, env.main.sf), T1 = symbolUs(active.p.bw, active.p.sf);
    const uint8_t s = env.stats.samples > kSwitchSamples ? kSwitchSamples : env.stats.samples;
    snprintf(reply, cap, "> chirps=%.2f,%.2f; need=%u,%u; switch=%luus; loop=0us; test=%u/%u%s; preamble=%u,%u",
             T0 > 0 ? t.dwell_us[0] / T0 : 0.0, T1 > 0 ? t.dwell_us[1] / T1 : 0.0, t.need[0], t.need[1],
             (unsigned long)t.switch_us, s, kSwitchSamples, s >= kSwitchSamples ? " ready" : " pending",
             t.preamble[0], t.preamble[1]);
    if (!t.main_ok) {
      const size_t used = strlen(reply);
      if (used < cap) snprintf(reply + used, cap - used, "; WARN main preamble too short for scanning");
    }
    appendWarnings(t, active.p, reply, cap);
    return res;
  }

  // ---- tx.media ----
  if (media) {
    // Worded as mikecarper's keymindCascade answers, so apps and scripts read
    // both firmwares alike: `> tx.media radio2; active=radio2`,
    // `OK - tx.media auto; active=channel/global`, and
    // ` (radio2 unavailable for TX)` when radio2 can't send.
    auto describe = [&](const char* lead, uint8_t policy) {
      const uint8_t m = policyMask(r, policy);
      snprintf(reply, cap, "%s tx.media %s; active=%s%s", lead, policyName(policy),
               policy == TX_AUTO ? "channel/global" : maskName(m),
               (policy == TX_RADIO2 || policy == TX_BOTH) && !(m & 2) ? " (radio2 unavailable for TX)" : "");
    };
    if (verb == GET && !*arg) {
      describe(">", r.saved.media);
      return res;
    }
    uint8_t p;
    if (verb != SET || !parsePolicy(arg, p)) {
      snprintf(reply, cap, "Error: use get tx.media or set tx.media auto|radio|radio2|both|off");
      return res;
    }
    if (!env.supported && (p == TX_RADIO2 || p == TX_BOTH)) {
      snprintf(reply, cap, "Error: radio2 is not supported on this radio");
      return res;
    }
    res.save = p != r.saved.media;
    r.saved.media = p;
    describe("OK -", p);
    return res;
  }

  // ---- mediamesh.main (removed developer switch) ----
  // Photos and voice never use the main mesh. "off" is accepted so old
  // scripts and apps that turn it off still work.
  if (main_key) {
    if (verb == GET && !*arg) { snprintf(reply, cap, "> off"); return res; }
    if (verb == SET && !strcmp(arg, "off")) { snprintf(reply, cap, "OK - mediamesh.main off"); return res; }
    snprintf(reply, cap, "Error: photos and voice only go on MediaMesh (radio2)");
    return res;
  }

  // ---- mediamesh.share ----
  // Removed: photo and voice requests and answers stay on radio2. "off" is
  // accepted, since MeshCore Open TAK turns it off when saving MediaMesh.
  if (share) {
    if (verb == GET && !*arg) { snprintf(reply, cap, "> off"); return res; }
    if (verb == SET && !strcmp(arg, "off")) {
      res.save = r.saved.share != 0;
      r.saved.share = 0;
      snprintf(reply, cap, "OK - mediamesh.share off");
      return res;
    }
    snprintf(reply, cap, "Error: photos and voice only go on MediaMesh (radio2)");
    return res;
  }

  // ---- radio2.repeat ----
  if (repeat) {
    if (verb == GET && !*arg) {
      snprintf(reply, cap, "> %s%s", r.saved.relay ? "on" : "off",
               r.saved.relay && !canTransmit(r, 1) ? " (radio2 isn't sending: nothing is relayed)" : "");
      return res;
    }
    if (verb != SET || (strcmp(arg, "on") && strcmp(arg, "off"))) {
      snprintf(reply, cap, "Error: use get radio2.repeat or set radio2.repeat on|off");
      return res;
    }
    const uint8_t v = !strcmp(arg, "on") ? 1 : 0;
    res.save = v != r.saved.relay;
    r.saved.relay = v;
    snprintf(reply, cap, "OK - radio2.repeat %s (%s)", v ? "on" : "off",
             v ? "relaying what radio2 hears, on radio2" : "not relaying radio2");
    return res;
  }

  // ---- radio2.cross ----
  if (cross) {
    if (verb == GET && !*arg) { snprintf(reply, cap, "> %s", crossName(r.saved.cross)); return res; }
    if (verb != SET || (strcmp(arg, "auto") && strcmp(arg, "on") && strcmp(arg, "off"))) {
      snprintf(reply, cap, "Error: use get/set radio2.cross [auto|on|off]");
      return res;
    }
    const uint8_t c = !strcmp(arg, "on") ? CROSS_ON : !strcmp(arg, "off") ? CROSS_OFF : CROSS_AUTO;
    res.save = c != r.saved.cross;
    r.saved.cross = c;
    snprintf(reply, cap, "OK");
    return res;
  }

  // ---- tempradio / normalradio (the primary profile) ----
  if (temp1 || normal) {
    if (normal) {
      res.apply_primary = r.temp1;
      r.temp1 = false;
      snprintf(reply, cap, "OK - normal radio restored");
      return res;
    }
    if (!*arg) {
      if (!r.temp1) { snprintf(reply, cap, "TempRadio inactive"); return res; }
      snprintf(reply, cap, "TempRadio active: %.3f,%.2f,%u,%u %lus left", r.temp1_p.freq, r.temp1_p.bw, r.temp1_p.sf,
               r.temp1_p.cr, (unsigned long)((msLeft(env.now_ms, r.temp1_end_ms) + 999) / 1000));
      return res;
    }
    if (verb == GET) { snprintf(reply, cap, "Error: use tempradio freq,bw,sf,cr,minutes[,preamble]"); return res; }
    char buf[96];
    const size_t n = strlen(arg);
    if (n >= sizeof buf) { snprintf(reply, cap, "Error: params too long"); return res; }
    memcpy(buf, arg, n + 1);
    char* f[7] = {};
    const int count = splitFields(buf, f, 7);
    Params p;
    uint32_t sf = 0, cr = 0, minutes = 0;
    char* e = nullptr;
    bool ok = count == 5 || count == 6;
    if (ok) { p.freq = strtof(f[0], &e); ok = !*e && *f[0]; }
    if (ok) { p.bw = strtof(f[1], &e); ok = !*e && *f[1]; }
    ok = ok && parseUint(f[2], sf) && sf <= 255 && parseUint(f[3], cr) && cr <= 255 && parseUint(f[4], minutes)
         && minutes >= 1 && minutes <= kMaxTempMinutes;
    p.sf = (uint8_t)sf;
    p.cr = (uint8_t)cr;
    if (ok && count == 6) ok = parsePreambleField(f[5], p.preamble);
    if (!ok || !validSubGhz(p)) {   // the main radio stays sub-GHz
      snprintf(reply, cap, "ERR usage: tempradio freq,bw,sf,cr,minutes (minutes 1-%lu)", (unsigned long)kMaxTempMinutes);
      return res;
    }
    if (active.mode != MODE_OFF && sameChannel(p, active.p)) {
      snprintf(reply, cap, "Error: tempradio matches radio2");
      return res;
    }
    r.temp1 = true;
    r.temp1_p = p;
    r.temp1_end_ms = env.now_ms + minutes * 60000UL;
    res.apply_primary = true;
    snprintf(reply, cap, "OK - temp params for %lu mins", (unsigned long)minutes);
    return res;
  }

  // ---- getters for radio2 / tempradio2 ----
  if ((base || temp) && verb == GET && !*arg) {
    if (temp && !r.temp2) { snprintf(reply, cap, "> off"); return res; }
    const Profile2 s = temp ? r.temp2_prof : savedProfile(r.saved);
    formatProfileGet(env, s, reply, cap);
    if (temp) appendLeft(msLeft(env.now_ms, r.temp2_end_ms), reply, cap);
    if (base && r.saved.held != kNotHeld) {
      const size_t used = strlen(reply);
      if (used < cap) {
        snprintf(reply + used, cap - used, "; client repeat is on (radio2 back to %s when it is off)",
                 modeName(r.saved.held));
      }
      return res;
    }
    if (s.mode != MODE_OFF) appendWarnings(previewTiming(env, s.p), s.p, reply, cap);
    return res;
  }

  // ---- schedules: get / del ----
  if (sched && (verb == GET || verb == DEL)) {
    uint32_t index = 0;
    if (*arg && strcmp(arg, "all") && (!parseUint(arg, index) || index < 1 || index > 4)) {
      snprintf(reply, cap, "Error: slot must be 1-4 or all");
      return res;
    }
    const int first = sched_temp ? 4 : 0;
    if (verb == DEL) {
      for (int i = 0; i < 4; ++i) {
        if (index && index != (uint32_t)(i + 1)) continue;
        if (r.sched[first + i].started && r.temp2 && r.temp2_slot == first + i) {
          r.temp2 = false;
          r.temp2_slot = 0xFF;
          res.apply = true;
        }
        r.sched[first + i] = Schedule();
      }
      snprintf(reply, cap, "OK");
      return res;
    }
    if (!index) {
      snprintf(reply, cap, "> slots: ");
      for (int i = 0; i < 4; ++i) {
        if (!r.sched[first + i].active) continue;
        const size_t used = strlen(reply);
        if (used < cap) snprintf(reply + used, cap - used, "%d@%lu ", i + 1, (unsigned long)r.sched[first + i].start);
      }
      return res;
    }
    const Schedule& s = r.sched[first + index - 1];
    if (!s.active) { snprintf(reply, cap, "> off"); return res; }
    formatProfileGet(env, s.prof, reply, cap);
    const size_t used = strlen(reply);
    if (used < cap) snprintf(reply + used, cap - used, "; @%lu-%lu", (unsigned long)s.start, (unsigned long)s.end);
    return res;
  }

  if (verb != SET) {
    snprintf(reply, cap, "Error: invalid radio2 command");
    return res;
  }

  // ---- set ... off ----
  if ((base || temp) && !strcmp(arg, "off")) {
    if (base) {
      res.save = r.saved.mode != MODE_OFF;
      r.saved.mode = MODE_OFF;
      for (auto& s : r.sched) s = Schedule();
    }
    res.apply = r.temp2 || base;
    endTemp2(r);
    snprintf(reply, cap, "OK - %s", !base && r.saved.mode != MODE_OFF ? "saved radio2 restored" : "single radio");
    return res;
  }

  if (!env.supported) {
    snprintf(reply, cap, "Error: radio2 is not supported on this radio");
    return res;
  }

  // ---- set radio2 / tempradio2 / radioat2 / tempradioat2 <tuple> ----
  Profile2 prof;
  const char* extra[2] = {};
  const int n_extra = sched_temp ? 2 : (sched || temp) ? 1 : 0;
  char buf[140];
  const char* why = "";
  if (!parseTuple(arg, prof, extra, n_extra, buf, sizeof buf, &why)) {
    if (base && strcmp(why, "fields") != 0) snprintf(reply, cap, "Error: %s", why);
    else {
      snprintf(reply, cap, "Error: use %s f,bw,sf,cr,rx|rxtx%s[,preamble|auto]", key,
               sched_temp ? ",start,end (UTC or +minutes)" : sched ? ",start (UTC or +minutes)" : temp ? ",minutes" : "");
    }
    return res;
  }
  if (sameChannel(prof.p, env.main) || sameChannel(prof.p, env.main_saved)) {
    snprintf(reply, cap, "Error: radio2 matches the main radio");
    return res;
  }
  const Timing preview = previewTiming(env, prof.p);
  const uint16_t pre = preview.valid ? preview.preamble[1] : 0;

  if (sched) {
    uint32_t start = 0, end = 0;
    const uint32_t epoch = env.epoch;
    bool ok = parseScheduleTime(extra[0], epoch, start);
    if (ok && sched_temp) ok = parseScheduleTime(extra[1], epoch, end);
    if (!ok) {
      snprintf(reply, cap, "Error: use %s f,bw,sf,cr,rx|rxtx,%s[,preamble|auto]", key,
               sched_temp ? "start,end (UTC or +minutes)" : "start (UTC or +minutes)");
      return res;
    }
    if (!epoch || start <= epoch || start - epoch > kMaxScheduleSeconds
        || (sched_temp && (end <= start || end - epoch > kMaxScheduleSeconds))) {
      snprintf(reply, cap, "Error: future UTC start/end required, within 24 days");
      return res;
    }
    const uint32_t start_ms = env.now_ms + (start - epoch) * 1000UL;
    if (sched_temp) {
      if (r.temp2 && (int32_t)(r.temp2_end_ms - start_ms) > 0) {
        snprintf(reply, cap, "Error: temporary schedule overlaps active session");
        return res;
      }
      for (int i = 4; i < 8; ++i) {
        const Schedule& s = r.sched[i];
        if (s.active && start < s.end && end > s.start) {
          snprintf(reply, cap, "Error: temporary schedules overlap");
          return res;
        }
      }
    }
    const int first = sched_temp ? 4 : 0;
    for (int i = 0; i < 4; ++i) {
      Schedule& s = r.sched[first + i];
      if (s.active) continue;
      s = Schedule();
      s.prof = prof;
      s.start = start;
      s.end = end;
      s.start_ms = start_ms;
      s.end_ms = sched_temp ? env.now_ms + (end - epoch) * 1000UL : 0;
      s.temporary = sched_temp;
      s.active = true;
      snprintf(reply, cap, "OK - %s %d queued, preamble=%u", key, i + 1, pre);
      appendWarnings(preview, prof.p, reply, cap);
      return res;
    }
    snprintf(reply, cap, "Error: all four schedule slots occupied");
    return res;
  }

  if (temp) {
    if (r.hasTempSchedule()) {
      snprintf(reply, cap, "Error: clear tempradioat2 schedules first");
      return res;
    }
    uint32_t minutes = 0;
    if (!parseUint(extra[0], minutes) || minutes < 1 || minutes > kMaxTempMinutes) {
      snprintf(reply, cap, "Error: use tempradio2 f,bw,sf,cr,rx|rxtx,minutes[,preamble|auto]");
      return res;
    }
    r.temp2 = true;
    r.temp2_prof = prof;
    r.temp2_end_ms = env.now_ms + minutes * 60000UL;
    r.temp2_slot = 0xFF;
    res.apply = true;
    snprintf(reply, cap, "OK - tempradio2 %lud%luh%lum (%lu min); preamble=%u", (unsigned long)(minutes / 1440),
             (unsigned long)(minutes / 60 % 24), (unsigned long)(minutes % 60), (unsigned long)minutes, pre);
    appendWarnings(preview, prof.p, reply, cap);
    return res;
  }

  // set radio2 <tuple>
  res.save = !sameParams(prof.p, r.saved.p) || prof.mode != r.saved.mode;
  res.apply = res.save && !r.temp2;
  r.saved.mode = prof.mode;
  r.saved.p = prof.p;
  snprintf(reply, cap, "OK - radio2 %s; preamble=%u", modeName(prof.mode), pre);
  appendWarnings(preview, prof.p, reply, cap);
  return res;
}

// ---- timers ---------------------------------------------------------------
// Ends temporary sessions and starts schedules. Call every loop.

struct TickResult {
  bool save = false;
  bool apply = false;
  bool apply_primary = false;
};

inline TickResult tick(Runtime& r, uint32_t now_ms) {
  TickResult t;
  if (r.temp1 && deadlinePassed(now_ms, r.temp1_end_ms)) {
    r.temp1 = false;
    t.apply_primary = true;
  }
  if (r.temp2 && deadlinePassed(now_ms, r.temp2_end_ms)) {
    if (r.temp2_slot < 8) r.sched[r.temp2_slot] = Schedule();
    r.temp2 = false;
    r.temp2_slot = 0xFF;
    t.apply = true;
  }
  for (int i = 0; i < 8; ++i) {
    Schedule& s = r.sched[i];
    if (!s.active || s.started || !deadlinePassed(now_ms, s.start_ms)) continue;
    if (!s.temporary) {
      // radioat2: becomes the saved second profile.
      r.saved.mode = s.prof.mode;
      r.saved.p = s.prof.p;
      s = Schedule();
      t.save = true;
      t.apply = t.apply || !r.temp2;
    } else if (deadlinePassed(now_ms, s.end_ms)) {
      s = Schedule();   // missed entirely (a long stall): nothing to run
    } else {
      s.started = true;
      r.temp2 = true;
      r.temp2_prof = s.prof;
      r.temp2_end_ms = s.end_ms;
      r.temp2_slot = (uint8_t)i;
      t.apply = true;
    }
  }
  return t;
}

}  // namespace mediamesh

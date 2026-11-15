// SPDX-License-Identifier: GPL-3.0-or-later
//
// MediaMesh on an LR1121 / LR2021 (MEDIAMESH_HAS_24GHZ): radio2 may be 2.4 GHz
// LoRa, the main radio and tempradio stay sub-GHz. Also radio2.repeat (Repeat
// MediaMesh): saved, set from the CLI, and only active while radio2 can send.
// test_mediamesh_core.cpp covers a board without 2.4 GHz.

#define MEDIAMESH_HAS_24GHZ 1

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "helpers/MediaMeshCore.h"

using namespace mediamesh;

static Params P(float f, float bw, uint8_t sf, uint8_t cr) {
  Params p; p.freq = f; p.bw = bw; p.sf = sf; p.cr = cr; return p;
}

static Env E() {
  Env e;
  e.main = e.main_saved = P(910.525f, 62.5f, 7, 5);
  e.main_preamble = 32;
  e.switch_us = 840;
  e.now_ms = 1000;
  e.epoch = 1800000000UL;
  return e;
}

static void testValidity() {
  assert(has24GHz());
  assert(validParams(P(k24Freq, k24Bw, k24Sf, 5)));
  assert(validParams(P(2440.0f, 406.25f, 8, 5)));
  assert(validParams(P(2410.0f, 203.125f, 9, 6)));
  // Sub-GHz widths don't apply at 2.4 GHz, nor 2.4 GHz widths below 1 GHz.
  assert(!validParams(P(k24Freq, 250.0f, 7, 5)));
  assert(!validParams(P(911.15f, k24Bw, 7, 5)));
  // Outside the 2.4 GHz ISM band.
  assert(!validParams(P(2399.0f, k24Bw, 7, 5)));
  assert(!validParams(P(2501.0f, k24Bw, 7, 5)));
  // The main radio stays sub-GHz.
  assert(!validSubGhz(P(k24Freq, k24Bw, 7, 5)));
  assert(validSubGhz(P(911.15f, 250.0f, 7, 5)));
  // The preset is about as fast as the 900 MHz 250 kHz preset (within 10%).
  const double r24 = k24Sf * (k24Bw * 1000.0) / (1 << k24Sf) * 0.8;
  const double r900 = 7 * 250000.0 / (1 << 7) * 0.8;
  assert(r24 > r900 * 0.9 && r24 < r900 * 1.1);
  // The preset sits between Wi-Fi channels 1 (to 2423) and 6 (from 2426).
  assert(k24Freq - k24Bw / 2000.0f > 2423.0f && k24Freq + k24Bw / 2000.0f < 2426.0f);
}

static void testCli24() {
  Runtime r;
  defaults(r.saved);
  Env e = E();
  char out[200];
  CliResult res = handleCli("set radio2 2424.5,812.5,7,5,rxtx", r, e, out, sizeof out);
  assert(res.handled && res.save && !strncmp(out, "OK - radio2 rxtx", 16));
  assert(is24GHz(r.saved.p.freq) && r.saved.p.bw == 812.5f);
  // A 2.4 GHz radio2 still gets a valid scan timing next to the main mesh,
  // the preset included.
  const Timing t = previewTiming(e, r.saved.p);
  assert(t.valid);
  assert(previewTiming(e, P(k24Freq, k24Bw, k24Sf, 5)).valid);
  res = handleCli("set radio2 2424.5,250,7,5,rxtx", r, e, out, sizeof out);
  assert(res.handled && !res.save && strstr(out, "rror"));
  // tempradio (the main radio) refuses 2.4 GHz.
  res = handleCli("set tempradio 2424.5,812.5,7,5,30", r, e, out, sizeof out);
  assert(res.handled && !strncmp(out, "ERR", 3));
}

static void testRepeat() {
  Runtime r;
  defaults(r.saved);
  Env e = E();
  char out[200];
  assert(r.saved.relay == 0 && !relaysRadio2(r));
  CliResult res = handleCli("set radio2.repeat on", r, e, out, sizeof out);
  assert(res.handled && res.save && r.saved.relay == 1);
  // radio2 off: nothing to relay on.
  assert(!relaysRadio2(r));
  handleCli("get radio2.repeat", r, e, out, sizeof out);
  assert(!strncmp(out, "> on (radio2 isn't sending", 26));
  handleCli("set radio2 2424.5,812.5,7,5,rxtx", r, e, out, sizeof out);
  assert(relaysRadio2(r));
  handleCli("get radio2.repeat", r, e, out, sizeof out);
  assert(!strcmp(out, "> on"));
  // Listen-only radio2 can't relay.
  handleCli("set radio2 2424.5,812.5,7,5,rx", r, e, out, sizeof out);
  assert(!relaysRadio2(r));
  handleCli("get mediamesh", r, e, out, sizeof out);
  assert(strstr(out, "repeat=on"));
  res = handleCli("set radio2.repeat maybe", r, e, out, sizeof out);
  assert(res.handled && !res.save && strstr(out, "Error"));
  res = handleCli("set radio2.repeat off", r, e, out, sizeof out);
  assert(res.save && r.saved.relay == 0);

  // Saved in the blob (v8); a v6 blob (22 bytes) still reads, a v5 one as off.
  Config c;
  defaults(c);
  c.mode = MODE_RXTX;
  c.p = P(k24Freq, k24Bw, 7, 5);
  c.relay = 1;
  uint8_t b[kBlobSize];
  assert(encode(c, b) == kBlobSize && b[2] == kBlobVersion);
  Config d;
  assert(decode(b, sizeof b, d) && d.relay == 1 && is24GHz(d.p.freq));
  assert(!decode(b, kBlobSizeV6, d));   // v8 cut short
  b[2] = 6;
  assert(decode(b, kBlobSizeV6, d) && d.relay == 1 && d.held_p.freq == 0);
  assert(!decode(b, kBlobSizeV5, d));   // v6 cut short
  b[2] = 5;
  assert(decode(b, kBlobSizeV5, d) && d.relay == 0);
}

// Client Repeat holds a 2.4 GHz radio2 off too, like a 900 MHz one, and moving
// a held radio2 to 2.4 GHz doesn't bring it back.
static void testClientRepeat() {
  Runtime r;
  defaults(r.saved);
  r.saved.mode = MODE_RXTX;
  r.saved.p = P(k24Freq, k24Bw, 7, 5);
  assert(holdForClientRepeat(r, true) && r.saved.mode == MODE_OFF && r.saved.held == MODE_RXTX);
  assert(!holdForClientRepeat(r, true));   // steady
  r.saved.p = P(911.15f, 250.0f, 7, 5);
  assert(!holdForClientRepeat(r, true) && r.saved.mode == MODE_OFF);
  r.saved.p = P(k24Freq, k24Bw, 7, 5);
  assert(!holdForClientRepeat(r, true) && r.saved.mode == MODE_OFF && r.saved.held == MODE_RXTX);
  // Off puts it back as it was.
  assert(holdForClientRepeat(r, false) && r.saved.mode == MODE_RXTX && r.saved.held == kNotHeld);
  assert(!holdForClientRepeat(r, false));  // nothing held
}

// The build allows 2.4 GHz, but the chip says it can't (an LR1110 where an
// LR1121 was expected): radio2 stays sub-GHz.
static void testChipSaysNo() {
  setHas24GHz(false);
  assert(!has24GHz() && !validParams(P(k24Freq, k24Bw, k24Sf, 5)));
  assert(validParams(P(911.15f, 250.0f, 7, 5)));
  setHas24GHz(true);
  assert(has24GHz() && validParams(P(k24Freq, k24Bw, k24Sf, 5)));
}

int main() {
  testChipSaysNo();
  testClientRepeat();
  testValidity();
  testCli24();
  testRepeat();
  printf("mediamesh 2.4 GHz and repeat: ok\n");
  return 0;
}

// SPDX-License-Identifier: GPL-3.0-or-later
//
// Sending coverage points to an analyzer (src/helpers/MediaMeshCoverageShare.h):
// what CoreScope's POST /api/mediamesh/coverage-points expects, as MeshCore
// Open TAK's test/services/mediamesh_coverage_share_test.dart checks.

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "helpers/MediaMeshCoverageShare.h"

using namespace mmshare;

static bool ep(const char* in, const char* want) {
  char out[200];
  const bool ok = endpointFor(in, out, sizeof out);
  if (!want) return !ok;
  if (!ok || strcmp(out, want)) { printf("%s -> %s\n", in, ok ? out : "(none)"); return false; }
  return true;
}

int main() {
  assert(ep(kDefaultAnalyzer, "https://analyzer.cascadiamesh.org/api/mediamesh/coverage-points"));
  assert(ep(" mesh.example.org/ ", "https://mesh.example.org/api/mediamesh/coverage-points"));
  assert(ep("http://192.168.1.5:3000/corescope/?x=1", "http://192.168.1.5:3000/corescope/api/mediamesh/coverage-points"));
  assert(ep("https://analyzer.cascadiamesh.org/api/mediamesh/coverage-points/", "https://analyzer.cascadiamesh.org/api/mediamesh/coverage-points"));
  assert(ep("analyzer.cascadiamesh.org", kDefaultAnalyzer));
  assert(ep("", nullptr));
  assert(ep("ftp://example.org", nullptr));
  assert(ep("not an address", nullptr));

  Row r;
  assert(!parseRow("epoch,lat,lon,reach,best_margin,repeater,snr,remote_snr,margin,key\n", r));
  assert(!parseRow("", r));
  assert(parseRow("1795000000,47.610000,-122.325000,none,,,,,\n", r) && r.none && r.epoch == 1795000000u);
  assert(parseRow("1795000060,47.611000,-122.325000,good,6.0,First Hill,4.2,2.5,6.0,1EA5E195\n", r));
  assert(!r.none && r.has_key && !strcmp(r.key, "1ea5e195") && r.margin > 5.9f && r.remote > 2.4f);
  assert(parseRow("1795000060,47.611000,-122.325000,good,6.0,Old line,4.2,2.5,6.0\n", r) && !r.none && !r.has_key);

  static char buf[8192];
  Batch b(buf, sizeof buf);
  assert(b.begin("ab01", 1795000500, "Wadamesh TAK", 62500, 250000, 7));
  const char* lines[] = {
      "1795000000,47.610000,-122.325000,none,,,,,\n",
      "1795000060,47.611000,-122.325000,good,6.0,First Hill,4.2,2.5,6.0,1ea5e195\n",
      "1795000060,47.611000,-122.325000,good,6.0,SoDo,1.0,0.5,-1.5,5d01cacf\n",
      "1795000120,47.612000,-122.325000,good,6.0,Old,4.2,2.5,6.0\n",   // no key: left out
      "1795000180,47.613000,-122.325000,marginal,2.0,First Hill,0.2,0.1,2.0,1ea5e195\n",
  };
  for (const char* l : lines) { assert(parseRow(l, r)); assert(b.add(r)); }
  assert(b.finish());
  assert(b.samples() == 3);
  assert(b.lastEpoch() == 1795000180u);
  const char* want =
      "{\"v\":1,\"radio\":\"ab01\",\"sent\":1795000500,\"app\":\"Wadamesh TAK\",\"radio2\":{\"bw_hz\":250000,\"sf\":7},\"main_bw_hz\":62500,"
      "\"points\":[{\"t\":1795000000,\"lat\":47.610000,\"lon\":-122.325000,\"reps\":[]},"
      "{\"t\":1795000060,\"lat\":47.611000,\"lon\":-122.325000,\"speed\":1.9,\"reps\":["
      "{\"key\":\"1ea5e195\",\"snr\":4.20,\"remote_snr\":2.50,\"margin\":6.0},{\"key\":\"5d01cacf\",\"snr\":1.00,\"remote_snr\":0.50,\"margin\":-1.5}]},"
      "{\"t\":1795000180,\"lat\":47.613000,\"lon\":-122.325000,\"speed\":1.9,\"reps\":[{\"key\":\"1ea5e195\",\"snr\":0.20,\"remote_snr\":0.10,\"margin\":2.0}]}]}";
  if (strcmp(buf, want)) { printf("%s\n", buf); assert(false); }

  // A full batch stops at a sample's first line, and takes nothing of it.
  static char small[1600];
  Batch s(small, sizeof small);
  assert(s.begin("ab01", 1, "x", 62500, 250000, 7));
  int taken = 0;
  for (int i = 0; i < 20; i++) {
    char l[120];
    snprintf(l, sizeof l, "%d,47.6%04d,-122.3,good,6.0,R,1,1,6,1ea5e195\n", 1795000000 + i * 60, i);
    assert(parseRow(l, r));
    if (!s.add(r)) break;
    taken++;
  }
  assert(taken > 0 && taken < 20 && s.finish() && s.samples() == taken);
  assert(s.lastEpoch() == (uint32_t)(1795000000 + (taken - 1) * 60));

  char msg[96];
  const uint8_t empty_json[32] = {0x44, 0x13, 0x6f, 0xa3, 0x55, 0xb3, 0x67, 0x8a, 0x11, 0x46, 0xad, 0x16, 0xf7, 0xe8, 0x64, 0x9e,
                                  0x94, 0xfb, 0x4f, 0xc2, 0x1f, 0xe7, 0x7e, 0x83, 0x10, 0xc0, 0x60, 0xf6, 0x1c, 0xaa, 0xff, 0x8a};
  signedMessage(msg, sizeof msg, kSignPrefix, empty_json);
  assert(!strcmp(msg, "corescope-coverage:44136fa355b3678a1146ad16f7e8649e94fb4fc21fe77e8310c060f61caaff8a"));
  char del[120];
  deleteBody(del, sizeof del, "ab01", 5);
  assert(!strcmp(del, "{\"radio\":\"ab01\",\"sent\":5}"));
  char err[64];
  replyError("{\"error\":\"this analyzer hasn't heard this radio yet\"}", err, sizeof err);
  assert(!strcmp(err, "this analyzer hasn't heard this radio yet"));
  replyError("<html>", err, sizeof err);
  assert(!err[0]);
  printf("mediamesh coverage share: ok\n");
  return 0;
}

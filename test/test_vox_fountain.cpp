// Host test for src/ui-touch/VoxFountain.h (rateless repair packets).
//   g++ -std=c++17 -Wall -Wextra -Werror -I src -o /tmp/tvf test/test_vox_fountain.cpp && /tmp/tvf
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui-touch/VoxFountain.h"

static uint32_t rng = 12345;
static uint32_t rnd() { rng = rng * 1103515245u + 12345u; return rng >> 8; }

int main() {
  // The same coefficients as the app (scripts/vox/fountain_ref.py).
  const int want0[4] = {170, 198, 111, 183};
  for (int i = 0; i < 4; i++) assert(VoxFountain::coef(0, i) == want0[i]);
  const int want200[4] = {217, 252, 244, 168};
  for (int i = 0; i < 4; i++) assert(VoxFountain::coef(200, 250 + i) == want200[i]);
  for (int s = 0; s < 255; s += 7)
    for (int i = 0; i < 255; i += 3) assert(VoxFountain::coef(s, i) != 0);

  const int p = 156;
  int exact_ok = 0, trials = 40;
  for (int t = 0; t < trials; t++) {
    const int total = 20 + (int)(rnd() % 200);
    const int last_len = 1 + (int)(rnd() % p);
    uint8_t* bits = (uint8_t*)calloc((size_t)total, p);
    for (int i = 0; i < total; i++) {
      const int len = i == total - 1 ? last_len : p;
      for (int k = 0; k < len; k++) bits[i * p + k] = (uint8_t)rnd();
    }
    const int m = 1 + (int)(rnd() % (uint32_t)(total < 30 ? total : 30));
    bool lost[256] = {false};
    int nl = 0;
    while (nl < m) { const int i = (int)(rnd() % total); if (!lost[i]) { lost[i] = true; nl++; } }
    uint8_t* buf = (uint8_t*)calloc((size_t)total, p);
    for (int i = 0; i < total; i++) if (!lost[i]) memcpy(buf + i * p, bits + i * p, (size_t)p);
    for (int extra = 0; extra <= 1; extra++) {
      const int n = m + extra;
      uint8_t* pk = (uint8_t*)malloc((size_t)n * VoxFountain::kMaxPacket);
      VoxFountain::Repair rep[64];
      const int start = (int)(rnd() % 255);
      for (int k = 0; k < n; k++) {
        uint8_t* o = pk + k * VoxFountain::kMaxPacket;
        const int len = VoxFountain::encode(0x1a2b3c4d, bits, total, p, last_len, (start + k) % 255, o,
                                            VoxFountain::kMaxPacket);
        assert(len == VoxFountain::kHeader + p);
        assert(o[0] == 0x56 && o[1] == (uint8_t)((0x1a2b3c4du ^ VoxFountain::kSidXor) >> 24));
        rep[k] = VoxFountain::Repair{o[5], (uint8_t)(len - 6), o + 6};
      }
      uint8_t* out = (uint8_t*)calloc((size_t)total, p);
      bool got[256] = {false};
      const int r = VoxFountain::rebuild(
          total, p, buf, [&](int i) { return !lost[i]; }, rep, n,
          [&](int i, const uint8_t* d, int len) { memcpy(out + i * p, d, (size_t)len); got[i] = true; });
      if (extra == 0) {
        if (r == m) exact_ok++;
      } else {
        assert(r == m);
        for (int i = 0; i < total; i++)
          if (lost[i]) {
            assert(got[i]);
            assert(memcmp(out + i * p, bits + i * p, (size_t)(i == total - 1 ? last_len : p)) == 0);
          }
      }
      free(out);
      free(pk);
    }
    free(buf);
    free(bits);
  }
  assert(exact_ok >= trials - 3);
  // Too few: nothing.
  assert(VoxFountain::needed(0) == 0 && VoxFountain::needed(40) >= 46);
  assert(VoxFountain::firstPass(217) == 23);
  printf("vox fountain: ok (%d/%d with exactly m)\n", exact_ok, trials);
  return 0;
}

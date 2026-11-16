// A person's hue (src/ui-touch/NameHue.h) against hues measured from the
// official MeshCore app's avatars; MeshCore Open TAK's chat_look_test.dart
// checks the same names.
#include <stdio.h>

#include "ui-touch/NameHue.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

int main() {
  struct { const char* name; int hue; } k[] = {
      {"Dupont C2b", 4},
      {"BGMatt", 120},
      {"Thalestr \xF0\x9F\x90\xA0", 227},                            // tropical fish
      {"Chuck@\xF0\x9F\x8F\xA0", 249},                               // house
      {"Mark MC 2b \xE2\x98\x95", 199},                              // hot beverage
      {"Csmc Mffn \xF0\x9F\x94\x91\xF0\x9F\xA7\xA0 ACOS T096", 75},  // key, brain
      {"\xF0\x9F\xA6\x80", 347},                                     // crab
  };
  for (auto& c : k) {
    const int h = namehue::hue(c.name);
    if (h != c.hue) printf("%s: %d, want %d\n", c.name, h, c.hue);
    CHECK(h == c.hue);
  }
  CHECK(namehue::hue(nullptr) == namehue::hue(""));
  CHECK(namehue::hue("\xE2\x98") >= 0);   // cut short: still a hue
  if (g_fail) return 1;
  printf("name hue: all passed\n");
  return 0;
}

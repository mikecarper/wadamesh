// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A person's colour, as a hue from their name: the official MeshCore app's
// formula, so a person has the same colour on a standalone, in that app and in
// MeshCore Open TAK (lib/theme/chat_look.dart, classicNameHue). FNV-1a over the
// name's UTF-16 code units (as a Dart string holds them), mod 360. Malformed
// UTF-8 counts its bytes one by one. Host-tested (test/test_name_hue.cpp) on
// names whose colours were measured from the official app.

#include <stdint.h>

namespace namehue {

inline int hue(const char* name) {
  uint32_t h = 2166136261u;
  auto unit = [&](uint32_t u) { h ^= u; h *= 16777619u; };
  const uint8_t* p = (const uint8_t*)(name ? name : "");
  while (*p) {
    uint32_t cp = *p;
    int more = 0;
    if (cp >= 0xF0 && cp <= 0xF4) { cp &= 0x07; more = 3; }
    else if (cp >= 0xE0) { cp &= 0x0F; more = 2; }
    else if (cp >= 0xC2 && cp <= 0xDF) { cp &= 0x1F; more = 1; }
    int k = 1;
    for (; k <= more && (p[k] & 0xC0) == 0x80; ++k) cp = (cp << 6) | (p[k] & 0x3F);
    if (k <= more) { unit(*p++); continue; }   // cut short: the lead byte alone
    p += 1 + more;
    if (cp >= 0x10000) {   // a surrogate pair, as UTF-16 has it
      cp -= 0x10000;
      unit(0xD800 | (cp >> 10));
      unit(0xDC00 | (cp & 0x3FF));
    } else {
      unit(cp);
    }
  }
  return (int)(h % 360u);
}

}  // namespace namehue

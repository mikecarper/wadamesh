// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "../helpers/TrackerProtocol.h"
#include <string.h>

namespace bike {
struct RideTag {
  uint8_t key[32]{};
  uint8_t enabled = 0, answer = mesh::tracker::Safe;
  uint16_t battery_mv = 0;
  uint32_t heard = 0, position = 0;
  int32_t latitude = 0, longitude = 0;  // ten-millionths of a degree

  bool accept(const uint8_t* sender, uint32_t tag, uint32_t now, const uint8_t* data, size_t len) {
    mesh::tracker::StatusRequest request{};
    if (!enabled || memcmp(key, sender, sizeof(key)) || !tag || tag <= heard ||
        now < 1700000000UL || (now > tag ? now - tag : tag - now) > 300 ||
        !mesh::tracker::parseStatusRequest(data, len, request)) return false;
    heard = tag;
    battery_mv = request.battery_mv;
    if (request.flags & mesh::tracker::FreshGps) {
      latitude = request.latitude; longitude = request.longitude; position = tag;
    }
    return true;
  }
};
}

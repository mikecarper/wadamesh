// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace gameboy_link {

enum class PacketType : uint8_t {
  Discover = 1,
  Advertise = 2,
  Join = 3,
  Accept = 4,
  KeepAlive = 5,
  Transfer = 6,
  Reply = 7,
  Bye = 8,
};

struct Packet {
  PacketType type = PacketType::Discover;
  uint8_t flags = 0;
  uint8_t value = 0;
  uint32_t session = 0;
  uint32_t sender = 0;
  uint32_t target = 0;
  uint32_t sequence = 0;
  uint32_t game_id = 0;
};

static constexpr uint8_t kVersion = 1;
static constexpr size_t kWireSize = 28;
static constexpr uint8_t kInternalClock = 0x01;

inline void putU32(uint8_t* out, uint32_t value) {
  out[0] = (uint8_t)value;
  out[1] = (uint8_t)(value >> 8);
  out[2] = (uint8_t)(value >> 16);
  out[3] = (uint8_t)(value >> 24);
}

inline uint32_t getU32(const uint8_t* in) {
  return (uint32_t)in[0]
       | ((uint32_t)in[1] << 8)
       | ((uint32_t)in[2] << 16)
       | ((uint32_t)in[3] << 24);
}

inline void encode(const Packet& packet, uint8_t out[kWireSize]) {
  out[0] = 'W';
  out[1] = 'G';
  out[2] = 'B';
  out[3] = 'L';
  out[4] = kVersion;
  out[5] = (uint8_t)packet.type;
  out[6] = packet.flags;
  out[7] = packet.value;
  putU32(out + 8, packet.session);
  putU32(out + 12, packet.sender);
  putU32(out + 16, packet.target);
  putU32(out + 20, packet.sequence);
  putU32(out + 24, packet.game_id);
}

inline bool decode(const uint8_t* in, size_t len, Packet& packet) {
  if (!in || len != kWireSize || in[0] != 'W' || in[1] != 'G'
      || in[2] != 'B' || in[3] != 'L' || in[4] != kVersion) return false;
  const uint8_t type = in[5];
  if (type < (uint8_t)PacketType::Discover || type > (uint8_t)PacketType::Bye)
    return false;
  packet.type = (PacketType)type;
  packet.flags = in[6];
  packet.value = in[7];
  packet.session = getU32(in + 8);
  packet.sender = getU32(in + 12);
  packet.target = getU32(in + 16);
  packet.sequence = getU32(in + 20);
  packet.game_id = getU32(in + 24);
  return true;
}

}  // namespace gameboy_link

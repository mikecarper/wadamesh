#include <assert.h>
#include <string.h>

#include "ui-touch/GameBoyLinkProtocol.h"

int main() {
  using namespace gameboy_link;

  static_assert(kWireSize == 28, "Game Boy WiFi packet size changed");

  Packet source;
  source.type = PacketType::Transfer;
  source.flags = kInternalClock;
  source.value = 0xA5;
  source.session = 0x12345678u;
  source.sender = 0x10203040u;
  source.target = 0x50607080u;
  source.sequence = 0x90ABCDEFu;
  source.game_id = 0x0BADF00Du;

  uint8_t wire[kWireSize];
  encode(source, wire);
  assert(memcmp(wire, "WGBL", 4) == 0);
  assert(wire[4] == kVersion);
  assert(wire[5] == (uint8_t)PacketType::Transfer);
  assert(wire[8] == 0x78 && wire[11] == 0x12);

  Packet decoded;
  assert(decode(wire, sizeof(wire), decoded));
  assert(decoded.type == source.type);
  assert(decoded.flags == source.flags);
  assert(decoded.value == source.value);
  assert(decoded.session == source.session);
  assert(decoded.sender == source.sender);
  assert(decoded.target == source.target);
  assert(decoded.sequence == source.sequence);
  assert(decoded.game_id == source.game_id);

  assert(!decode(wire, sizeof(wire) - 1, decoded));
  wire[0] = 'X';
  assert(!decode(wire, sizeof(wire), decoded));
  wire[0] = 'W';
  wire[4] = kVersion + 1;
  assert(!decode(wire, sizeof(wire), decoded));
  wire[4] = kVersion;
  wire[5] = 0xFF;
  assert(!decode(wire, sizeof(wire), decoded));
  return 0;
}

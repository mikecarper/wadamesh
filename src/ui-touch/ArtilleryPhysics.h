// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stdint.h>

namespace artillery {

constexpr int kWorldWidth = 320;
constexpr int kWorldHeight = 160;
constexpr int kBlastRadius = 15;
constexpr int kMaxPlayers = 10;

struct Tank {
  int16_t x = 0;
  int8_t health = 3;
  uint8_t angle = 45;
  uint8_t power = 65;
};

struct Shot {
  int32_t x_q16 = 0;
  int32_t y_q16 = 0;
  int32_t vx_q16 = 0;
  int32_t vy_q16 = 0;
  uint16_t age = 0;
  int8_t shooter = -1;
};

enum class ShotEvent : uint8_t {
  InFlight,
  Impact,
  Miss,
};

struct ShotResult {
  ShotEvent event = ShotEvent::InFlight;
  int16_t x = -1;
  int16_t y = -1;
};

void generateTerrain(uint32_t seed, int16_t terrain[kWorldWidth]);
void flattenForTank(int16_t terrain[kWorldWidth], int x);
int tankY(const int16_t terrain[kWorldWidth], const Tank& tank);

void startShot(Shot& shot, const int16_t terrain[kWorldWidth],
               const Tank& tank, int shooter, int angle, int power);
ShotResult stepShot(Shot& shot, int wind,
                    const int16_t terrain[kWorldWidth],
                    const Tank* tanks, int tank_count);
ShotResult simulateShot(Shot& shot, int wind,
                        const int16_t terrain[kWorldWidth],
                        const Tank* tanks, int tank_count);

void damageTank(Tank& tank, const int16_t terrain[kWorldWidth], int x, int y);
void carveCrater(int16_t terrain[kWorldWidth], int x, int y);
void applyImpact(int16_t terrain[kWorldWidth], Tank* tanks, int tank_count,
                 int x, int y);

int windForTurn(uint32_t seed, uint16_t turn);
int shotX(const Shot& shot);
int shotY(const Shot& shot);

uint32_t hashByte(uint32_t hash, uint8_t value);
uint32_t hashU16(uint32_t hash, uint16_t value);
uint32_t hashU32(uint32_t hash, uint32_t value);

}  // namespace artillery

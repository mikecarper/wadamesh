// SPDX-License-Identifier: GPL-3.0-or-later

#include "ArtilleryPhysics.h"

namespace artillery {
namespace {

constexpr int32_t kQ16 = 65536;
constexpr int32_t kTrigScale = 32767;
constexpr int32_t kVelocityScaleQ16 = 5571;  // 0.085 in Q16.
constexpr int32_t kGravityPerSubstepQ16 = 2621;  // 0.12 / 3 in Q16.
constexpr int32_t kWindPerSubstepQ16 = 66;       // 0.003 / 3 in Q16.
constexpr int kSubstepsPerTick = 3;
constexpr int kMaxSimulationTicks = 720;

// sin(0..90 degrees), scaled to 32767. Keeping this table in source avoids
// platform libm differences from entering multiplayer state.
constexpr int16_t kSinQ15[91] = {
  0, 572, 1144, 1715, 2286, 2856, 3425, 3993, 4560, 5126,
  5690, 6252, 6813, 7371, 7927, 8481, 9032, 9580, 10126, 10668,
  11207, 11743, 12275, 12803, 13328, 13848, 14364, 14876, 15383, 15886,
  16383, 16876, 17364, 17846, 18323, 18794, 19260, 19720, 20173, 20621,
  21062, 21497, 21925, 22347, 22762, 23170, 23571, 23964, 24351, 24730,
  25101, 25465, 25821, 26169, 26509, 26841, 27165, 27481, 27788, 28087,
  28377, 28659, 28932, 29196, 29451, 29697, 29934, 30162, 30381, 30591,
  30791, 30982, 31163, 31335, 31498, 31650, 31794, 31927, 32051, 32165,
  32269, 32364, 32448, 32523, 32587, 32642, 32687, 32722, 32747, 32762,
  32767,
};

uint32_t nextRand(uint32_t& state) {
  if (!state) state = 0x6D2B79F5u;
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return state;
}

int32_t divRound(int64_t numerator, int32_t denominator) {
  if (numerator >= 0) return (int32_t)((numerator + denominator / 2) / denominator);
  return (int32_t)((numerator - denominator / 2) / denominator);
}

int sinQ15(int angle) {
  if (angle < 0) angle = -angle;
  angle %= 360;
  if (angle > 180) angle = 360 - angle;
  if (angle > 90) angle = 180 - angle;
  return kSinQ15[angle];
}

int cosQ15(int angle) {
  angle %= 360;
  if (angle < 0) angle += 360;
  if (angle <= 90) return kSinQ15[90 - angle];
  if (angle <= 180) return -kSinQ15[angle - 90];
  if (angle <= 270) return -kSinQ15[270 - angle];
  return kSinQ15[angle - 270];
}

int intSqrt(int value) {
  if (value <= 0) return 0;
  unsigned op = (unsigned)value;
  unsigned result = 0;
  unsigned one = 1u << 30;
  while (one > op) one >>= 2;
  while (one != 0) {
    if (op >= result + one) {
      op -= result + one;
      result = (result >> 1) + one;
    } else {
      result >>= 1;
    }
    one >>= 2;
  }
  return (int)result;
}

ShotResult missResult() {
  ShotResult result;
  result.event = ShotEvent::Miss;
  return result;
}

ShotResult impactResult(int x, int y) {
  ShotResult result;
  result.event = ShotEvent::Impact;
  result.x = (int16_t)x;
  result.y = (int16_t)y;
  return result;
}

}  // namespace

void generateTerrain(uint32_t seed, int16_t terrain[kWorldWidth]) {
  uint32_t rng = seed ? seed : 1;
  const int min_y = kWorldHeight * 48 / 100;
  const int max_y = kWorldHeight * 78 / 100;
  int y = kWorldHeight * 62 / 100;
  int target = y;
  int remaining = 1;
  for (int x = 0; x < kWorldWidth; ++x) {
    if (--remaining <= 0) {
      remaining = 18 + (int)(nextRand(rng) % 20u);
      target = min_y + (int)(nextRand(rng) % (uint32_t)(max_y - min_y + 1));
    }
    if (y < target) ++y;
    else if (y > target) --y;

    // A small deterministic triangular ripple replaces the prior sinf call.
    const int phase = x % 56;
    const int triangle = phase <= 28 ? phase : 56 - phase;
    int world_y = y + (triangle - 14) / 5;
    if (world_y < min_y) world_y = min_y;
    if (world_y > max_y) world_y = max_y;
    terrain[x] = (int16_t)world_y;
  }
}

void flattenForTank(int16_t terrain[kWorldWidth], int x) {
  if (x < 0 || x >= kWorldWidth) return;
  const int y = terrain[x];
  for (int px = x - 8; px <= x + 8; ++px) {
    if (px >= 0 && px < kWorldWidth) terrain[px] = (int16_t)y;
  }
}

int tankY(const int16_t terrain[kWorldWidth], const Tank& tank) {
  if (tank.x < 0 || tank.x >= kWorldWidth) return kWorldHeight - 1;
  return terrain[tank.x];
}

void startShot(Shot& shot, const int16_t terrain[kWorldWidth],
               const Tank& tank, int shooter, int angle, int power) {
  const int sin_value = sinQ15(angle);
  const int cos_value = cosQ15(angle);
  const int y = tankY(terrain, tank);
  shot.x_q16 = (int32_t)tank.x * kQ16
      + divRound((int64_t)cos_value * 12 * kQ16, kTrigScale);
  shot.y_q16 = (int32_t)(y - 6) * kQ16
      - divRound((int64_t)sin_value * 12 * kQ16, kTrigScale);
  shot.vx_q16 = divRound((int64_t)cos_value * power * kVelocityScaleQ16,
                         kTrigScale);
  shot.vy_q16 = -divRound((int64_t)sin_value * power * kVelocityScaleQ16,
                          kTrigScale);
  shot.age = 0;
  shot.shooter = (int8_t)shooter;
}

ShotResult stepShot(Shot& shot, int wind,
                    const int16_t terrain[kWorldWidth],
                    const Tank* tanks, int tank_count) {
  for (int substep = 0; substep < kSubstepsPerTick; ++substep) {
    shot.vx_q16 += wind * kWindPerSubstepQ16;
    shot.vy_q16 += kGravityPerSubstepQ16;
    shot.x_q16 += shot.vx_q16 / kSubstepsPerTick;
    shot.y_q16 += shot.vy_q16 / kSubstepsPerTick;
    ++shot.age;

    if (shot.x_q16 < 0 || shot.x_q16 >= kWorldWidth * kQ16
        || shot.y_q16 > (kWorldHeight + 12) * kQ16) {
      return missResult();
    }

    const int x = shot.x_q16 / kQ16;
    const int y = shot.y_q16 / kQ16;
    if (shot.age > 7 && tanks) {
      for (int i = 0; i < tank_count; ++i) {
        if (tanks[i].health <= 0) continue;
        if (i == shot.shooter && shot.age <= 12) continue;
        const int dx = x - tanks[i].x;
        const int dy = y - (tankY(terrain, tanks[i]) - 5);
        if (dx * dx + dy * dy <= 49) return impactResult(x, y);
      }
    }
    if (y >= 0 && y >= terrain[x]) return impactResult(x, y);
  }
  return ShotResult{};
}

ShotResult simulateShot(Shot& shot, int wind,
                        const int16_t terrain[kWorldWidth],
                        const Tank* tanks, int tank_count) {
  for (int tick = 0; tick < kMaxSimulationTicks; ++tick) {
    const ShotResult result = stepShot(shot, wind, terrain, tanks, tank_count);
    if (result.event != ShotEvent::InFlight) return result;
  }
  return missResult();
}

void damageTank(Tank& tank, const int16_t terrain[kWorldWidth], int x, int y) {
  if (tank.health <= 0) return;
  const int dx = tank.x - x;
  const int dy = (tankY(terrain, tank) - 5) - y;
  const int radius = kBlastRadius + 7;
  if (dx * dx + dy * dy <= radius * radius) --tank.health;
}

void carveCrater(int16_t terrain[kWorldWidth], int x, int y) {
  const int radius_squared = kBlastRadius * kBlastRadius;
  for (int px = x - kBlastRadius; px <= x + kBlastRadius; ++px) {
    if (px < 0 || px >= kWorldWidth) continue;
    const int dx = px - x;
    const int inside = radius_squared - dx * dx;
    if (inside < 0) continue;
    int bottom = y + intSqrt(inside);
    if (bottom >= kWorldHeight) bottom = kWorldHeight - 1;
    if (bottom > terrain[px]) terrain[px] = (int16_t)bottom;
  }
}

void applyImpact(int16_t terrain[kWorldWidth], Tank* tanks, int tank_count,
                 int x, int y) {
  for (int i = 0; tanks && i < tank_count; ++i) damageTank(tanks[i], terrain, x, y);
  carveCrater(terrain, x, y);
}

int windForTurn(uint32_t seed, uint16_t turn) {
  uint32_t state = seed ^ (0x9E3779B9u * ((uint32_t)turn + 1u));
  state ^= 0xA341316Cu;
  const uint32_t value = nextRand(state);
  return (int)(value % 11u) - 5;
}

int shotX(const Shot& shot) { return shot.x_q16 / kQ16; }
int shotY(const Shot& shot) { return shot.y_q16 / kQ16; }

uint32_t hashByte(uint32_t hash, uint8_t value) {
  return (hash ^ value) * 16777619u;
}

uint32_t hashU16(uint32_t hash, uint16_t value) {
  hash = hashByte(hash, (uint8_t)(value & 0xFFu));
  return hashByte(hash, (uint8_t)(value >> 8));
}

uint32_t hashU32(uint32_t hash, uint32_t value) {
  hash = hashU16(hash, (uint16_t)(value & 0xFFFFu));
  return hashU16(hash, (uint16_t)(value >> 16));
}

}  // namespace artillery

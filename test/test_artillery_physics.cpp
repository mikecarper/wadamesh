#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "ArtilleryPhysics.h"

namespace {

struct World {
  int16_t terrain[artillery::kWorldWidth] = {};
  artillery::Tank tanks[artillery::kMaxPlayers] = {};
  int count = 0;
};

void configureWorld(World& world, uint32_t seed, int count) {
  world = World{};
  world.count = count;
  artillery::generateTerrain(seed, world.terrain);
  for (int i = 0; i < count; ++i) {
    artillery::Tank& tank = world.tanks[i];
    tank.x = (int16_t)((i + 1) * artillery::kWorldWidth / (count + 1));
    tank.health = 3;
    tank.angle = tank.x < artillery::kWorldWidth / 2 ? 45 : 135;
    tank.power = 65;
    artillery::flattenForTank(world.terrain, tank.x);
  }
}

bool sameShot(const artillery::Shot& left, const artillery::Shot& right) {
  return left.x_q16 == right.x_q16 && left.y_q16 == right.y_q16
      && left.vx_q16 == right.vx_q16 && left.vy_q16 == right.vy_q16
      && left.age == right.age && left.shooter == right.shooter;
}

bool sameWorld(const World& left, const World& right) {
  if (left.count != right.count
      || memcmp(left.terrain, right.terrain, sizeof(left.terrain)) != 0) return false;
  for (int i = 0; i < left.count; ++i) {
    const artillery::Tank& a = left.tanks[i];
    const artillery::Tank& b = right.tanks[i];
    if (a.x != b.x || a.health != b.health || a.angle != b.angle || a.power != b.power)
      return false;
  }
  return true;
}

uint32_t hashWorld(const World& world) {
  uint32_t hash = 2166136261u;
  hash = artillery::hashByte(hash, (uint8_t)world.count);
  for (int i = 0; i < world.count; ++i) {
    const artillery::Tank& tank = world.tanks[i];
    hash = artillery::hashU16(hash, (uint16_t)tank.x);
    hash = artillery::hashByte(hash, (uint8_t)tank.health);
    hash = artillery::hashByte(hash, tank.angle);
    hash = artillery::hashByte(hash, tank.power);
  }
  for (int x = 0; x < artillery::kWorldWidth; ++x)
    hash = artillery::hashU16(hash, (uint16_t)world.terrain[x]);
  return hash;
}

artillery::ShotResult fire(World& world, int seat, int angle, int power,
                           int wind, artillery::Shot& shot) {
  world.tanks[seat].angle = (uint8_t)angle;
  world.tanks[seat].power = (uint8_t)power;
  artillery::startShot(shot, world.terrain, world.tanks[seat], seat, angle, power);
  const artillery::ShotResult result = artillery::simulateShot(
      shot, wind, world.terrain, world.tanks, world.count);
  if (result.event == artillery::ShotEvent::Impact)
    artillery::applyImpact(world.terrain, world.tanks, world.count, result.x, result.y);
  return result;
}

}  // namespace

int main() {
  static_assert(artillery::kWorldWidth == 320, "canonical width changed");
  static_assert(artillery::kWorldHeight == 160, "canonical height changed");
  static_assert(artillery::kMaxPlayers == 10, "room player limit changed");

  int16_t terrain_a[artillery::kWorldWidth];
  int16_t terrain_b[artillery::kWorldWidth];
  int16_t terrain_c[artillery::kWorldWidth];
  artillery::generateTerrain(0x13579BDFu, terrain_a);
  artillery::generateTerrain(0x13579BDFu, terrain_b);
  artillery::generateTerrain(0x2468ACE0u, terrain_c);
  assert(memcmp(terrain_a, terrain_b, sizeof(terrain_a)) == 0);
  assert(memcmp(terrain_a, terrain_c, sizeof(terrain_a)) != 0);
  for (int x = 0; x < artillery::kWorldWidth; ++x) {
    assert(terrain_a[x] >= artillery::kWorldHeight * 48 / 100);
    assert(terrain_a[x] <= artillery::kWorldHeight * 78 / 100);
  }

  for (uint16_t turn = 0; turn < 512; ++turn) {
    const int wind = artillery::windForTurn(0x13579BDFu, turn);
    assert(wind >= -5 && wind <= 5);
    assert(wind == artillery::windForTurn(0x13579BDFu, turn));
  }

  int16_t flat[artillery::kWorldWidth];
  for (int x = 0; x < artillery::kWorldWidth; ++x) flat[x] = 100;
  artillery::Tank target;
  target.x = 50;
  target.health = 3;
  artillery::damageTank(target, flat, 50, 95);
  assert(target.health == 2);
  artillery::damageTank(target, flat, 73, 95);
  assert(target.health == 2);
  artillery::carveCrater(flat, 0, 100);
  assert(flat[0] == 115);
  assert(flat[14] == 105);
  assert(flat[15] == 100);
  assert(flat[16] == 100);

  World animated_world;
  configureWorld(animated_world, 0x10203040u, 4);
  artillery::Shot instant_shot;
  artillery::Shot animated_shot;
  artillery::startShot(instant_shot, animated_world.terrain,
                       animated_world.tanks[1], 1, 57, 82);
  animated_shot = instant_shot;
  const artillery::ShotResult instant_result = artillery::simulateShot(
      instant_shot, -3, animated_world.terrain,
      animated_world.tanks, animated_world.count);
  artillery::ShotResult animated_result;
  for (int tick = 0; tick < 720; ++tick) {
    animated_result = artillery::stepShot(
        animated_shot, -3, animated_world.terrain,
        animated_world.tanks, animated_world.count);
    if (animated_result.event != artillery::ShotEvent::InFlight) break;
  }
  assert(animated_result.event == instant_result.event);
  assert(animated_result.x == instant_result.x && animated_result.y == instant_result.y);
  assert(sameShot(animated_shot, instant_shot));

  World first;
  World second;
  configureWorld(first, 0x13579BDFu, 10);
  configureWorld(second, 0x13579BDFu, 10);
  assert(sameWorld(first, second));
  const uint32_t initial_hash = hashWorld(first);
  assert(initial_hash == 0xDBE7C673u);
  assert(artillery::windForTurn(0x13579BDFu, 0) == 3);
  assert(artillery::windForTurn(0x13579BDFu, 79) == -3);

  for (uint16_t turn = 0; turn < 80; ++turn) {
    const int seat = turn % first.count;
    const bool left_half = first.tanks[seat].x < artillery::kWorldWidth / 2;
    const int angle = left_half ? 32 + (turn * 7u) % 54u : 94 + (turn * 11u) % 54u;
    const int power = 24 + (turn * 13u) % 77u;
    const int wind = artillery::windForTurn(0x13579BDFu, turn);
    artillery::Shot shot_a;
    artillery::Shot shot_b;
    const artillery::ShotResult result_a = fire(first, seat, angle, power, wind, shot_a);
    const artillery::ShotResult result_b = fire(second, seat, angle, power, wind, shot_b);
    assert(result_a.event == result_b.event);
    assert(result_a.x == result_b.x && result_a.y == result_b.y);
    assert(sameShot(shot_a, shot_b));
    assert(sameWorld(first, second));
  }

  assert(hashWorld(first) == 0xB12FE9E0u);
  return 0;
}

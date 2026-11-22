// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui-touch/BikeRide.h"
#include "ui-touch/RideTag.h"
#include "ui-touch/LineGame.h"
#include <cassert>
#include <cstdio>
#include <initializer_list>

static void boundaries() {
  bike::Boundary b;
  b.points[0] = {0, 0};
  b.count = 1; b.radius = 100;
  assert(bike::valid(b) && bike::contains(b, {0, 899}) && !bike::contains(b, {0, 900}));
  b.points[0] = {0, 180000000}; b.points[1] = {1000000, 179000000}; b.count = 2;
  assert(bike::valid(b) && bike::contains(b, {100, -180000000}));
  b.points[0].lon = -179000000;
  assert(bike::contains(b, {100, 180000000}) && !bike::contains(b, {100, 0}));
  b.points[2] = {1000000, -179000000}; b.count = 3;
  assert(bike::valid(b) && bike::contains(b, {700000, -180000000}));
  b.points[0] = {0,0}; b.points[1] = {100,100}; b.points[2] = {100,0}; b.points[3] = {0,100}; b.count = 4;
  assert(!bike::valid(b));
  b.count = 1;
  bike::Fence f;
  assert(!f.observe(b, {0,0}, 1) && !f.observe(b, {0,0}, 2) && f.side == bike::Inside);
  assert(!f.observe(b, {0,2000}, 3) && !f.observe(b, {0,2000}, 3));
  assert(!f.observe(b, {0,2000}, 90) && f.observe(b, {0,2000}, 91));
  assert(!f.observe(b, {0,0}, 92) && f.observe(b, {0,0}, 93));
  bike::Absence a;
  uint32_t start = UINT32_MAX - 100;
  a.reset(start);
  assert(!a.update(start + 179999U, false, 180));
  assert(a.update(start + 180000U, false, 180) && a.lost);
  assert(!a.update(start + 181000U, false, 180));
  assert(a.update(start + 181001U, true, 180) && !a.lost);
}

static void tagChecks() {
  using namespace mesh::tracker;
  bike::RideTag t;
  uint8_t key[32]{}, data[28]{}, answer[16]{};
  key[31] = t.key[31] = 7;
  StatusRequest req{FreshGps,470000000,-1220000000,3900};
  const uint32_t now = 1800000000;
  assert(makeStatusRequest(req, data, sizeof(data)));
  assert(!t.accept(key, now, now, data, sizeof(data)));
  t.enabled = 1;
  key[31]++; assert(!t.accept(key, now, now, data, sizeof(data))); key[31]--;
  assert(!t.accept(key, now+301, now, data, sizeof(data)));
  data[27] = 1; assert(!t.accept(key, now, now, data, sizeof(data))); data[27] = 0;
  assert(t.accept(key, now, now, data, sizeof(data)));
  assert(t.latitude == req.latitude && t.longitude == req.longitude && t.battery_mv == 3900);
  assert(!t.accept(key, now, now, data, sizeof(data)));
  req = {GpsAttemptFailed,0,0,3800}; assert(makeStatusRequest(req, data, sizeof(data)));
  assert(t.accept(key, now+60, now+60, data, sizeof(data)));
  assert(t.heard == now+60 && t.position == now && t.battery_mv == 3800);
  uint8_t status; uint16_t lease;
  assert(makeStatusResponse(t.heard, Lost, 120, answer, sizeof(answer)));
  assert(parseStatusResponse(answer, sizeof(answer), t.heard, status, lease) && status == Lost && lease == 120);
}

static void boards() {
  linegame::Board b;
  for (int i : {0,1,2,4,3,5,7,6,8}) assert(b.play(i/3,i%3));
  assert(b.done() && !b.winner && !b.play(0,0));
  b = {}; assert(b.play(1,1) && !b.play(1,1));
  for (int i : {1,0,2,8}) assert(b.play(i/3,i%3));
  assert(b.winner == 1);
  b = {}; b.gravity = true;
  for (int c : {0,1,0,1,0,1,0}) assert(b.play(0,c));
  assert(b.winner == 1 && !b.play(0,2));
  b = {}; b.gravity = true;
  for (int c : {0,0,0,0,0,0}) assert(b.play(0,c));
  assert(!b.play(0,0) && b.moves == 6);
  b = {}; b.gravity = true;
  for (int c : {0,1,1,2,4,2,2,3,4,3,5,3,3}) assert(b.play(0,c));
  assert(b.winner == 1);
}
int main() { boundaries(); tagChecks(); boards(); puts("Bike, private tracker and board games passed"); }

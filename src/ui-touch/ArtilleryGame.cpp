// SPDX-License-Identifier: GPL-3.0-or-later
//
// Gameplay structure adapted from loxai/rotaryGames' MIT-licensed Scorched
// mini-game. The LVGL host, procedural graphics, multiplayer protocol,
// terrain representation, collision handling, wind, and CPU player are
// WadaMesh-native. See ArtilleryGame.LICENSE for the retained upstream notice.

#include "ArtilleryGame.h"

#include "../MyMesh.h"
#include "AppPage.h"
#include "i18n.h"

#include <Arduino.h>
#include <LvglPsramAlloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" void touchArtilleryTurnAlert();

namespace {

constexpr uint32_t kTickMs = 33;
constexpr int kControlH = 32;
constexpr float kGravity = 0.12f;
constexpr float kWindAccel = 0.003f;
constexpr float kShotScale = 0.085f;
constexpr float kPi = 3.14159265358979323846f;
constexpr char kProtocolPrefix[] = "~SG1|";

constexpr uint32_t kSkyTop = 0x081A2B;
constexpr uint32_t kSkyBottom = 0x24506A;
constexpr uint32_t kSoil = 0x5A3824;
constexpr uint32_t kSoilDeep = 0x39251C;
constexpr uint32_t kGrass = 0x6E9E45;
constexpr uint32_t kPlayer = 0x29C7B8;
constexpr uint32_t kCpu = 0xE45C4F;
constexpr uint32_t kShell = 0xFFF4C2;
constexpr uint32_t kBlast = 0xFF9F1C;
constexpr uint32_t kText = 0xE8EEF2;

struct PendingInvite {
  bool valid = false;
  bool room = false;
  bool teams = false;
  uint32_t match = 0;
  uint32_t seed = 1;
  uint8_t host_id[4] = {};
  uint8_t transport_pub[32] = {};
  char transport_name[16] = {};
  char host_name[16] = {};
};

PendingInvite s_pending_invite;

struct ProtocolFields {
  char storage[MAX_TEXT_LEN + 1] = {};
  char* field[16] = {};
  int count = 0;
};

bool splitProtocol(const char* text, ProtocolFields& out) {
  if (!text) return false;
  const size_t len = strlen(text);
  if (len < sizeof(kProtocolPrefix) || len > MAX_TEXT_LEN) return false;
  if (strncmp(text, kProtocolPrefix, sizeof(kProtocolPrefix) - 1) != 0) return false;
  memcpy(out.storage, text, len + 1);
  char* p = out.storage;
  while (p && out.count < (int)(sizeof(out.field) / sizeof(out.field[0]))) {
    out.field[out.count++] = p;
    char* bar = strchr(p, '|');
    if (!bar) break;
    *bar = '\0';
    p = bar + 1;
  }
  return out.count >= 2 && strcmp(out.field[0], "~SG1") == 0;
}

bool knownCommand(const char* command) {
  return command && command[0] && command[1] == '\0'
      && strchr("IJPCBVFSXTRQ", command[0]) != nullptr;
}

bool parseUnsigned(const char* s, uint32_t& value, unsigned base = 10) {
  if (!s || !*s || (base != 10 && base != 16)) return false;
  uint32_t result = 0;
  for (const char* p = s; *p; ++p) {
    unsigned digit;
    if (*p >= '0' && *p <= '9') digit = (unsigned)(*p - '0');
    else if (base == 16 && *p >= 'a' && *p <= 'f') digit = (unsigned)(*p - 'a' + 10);
    else if (base == 16 && *p >= 'A' && *p <= 'F') digit = (unsigned)(*p - 'A' + 10);
    else return false;
    if (digit >= base || result > (UINT32_MAX - digit) / base) return false;
    result = result * base + digit;
  }
  value = result;
  return true;
}

bool parseSigned(const char* s, int& value) {
  if (!s || !*s) return false;
  bool neg = *s == '-';
  if (neg) ++s;
  uint32_t magnitude = 0;
  if (!parseUnsigned(s, magnitude) || magnitude > 32767u) return false;
  value = neg ? -(int)magnitude : (int)magnitude;
  return true;
}

void idToHex(const uint8_t id[4], char out[9]) {
  snprintf(out, 9, "%02X%02X%02X%02X", id[0], id[1], id[2], id[3]);
}

bool hexToId(const char* text, uint8_t out[4]) {
  if (!text || strlen(text) != 8) return false;
  for (int i = 0; i < 4; ++i) {
    char pair[3] = {text[i * 2], text[i * 2 + 1], '\0'};
    uint32_t v = 0;
    if (!parseUnsigned(pair, v, 16) || v > 255) return false;
    out[i] = (uint8_t)v;
  }
  return true;
}

void safeName(char* dest, size_t cap, const char* source) {
  if (!dest || cap == 0) return;
  size_t n = 0;
  if (source) {
    for (; *source && n + 1 < cap; ++source) {
      const unsigned char c = (unsigned char)*source;
      if (c == '|' || c == ',' || c == ';' || c < 0x20 || c > 0x7E) dest[n++] = '_';
      else dest[n++] = (char)c;
    }
  }
  dest[n] = '\0';
  if (n == 0) snprintf(dest, cap, "player");
}

uint32_t nextTerrainRand(uint32_t& state) {
  if (!state) state = 0x6D2B79F5u;
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return state;
}

bool timeReached(uint32_t now, uint32_t deadline) {
  return deadline != 0 && (int32_t)(now - deadline) >= 0;
}

}  // namespace

ArtilleryGame* ArtilleryGame::s_active = nullptr;

bool ArtilleryGame::isOpen() { return s_active && s_active->root_; }
bool ArtilleryGame::hasPendingInvite() { return s_pending_invite.valid; }

void ArtilleryGame::launch() {
  if (s_active) return;
  ArtilleryGame* game = new ArtilleryGame();
  if (!game) return;
  s_active = game;
  if (!game->open()) {
    s_active = nullptr;
    delete game;
  }
}

void ArtilleryGame::dismiss() {
  if (!s_active) return;
  ArtilleryGame* game = s_active;
  if (game->is_host_ && (game->mode_ == MODE_DM || game->mode_ == MODE_ROOM)
      && game->match_id_ != 0) {
    char frame[40];
    snprintf(frame, sizeof frame, "~SG1|Q|%08lX", (unsigned long)game->match_id_);
    game->broadcast(frame);
  } else if (!game->is_host_ && game->state_ != GAME_OVER
             && (game->mode_ == MODE_DM || game->mode_ == MODE_ROOM)
             && game->match_id_ != 0) {
    char frame[40];
    snprintf(frame, sizeof frame, "~SG1|Q|%08lX", (unsigned long)game->match_id_);
    game->sendTransport(frame);
  }
  s_active = nullptr;
  game->close();
  lv_async_call(destroyAsync, game);
}

void ArtilleryGame::destroyAsync(void* p) { delete static_cast<ArtilleryGame*>(p); }

ArtilleryGame::~ArtilleryGame() {
  if (buf_) {
    lvglPsramFree(buf_);
    buf_ = nullptr;
  }
}

bool ArtilleryGame::handleMeshMessage(const ContactInfo& transport,
                                      const uint8_t* author_id, size_t author_id_len,
                                      bool via_room, const char* text) {
  ProtocolFields frame;
  if (!splitProtocol(text, frame) || !knownCommand(frame.field[1])) return false;

  // Invitations must be retained even while the game is closed. They contain
  // no gameplay state beyond the lobby identity/seed, so one small static slot
  // is enough and a newer invitation intentionally replaces an older one.
  if (frame.field[1][0] == 'I' && frame.count == 7 && author_id && author_id_len >= 4) {
    uint32_t match = 0, seed = 0, teams = 0;
    const bool room_frame = strcmp(frame.field[4], "R") == 0;
    const bool dm_frame = strcmp(frame.field[4], "D") == 0;
    if ((!room_frame && !dm_frame) || room_frame != via_room
        || !parseUnsigned(frame.field[2], match, 16) || match == 0
        || !parseUnsigned(frame.field[3], seed, 16) || seed == 0
        || !parseUnsigned(frame.field[5], teams) || teams > 1) {
      return true;  // recognized control traffic, but malformed
    }
    const uint8_t* self = the_mesh.getSelfPubKey();
    // Room servers sign and push our own post back to us. Do not turn the
    // host's echoed invitation into a prompt on top of its lobby.
    if (self && memcmp(author_id, self, 4) == 0) return true;
    if (s_active && s_active->match_started_) return true;

    s_pending_invite = PendingInvite{};
    s_pending_invite.valid = true;
    s_pending_invite.room = via_room;
    s_pending_invite.teams = teams != 0;
    s_pending_invite.match = match;
    s_pending_invite.seed = seed;
    memcpy(s_pending_invite.host_id, author_id, 4);
    memcpy(s_pending_invite.transport_pub, transport.id.pub_key, 32);
    safeName(s_pending_invite.transport_name, sizeof(s_pending_invite.transport_name), transport.name);
    safeName(s_pending_invite.host_name, sizeof(s_pending_invite.host_name), frame.field[6]);
    if (s_active && !s_active->match_started_
        && (s_active->view_ == VIEW_MODE || s_active->view_ == VIEW_PICKER
            || s_active->view_ == VIEW_INVITE)) {
      s_active->showInvitePrompt();
    }
    return true;
  }

  // Room history can replay an invitation after its host already started or
  // cancelled the match. A matching BEGIN/CLOSE from that same host retires
  // the pending card even when Artillery is not currently open.
  if ((frame.field[1][0] == 'B' || frame.field[1][0] == 'Q')
      && frame.count >= 3 && s_pending_invite.valid && author_id_len >= 4) {
    uint32_t match = 0;
    if (parseUnsigned(frame.field[2], match, 16)
        && match == s_pending_invite.match
        && memcmp(author_id, s_pending_invite.host_id, 4) == 0
        && memcmp(transport.id.pub_key, s_pending_invite.transport_pub, 32) == 0) {
      s_pending_invite.valid = false;
    }
  }

  if (s_active) {
    s_active->receiveProtocol(transport, author_id, author_id_len, via_room, text);
  }
  return true;
}

void ArtilleryGame::steer(int dx, int dy) {
  if (!s_active || (dx == 0 && dy == 0)) return;
  ArtilleryGame* game = s_active;
  if (game->view_ == VIEW_MODE) {
    int delta = (dx > 0 || dy > 0) ? 1 : -1;
    game->picker_cursor_ = (game->picker_cursor_ + delta + 3) % 3;
    for (int i = 0; i < 3; ++i) {
      if (!game->mode_buttons_[i]) continue;
      lv_obj_set_style_bg_color(game->mode_buttons_[i],
          lv_color_hex(i == game->picker_cursor_ ? 0x287D83 : 0x233746), LV_PART_MAIN);
    }
    return;
  }
  if (game->view_ == VIEW_PICKER) {
    game->movePicker((dx > 0 || dy > 0) ? 1 : -1);
    return;
  }
  if (game->view_ != VIEW_GAME || game->state_ != PLAYER_AIM) return;
  const int ax = dx < 0 ? -dx : dx;
  const int ay = dy < 0 ? -dy : dy;
  if (ax >= ay) game->adjustAngle(dx > 0 ? 1 : -1);
  else          game->adjustPower(dy < 0 ? 2 : -2);
}

void ArtilleryGame::setFire(bool down) {
  if (!s_active) return;
  if (down && !s_active->fire_down_) s_active->action();
  s_active->fire_down_ = down;
}

bool ArtilleryGame::keyChar(char c) {
  if (!s_active) return false;
  ArtilleryGame* game = s_active;
  if (c == '\b' || c == 0x1B || c == 'q' || c == 'Q') {
    dismiss();
    return true;
  }

  if (game->view_ == VIEW_MODE) {
    if (c == '1' || c == 'l' || c == 'L') game->startLocalGame();
    else if (c == '2' || c == 'd' || c == 'D') game->showContactPicker(false);
    else if (c == '3' || c == 'm' || c == 'M') game->showContactPicker(true);
    else if (c == '\r' || c == '\n' || c == ' ') game->action();
    return true;
  }
  if (game->view_ == VIEW_PICKER) {
    if (c == 'w' || c == 'W' || c == 'k' || c == 'K') game->movePicker(-1);
    else if (c == 'x' || c == 'X' || c == 'j' || c == 'J') game->movePicker(1);
    else if (c == 'n' || c == 'N') { ++game->picker_page_; game->refreshPicker(); }
    else if (c == 'p' || c == 'P') { if (game->picker_page_ > 0) --game->picker_page_; game->refreshPicker(); }
    else if (c == 's' || c == 'S' || c == 'f' || c == 'F') game->beginHostLobby();
    else if (c == '\r' || c == '\n' || c == ' ') game->action();
    else if (c == 'b' || c == 'B') game->showModeMenu();
    return true;
  }
  if (game->view_ == VIEW_INVITE) {
    if (c == 'y' || c == 'Y' || c == '\r' || c == '\n' || c == ' ') game->acceptInvite();
    else if (c == 'n' || c == 'N') game->declineInvite();
    return true;
  }
  if (game->view_ == VIEW_LOBBY) {
    if (c == 't' || c == 'T') game->toggleLocalTeam();
    else if ((c == 'g' || c == 'G') && game->is_host_) game->toggleTeams();
    else if ((c == 's' || c == 'S' || c == 'f' || c == 'F'
              || c == '\r' || c == '\n') && game->is_host_) game->startHostedMatch();
    return true;
  }

  switch (c) {
    case 'a': case 'A': case 'h': case 'H': game->adjustAngle(-1); break;
    case 'd': case 'D': case 'l': case 'L': game->adjustAngle(+1); break;
    case 'w': case 'W': case 'i': case 'I': game->adjustPower(+2); break;
    case 's': case 'S': case 'k': case 'K': game->adjustPower(-2); break;
    case 'f': case 'F': case ' ': case '\r': case '\n': game->action(); break;
    case 'r': case 'R': game->replayGame(); break;
    default: break;
  }
  return true;
}

lv_obj_t* ArtilleryGame::makeButton(int x, int y, int w, const char* text, lv_event_cb_t cb) {
  lv_obj_t* button = lv_btn_create(root_);
  lv_obj_set_size(button, w, kControlH - 4);
  lv_obj_set_pos(button, x, y + 2);
  lv_obj_set_style_radius(button, 5, LV_PART_MAIN);
  lv_obj_set_style_pad_all(button, 0, LV_PART_MAIN);
  lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, this);
  lv_obj_t* label = lv_label_create(button);
  lv_label_set_text(label, text);
  lv_obj_center(label);
  return button;
}

void ArtilleryGame::buildControls(int local_y) {
  const int total = 284;
  int x = (width_ - total) / 2;
  angle_minus_ = makeButton(x, local_y, 28, "-", angleMinusCb); x += 28;

  angle_label_ = lv_label_create(root_);
  lv_obj_set_width(angle_label_, 52);
  lv_obj_set_pos(angle_label_, x, local_y + 8);
  lv_obj_set_style_text_align(angle_label_, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_style_text_color(angle_label_, lv_color_hex(kText), LV_PART_MAIN);
  x += 52;

  angle_plus_ = makeButton(x, local_y, 28, "+", anglePlusCb); x += 32;
  power_minus_ = makeButton(x, local_y, 28, "-", powerMinusCb); x += 28;

  power_label_ = lv_label_create(root_);
  lv_obj_set_width(power_label_, 52);
  lv_obj_set_pos(power_label_, x, local_y + 8);
  lv_obj_set_style_text_align(power_label_, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_style_text_color(power_label_, lv_color_hex(kText), LV_PART_MAIN);
  x += 52;

  power_plus_ = makeButton(x, local_y, 28, "+", powerPlusCb); x += 32;
  fire_btn_ = makeButton(x, local_y, 64, "FIRE", fireCb);
  lv_obj_set_style_bg_color(fire_btn_, lv_color_hex(0xC74D42), LV_PART_MAIN);
}

bool ArtilleryGame::open() {
  width_ = (int)lv_disp_get_hor_res(nullptr);
  const int root_h = (int)appPageContentH();
  if (width_ <= 0 || width_ > kMaxWidth || root_h <= 96) return false;

  const int visible_top = (int)appPageContentTop();
  canvas_y_ = visible_top + kControlH;
  height_ = root_h - canvas_y_;
  if (height_ < 90) return false;

  buf_ = static_cast<lv_color_t*>(lvglPsramAlloc((size_t)width_ * height_ * sizeof(lv_color_t)));
  if (!buf_) return false;

  root_ = appPageCreateRoot(0x07131E);
  if (!root_) return false;
  appPageBegin(TR("Artillery"), &ArtilleryGame::dismiss);

  canvas_ = lv_canvas_create(root_);
  lv_canvas_set_buffer(canvas_, buf_, width_, height_, LV_IMG_CF_TRUE_COLOR);
  lv_obj_set_pos(canvas_, 0, canvas_y_);
  lv_obj_clear_flag(canvas_, LV_OBJ_FLAG_SCROLLABLE);
  buildControls(visible_top);

  info_label_ = lv_label_create(root_);
  lv_obj_set_pos(info_label_, 5, canvas_y_ + 3);
  lv_obj_set_style_text_color(info_label_, lv_color_hex(kText), LV_PART_MAIN);
  lv_obj_set_style_bg_color(info_label_, lv_color_hex(0x07131E), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(info_label_, LV_OPA_50, LV_PART_MAIN);
  lv_obj_set_style_pad_hor(info_label_, 3, LV_PART_MAIN);

  turn_label_ = lv_label_create(root_);
  lv_obj_align(turn_label_, LV_ALIGN_TOP_RIGHT, -5, canvas_y_ + 3);
  lv_obj_set_style_text_color(turn_label_, lv_color_hex(kText), LV_PART_MAIN);
  lv_obj_set_style_bg_color(turn_label_, lv_color_hex(0x07131E), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(turn_label_, LV_OPA_50, LV_PART_MAIN);
  lv_obj_set_style_pad_hor(turn_label_, 3, LV_PART_MAIN);

  timer_ = lv_timer_create(timerCb, kTickMs, this);
  terrain_seed_ = (uint32_t)random(1, 0x7FFFFFFF);
  generateTerrain();
  render();
  if (s_pending_invite.valid) showInvitePrompt();
  else showModeMenu();
  updateHud();
  return true;
}

void ArtilleryGame::close() {
  if (timer_) {
    lv_timer_del(timer_);
    timer_ = nullptr;
  }
  appPageEnd(&ArtilleryGame::dismiss);
  appPageDeleteRootAsync(root_);
  root_ = nullptr;
  canvas_ = nullptr;
  overlay_ = nullptr;
  angle_label_ = power_label_ = info_label_ = turn_label_ = nullptr;
  angle_minus_ = angle_plus_ = power_minus_ = power_plus_ = fire_btn_ = nullptr;
  replay_btn_ = replay_label_ = nullptr;
}

void ArtilleryGame::startLocalGame() {
  if (!root_) return;
  clearOverlay();
  mode_ = MODE_LOCAL;
  view_ = VIEW_GAME;
  is_host_ = false;
  match_started_ = false;
  pending_begin_ = false;
  player_ = Tank{};
  cpu_ = Tank{};
  player_.x = width_ * 14 / 100;
  cpu_.x = width_ * 86 / 100;
  player_.angle = 45;
  cpu_.angle = 135;
  wind_ = (int)random(-5, 6);
  terrain_seed_ = (uint32_t)random(1, 0x7FFFFFFF);
  shot_by_cpu_ = false;
  fire_down_ = false;
  explosion_x_ = explosion_y_ = -1;
  generateTerrain();
  flattenForTank(player_.x);
  flattenForTank(cpu_.x);
  state_ = PLAYER_AIM;
  state_since_ms_ = millis();
  if (timer_) lv_timer_pause(timer_);
  hideReplay();
  updateHud();
  render();
}

void ArtilleryGame::replayGame() {
  if (mode_ == MODE_LOCAL) startLocalGame();
  else showModeMenu();
}

void ArtilleryGame::generateTerrain() {
  int16_t world[kWorldWidth];
  uint32_t rng = terrain_seed_ ? terrain_seed_ : 1;
  const int min_y = kWorldHeight * 48 / 100;
  const int max_y = kWorldHeight * 78 / 100;
  int y = kWorldHeight * 62 / 100;
  int target = y;
  int remaining = 1;
  for (int x = 0; x < kWorldWidth; ++x) {
    if (--remaining <= 0) {
      remaining = 18 + (int)(nextTerrainRand(rng) % 20u);
      target = min_y + (int)(nextTerrainRand(rng) % (uint32_t)(max_y - min_y + 1));
    }
    if (y < target) ++y;
    else if (y > target) --y;
    int gy = y + (int)(3.0f * sinf((float)x * 0.055f));
    if (gy < min_y) gy = min_y;
    if (gy > max_y) gy = max_y;
    world[x] = (int16_t)gy;
  }
  const int xden = width_ > 1 ? width_ - 1 : 1;
  for (int x = 0; x < width_; ++x) {
    const int wx = x * (kWorldWidth - 1) / xden;
    terrain_[x] = (int16_t)(world[wx] * (height_ - 1) / (kWorldHeight - 1));
  }
}

void ArtilleryGame::flattenForTank(int x) {
  if (x < 0 || x >= width_) return;
  const int y = terrain_[x];
  for (int px = x - 8; px <= x + 8; ++px)
    if (px >= 0 && px < width_) terrain_[px] = (int16_t)y;
}

int ArtilleryGame::tankY(const Tank& tank) const {
  if (tank.x < 0 || tank.x >= width_) return height_ - 1;
  return terrain_[tank.x];
}

bool ArtilleryGame::isLocalTurn() const {
  if (mode_ == MODE_LOCAL) return state_ == PLAYER_AIM;
  return match_started_ && local_seat_ >= 0 && local_seat_ == turn_seat_
      && mesh_players_[local_seat_].used && mesh_players_[local_seat_].tank.health > 0
      && state_ == PLAYER_AIM;
}

void ArtilleryGame::adjustAngle(int delta) {
  if (!isLocalTurn()) return;
  Tank* tank = mode_ == MODE_LOCAL ? &player_ : &mesh_players_[local_seat_].tank;
  tank->angle += delta;
  if (tank->angle < 5) tank->angle = 5;
  if (tank->angle > 175) tank->angle = 175;
  updateHud();
  render();
}

void ArtilleryGame::adjustPower(int delta) {
  if (!isLocalTurn()) return;
  Tank* tank = mode_ == MODE_LOCAL ? &player_ : &mesh_players_[local_seat_].tank;
  tank->power += delta;
  if (tank->power < 20) tank->power = 20;
  if (tank->power > 100) tank->power = 100;
  updateHud();
  render();
}

void ArtilleryGame::action() {
  if (view_ == VIEW_MODE) {
    if (picker_cursor_ == 0) startLocalGame();
    else showContactPicker(picker_cursor_ == 2);
    return;
  }
  if (view_ == VIEW_PICKER) {
    PickerRow& row = picker_rows_[picker_cursor_ % kPickerRows];
    if (row.mesh_index >= 0) togglePickerRow(row.mesh_index);
    return;
  }
  if (view_ == VIEW_INVITE) { acceptInvite(); return; }
  if (view_ == VIEW_LOBBY) {
    if (is_host_) startHostedMatch();
    return;
  }
  if (state_ == GAME_OVER) { replayGame(); return; }
  if (!isLocalTurn()) return;
  if (mode_ == MODE_LOCAL) {
    launchShot(false);
  } else if (is_host_) {
    Tank& tank = mesh_players_[local_seat_].tank;
    broadcastShot(local_seat_, tank.angle, tank.power);
  } else {
    sendShotRequest();
    state_ = SHOT_PENDING;
    state_since_ms_ = millis();
    mesh_retry_at_ = millis() + 8000;
    if (timer_) { lv_timer_set_period(timer_, 250); lv_timer_resume(timer_); }
    updateHud();
  }
}

void ArtilleryGame::launchShot(bool cpu_shot) {
  Tank& tank = cpu_shot ? cpu_ : player_;
  const float angle = (float)tank.angle * kPi / 180.0f;
  const float display_scale = sqrtf((float)width_ / 320.0f);
  const float speed = (float)tank.power * kShotScale * display_scale;
  const int ty = tankY(tank);

  shell_x_ = (float)tank.x + cosf(angle) * 12.0f;
  shell_y_ = (float)ty - 6.0f - sinf(angle) * 12.0f;
  shell_vx_ = cosf(angle) * speed;
  shell_vy_ = -sinf(angle) * speed;
  shell_age_ = 0;
  shot_by_cpu_ = cpu_shot;
  state_ = cpu_shot ? CPU_FLIGHT : PLAYER_FLIGHT;
  state_since_ms_ = millis();
  if (timer_) { lv_timer_set_period(timer_, kTickMs); lv_timer_resume(timer_); }
  updateHud();
}

void ArtilleryGame::launchMeshShot(int seat, int angle_deg, int power, int shot_wind) {
  if (seat < 0 || seat >= mesh_player_count_ || !mesh_players_[seat].used) return;
  Tank& tank = mesh_players_[seat].tank;
  tank.angle = angle_deg;
  tank.power = power;
  wind_ = shot_wind;
  const float angle = (float)angle_deg * kPi / 180.0f;
  const float display_scale = sqrtf((float)width_ / 320.0f);
  const float speed = (float)power * kShotScale * display_scale;
  const int ty = tankY(tank);
  shell_x_ = (float)tank.x + cosf(angle) * 12.0f;
  shell_y_ = (float)ty - 6.0f - sinf(angle) * 12.0f;
  shell_vx_ = cosf(angle) * speed;
  shell_vy_ = -sinf(angle) * speed;
  shell_age_ = 0;
  shot_seat_ = seat;
  mesh_result_received_ = false;
  explosion_x_ = explosion_y_ = -1;
  state_ = PLAYER_FLIGHT;
  state_since_ms_ = millis();
  mesh_retry_at_ = millis() + 8000;
  if (timer_) { lv_timer_set_period(timer_, kTickMs); lv_timer_resume(timer_); }
  updateHud();
}

int ArtilleryGame::simulateCpuImpact(int angle_deg, int power) const {
  const float angle = (float)angle_deg * kPi / 180.0f;
  const float scale = sqrtf((float)width_ / 320.0f);
  const float speed = (float)power * kShotScale * scale;
  float x = (float)cpu_.x + cosf(angle) * 12.0f;
  float y = (float)tankY(cpu_) - 6.0f - sinf(angle) * 12.0f;
  float vx = cosf(angle) * speed;
  float vy = -sinf(angle) * speed;
  for (int step = 0; step < 360; ++step) {
    vx += (float)wind_ * kWindAccel;
    vy += kGravity;
    x += vx;
    y += vy;
    if (x < 0) return 0;
    if (x >= width_) return width_ - 1;
    const int ix = (int)x;
    if (y >= 0 && y >= terrain_[ix]) return ix;
    if (y > height_ + 16) return ix;
  }
  return (int)x;
}

void ArtilleryGame::chooseCpuShot() {
  int best_angle = 135;
  int best_power = 65;
  int best_error = width_ * 2;
  for (int angle = 95; angle <= 170; angle += 2) {
    for (int power = 28; power <= 100; power += 2) {
      const int impact = simulateCpuImpact(angle, power);
      const int error = abs(impact - player_.x);
      if (error < best_error) {
        best_error = error;
        best_angle = angle;
        best_power = power;
      }
    }
  }
  best_angle += (int)random(-2, 3);
  best_power += (int)random(-3, 4);
  if (best_angle < 5) best_angle = 5;
  if (best_angle > 175) best_angle = 175;
  if (best_power < 20) best_power = 20;
  if (best_power > 100) best_power = 100;
  cpu_.angle = best_angle;
  cpu_.power = best_power;
}

void ArtilleryGame::tick() {
  if (!root_) return;
  const uint32_t now = millis();
  bool redraw = false;
  switch (state_) {
    case PLAYER_FLIGHT:
    case CPU_FLIGHT:
      advanceShell();
      redraw = true;
      break;
    case CPU_THINK:
      if ((uint32_t)(now - state_since_ms_) >= 650) {
        chooseCpuShot();
        launchShot(true);
        redraw = true;
      }
      break;
    case SHOT_PENDING:
      if (timeReached(now, mesh_retry_at_)) {
        sendShotRequest();
        mesh_retry_at_ = now + 8000;
      }
      break;
    case EXPLODING:
      if (mode_ != MODE_LOCAL && !is_host_ && !mesh_result_received_) {
        if (timeReached(now, mesh_retry_at_)) {
          char request[48];
          snprintf(request, sizeof request, "~SG1|R|%08lX|%u",
                   (unsigned long)match_id_, (unsigned)mesh_turn_);
          sendTransport(request);
          mesh_retry_at_ = now + 8000;
        }
        redraw = (uint32_t)(now - state_since_ms_) < 600;
        if (!redraw && timer_) lv_timer_set_period(timer_, 250);
      } else {
        if ((uint32_t)(now - state_since_ms_) >= 520) finishExplosion();
        redraw = true;
      }
      break;
    default:
      break;
  }

  if (view_ == VIEW_LOBBY && !is_host_ && local_seat_ < 0
      && timeReached(now, mesh_retry_at_)) {
    sendJoin();
    mesh_retry_at_ = now + 10000;
  }
  if (pending_begin_ && !is_host_ && timeReached(now, mesh_retry_at_)) {
    char request[48];
    snprintf(request, sizeof request, "~SG1|R|%08lX|L", (unsigned long)match_id_);
    sendTransport(request);
    mesh_retry_at_ = now + 8000;
  }
  if (redraw) render();
}

void ArtilleryGame::advanceShell() {
  for (int sub = 0; sub < 3; ++sub) {
    shell_vx_ += ((float)wind_ * kWindAccel) / 3.0f;
    shell_vy_ += kGravity / 3.0f;
    shell_x_ += shell_vx_ / 3.0f;
    shell_y_ += shell_vy_ / 3.0f;
    ++shell_age_;

    if (shell_x_ < 0 || shell_x_ >= width_ || shell_y_ > height_ + 12) {
      finishMiss();
      return;
    }
    const int ix = (int)shell_x_;
    const int iy = (int)shell_y_;

    if (shell_age_ > 7) {
      if (mode_ == MODE_LOCAL) {
        const int py = tankY(player_) - 5;
        const int cy = tankY(cpu_) - 5;
        const int pdx = ix - player_.x, pdy = iy - py;
        const int cdx = ix - cpu_.x, cdy = iy - cy;
        if (pdx * pdx + pdy * pdy <= 49 || cdx * cdx + cdy * cdy <= 49) {
          beginExplosion(ix, iy);
          return;
        }
      } else {
        for (int i = 0; i < mesh_player_count_; ++i) {
          if (!mesh_players_[i].used || mesh_players_[i].tank.health <= 0) continue;
          if (i == shot_seat_ && shell_age_ <= 12) continue;
          const Tank& tank = mesh_players_[i].tank;
          const int dx = ix - tank.x;
          const int dy = iy - (tankY(tank) - 5);
          if (dx * dx + dy * dy <= 49) {
            beginExplosion(ix, iy);
            return;
          }
        }
      }
    }
    if (iy >= 0 && iy >= terrain_[ix]) {
      beginExplosion(ix, iy);
      return;
    }
  }
}

void ArtilleryGame::beginExplosion(int x, int y) {
  if (x < 0) x = 0;
  if (x >= width_) x = width_ - 1;
  if (y < 0) y = 0;
  if (y >= height_) y = height_ - 1;
  explosion_x_ = x;
  explosion_y_ = y;
  state_ = EXPLODING;
  state_since_ms_ = millis();
  if (timer_) { lv_timer_set_period(timer_, kTickMs); lv_timer_resume(timer_); }
  if (mode_ == MODE_LOCAL) {
    damageTank(player_, x, y);
    damageTank(cpu_, x, y);
    carveCrater(x, y);
  } else if (is_host_) {
    resolveMeshShot(x, y, false);
  } else {
    mesh_result_received_ = false;
    mesh_retry_at_ = millis() + 8000;
  }
  updateHud();
}

void ArtilleryGame::finishMiss() {
  explosion_x_ = explosion_y_ = -1;
  state_ = EXPLODING;
  state_since_ms_ = millis() - 260;
  if (timer_) { lv_timer_set_period(timer_, kTickMs); lv_timer_resume(timer_); }
  if (mode_ != MODE_LOCAL) {
    if (is_host_) resolveMeshShot(-1, -1, true);
    else {
      mesh_result_received_ = false;
      mesh_retry_at_ = millis() + 8000;
    }
  }
  updateHud();
}

void ArtilleryGame::damageTank(Tank& tank, int x, int y) {
  if (tank.health <= 0) return;
  const int dx = tank.x - x;
  const int dy = (tankY(tank) - 5) - y;
  const int radius = kBlastRadius + 7;
  if (dx * dx + dy * dy <= radius * radius) --tank.health;
}

void ArtilleryGame::carveCrater(int x, int y) {
  const int r2 = kBlastRadius * kBlastRadius;
  for (int px = x - kBlastRadius; px <= x + kBlastRadius; ++px) {
    if (px < 0 || px >= width_) continue;
    const int dx = px - x;
    const int inside = r2 - dx * dx;
    if (inside < 0) continue;
    int bottom = y + (int)sqrtf((float)inside);
    if (bottom >= height_) bottom = height_ - 1;
    if (bottom > terrain_[px]) terrain_[px] = (int16_t)bottom;
  }
}

int ArtilleryGame::aliveTeamMask() const {
  int mask = 0;
  for (int i = 0; i < mesh_player_count_; ++i) {
    if (!mesh_players_[i].used || mesh_players_[i].tank.health <= 0) continue;
    const int bit = teams_enabled_ ? (mesh_players_[i].team & 1) : i;
    mask |= 1 << bit;
  }
  return mask;
}

int ArtilleryGame::nextAliveSeat(int after) const {
  if (mesh_player_count_ <= 0) return -1;
  for (int step = 1; step <= mesh_player_count_; ++step) {
    const int seat = (after + step) % mesh_player_count_;
    if (mesh_players_[seat].used && mesh_players_[seat].tank.health > 0) return seat;
  }
  return -1;
}

bool ArtilleryGame::showMeshWinnerIfDone() {
  const int alive = aliveTeamMask();
  if (alive != 0 && (alive & (alive - 1)) != 0) return false;
  state_ = GAME_OVER;
  char result[48];
  if (alive == 0) {
    snprintf(result, sizeof result, "DRAW\nNEW GAME");
  } else if (teams_enabled_) {
    snprintf(result, sizeof result, "TEAM %c WINS\nNEW GAME", (alive & 1) ? 'A' : 'B');
  } else {
    int winner = 0;
    while (winner < mesh_player_count_ && !(alive & (1 << winner))) ++winner;
    snprintf(result, sizeof result, "%.15s WINS\nNEW GAME",
             winner < mesh_player_count_ ? mesh_players_[winner].name : "PLAYER");
  }
  showReplay(result);
  if (timer_) lv_timer_pause(timer_);
  updateHud();
  render();
  return true;
}

void ArtilleryGame::finishExplosion() {
  explosion_x_ = explosion_y_ = -1;
  if (mode_ == MODE_LOCAL) {
    if (player_.health <= 0 || cpu_.health <= 0) {
      state_ = GAME_OVER;
      showReplay(player_.health > 0 ? "YOU WIN!\nPLAY AGAIN" : "CPU WINS\nTRY AGAIN");
    } else if (shot_by_cpu_) {
      wind_ = (int)random(-5, 6);
      state_ = PLAYER_AIM;
    } else {
      state_ = CPU_THINK;
    }
  } else {
    if (!showMeshWinnerIfDone()) {
      ++mesh_turn_;
      turn_seat_ = pending_next_seat_;
      wind_ = pending_next_wind_;
      state_ = isLocalTurn() ? PLAYER_AIM : MESH_WAIT;
      // isLocalTurn checks state, so set explicitly for the live local seat.
      if (turn_seat_ == local_seat_ && local_seat_ >= 0
          && mesh_players_[local_seat_].tank.health > 0) state_ = PLAYER_AIM;
    }
  }
  state_since_ms_ = millis();
  if (timer_) {
    if (state_ == CPU_THINK) {
      lv_timer_set_period(timer_, 100);
      lv_timer_resume(timer_);
    } else {
      lv_timer_pause(timer_);
    }
  }
  updateHud();
  if (mode_ != MODE_LOCAL && state_ == PLAYER_AIM) touchArtilleryTurnAlert();
}

void ArtilleryGame::showReplay(const char* text) {
  if (!replay_btn_) {
    replay_btn_ = lv_btn_create(root_);
    lv_obj_set_size(replay_btn_, width_ < 400 ? 170 : 210, 52);
    lv_obj_set_pos(replay_btn_, (width_ - lv_obj_get_width(replay_btn_)) / 2,
                   canvas_y_ + (height_ - 52) / 2);
    lv_obj_add_event_cb(replay_btn_, replayCb, LV_EVENT_CLICKED, this);
    replay_label_ = lv_label_create(replay_btn_);
    lv_obj_set_style_text_align(replay_label_, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_center(replay_label_);
  }
  lv_label_set_text(replay_label_, text);
  lv_obj_clear_flag(replay_btn_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(replay_btn_);
}

void ArtilleryGame::hideReplay() {
  if (replay_btn_) lv_obj_add_flag(replay_btn_, LV_OBJ_FLAG_HIDDEN);
}

void ArtilleryGame::updateHud() {
  if (!root_) return;
  const Tank* aim = &player_;
  if (mode_ != MODE_LOCAL && local_seat_ >= 0 && local_seat_ < mesh_player_count_
      && mesh_players_[local_seat_].used) aim = &mesh_players_[local_seat_].tank;
  if (angle_label_) lv_label_set_text_fmt(angle_label_, "A %d", aim->angle);
  if (power_label_) lv_label_set_text_fmt(power_label_, "P %d", aim->power);

  const char* arrow = wind_ < 0 ? "<" : (wind_ > 0 ? ">" : "-");
  if (info_label_) {
    if (mode_ == MODE_LOCAL) {
      lv_label_set_text_fmt(info_label_, "Wind %s%d  You %d  CPU %d",
                            arrow, abs(wind_), player_.health, cpu_.health);
    } else {
      int alive = 0, a = 0, b = 0;
      for (int i = 0; i < mesh_player_count_; ++i) {
        if (!mesh_players_[i].used || mesh_players_[i].tank.health <= 0) continue;
        ++alive;
        if ((mesh_players_[i].team & 1) == 0) ++a; else ++b;
      }
      if (teams_enabled_)
        lv_label_set_text_fmt(info_label_, "W%s%d  Alive %d  A%d B%d", arrow, abs(wind_), alive, a, b);
      else
        lv_label_set_text_fmt(info_label_, "W%s%d  Alive %d/%d", arrow, abs(wind_), alive, mesh_player_count_);
    }
  }

  if (turn_label_) {
    const char* turn = "Your turn";
    if (state_ == PLAYER_FLIGHT || state_ == CPU_FLIGHT) turn = "Shot";
    else if (state_ == CPU_THINK) turn = "CPU aiming";
    else if (state_ == MESH_WAIT && turn_seat_ >= 0 && turn_seat_ < mesh_player_count_)
      turn = mesh_players_[turn_seat_].name;
    else if (state_ == SHOT_PENDING) turn = "Sending...";
    else if (state_ == EXPLODING && mode_ != MODE_LOCAL && !mesh_result_received_) turn = "Syncing...";
    else if (state_ == EXPLODING) turn = "BOOM";
    else if (state_ == GAME_OVER) turn = "Game over";
    lv_label_set_text(turn_label_, turn);
    lv_obj_align(turn_label_, LV_ALIGN_TOP_RIGHT, -5, canvas_y_ + 3);
  }
  const bool enabled = view_ == VIEW_GAME && isLocalTurn();
  lv_obj_t* controls[] = {angle_minus_, angle_plus_, power_minus_, power_plus_, fire_btn_};
  for (lv_obj_t* control : controls) {
    if (!control) continue;
    if (enabled) lv_obj_clear_state(control, LV_STATE_DISABLED);
    else         lv_obj_add_state(control, LV_STATE_DISABLED);
  }
}

lv_obj_t* ArtilleryGame::createOverlay(const char* title) {
  clearOverlay();
  const int top = (int)appPageContentTop();
  const int h = (int)lv_obj_get_height(root_) - top;
  overlay_ = lv_obj_create(root_);
  lv_obj_remove_style_all(overlay_);
  lv_obj_set_size(overlay_, width_, h);
  lv_obj_set_pos(overlay_, 0, top);
  lv_obj_set_style_bg_color(overlay_, lv_color_hex(0x0C1D29), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(overlay_, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_add_flag(overlay_, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(overlay_, LV_OBJ_FLAG_SCROLLABLE);
  overlay_title_ = lv_label_create(overlay_);
  lv_obj_set_width(overlay_title_, width_ - 12);
  lv_obj_set_pos(overlay_title_, 6, 5);
  lv_obj_set_style_text_align(overlay_title_, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_style_text_color(overlay_title_, lv_color_hex(kText), LV_PART_MAIN);
  lv_label_set_text(overlay_title_, title ? title : "");
  overlay_body_ = lv_label_create(overlay_);
  lv_obj_set_width(overlay_body_, width_ - 16);
  lv_obj_set_pos(overlay_body_, 8, 30);
  lv_obj_set_style_text_color(overlay_body_, lv_color_hex(0xB9CBD5), LV_PART_MAIN);
  lv_obj_set_style_text_align(overlay_body_, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_label_set_long_mode(overlay_body_, LV_LABEL_LONG_WRAP);
  lv_obj_move_foreground(overlay_);
  return overlay_;
}

lv_obj_t* ArtilleryGame::overlayButton(const char* text, int x, int y, int w, lv_event_cb_t cb) {
  if (!overlay_) return nullptr;
  lv_obj_t* button = lv_btn_create(overlay_);
  lv_obj_set_size(button, w, 29);
  lv_obj_set_pos(button, x, y);
  lv_obj_set_style_pad_all(button, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(button, 5, LV_PART_MAIN);
  lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, this);
  lv_obj_t* label = lv_label_create(button);
  lv_label_set_text(label, text);
  lv_obj_center(label);
  return button;
}

void ArtilleryGame::clearOverlay() {
  if (overlay_) lv_obj_del_async(overlay_);
  overlay_ = overlay_title_ = overlay_body_ = nullptr;
  overlay_primary_ = overlay_secondary_ = nullptr;
  for (lv_obj_t*& button : mode_buttons_) button = nullptr;
  for (PickerRow& row : picker_rows_) {
    row.game = this;
    row.mesh_index = -1;
    row.button = row.label = nullptr;
  }
}

void ArtilleryGame::showModeMenu() {
  if (is_host_ && (mode_ == MODE_DM || mode_ == MODE_ROOM) && match_id_ != 0) {
    char frame[40];
    snprintf(frame, sizeof frame, "~SG1|Q|%08lX", (unsigned long)match_id_);
    broadcast(frame);
  } else if (!is_host_ && state_ != GAME_OVER
             && (mode_ == MODE_DM || mode_ == MODE_ROOM) && match_id_ != 0) {
    char frame[40];
    snprintf(frame, sizeof frame, "~SG1|Q|%08lX", (unsigned long)match_id_);
    sendTransport(frame);
  }
  match_id_ = 0;
  mode_ = MODE_NONE;
  view_ = VIEW_MODE;
  match_started_ = false;
  pending_begin_ = false;
  is_host_ = false;
  picker_cursor_ = 0;
  if (timer_) lv_timer_pause(timer_);
  hideReplay();
  createOverlay("ARTILLERY");
  lv_label_set_text(overlay_body_, "Choose a game\n1 Local   2 Private   3 Room");
  const int gap = 7;
  const int bw = (width_ - 24 - gap * 2) / 3;
  const int y = 82;
  mode_buttons_[0] = overlayButton("LOCAL", 12, y, bw, localModeCb);
  mode_buttons_[1] = overlayButton("PRIVATE", 12 + bw + gap, y, bw, dmModeCb);
  mode_buttons_[2] = overlayButton("ROOM", 12 + (bw + gap) * 2, y, bw, roomModeCb);
  if (mode_buttons_[0]) lv_obj_set_style_bg_color(mode_buttons_[0], lv_color_hex(0x287D83), LV_PART_MAIN);
  updateHud();
}

void ArtilleryGame::showContactPicker(bool rooms) {
  mode_ = rooms ? MODE_ROOM : MODE_DM;
  view_ = VIEW_PICKER;
  picker_rooms_ = rooms;
  picker_page_ = 0;
  picker_cursor_ = 0;
  picker_total_ = 0;
  if (timer_) lv_timer_pause(timer_);
  invite_target_count_ = 0;
  memset(invite_targets_, 0, sizeof(invite_targets_));
  memset(transport_pub_, 0, sizeof(transport_pub_));
  transport_name_[0] = '\0';
  createOverlay(rooms ? "Choose a room" : "Choose up to 3 players");
  lv_obj_add_flag(overlay_body_, LV_OBJ_FLAG_HIDDEN);
  const int row_y = 27;
  for (int i = 0; i < kPickerRows; ++i) {
    PickerRow& row = picker_rows_[i];
    row.game = this;
    row.button = lv_btn_create(overlay_);
    lv_obj_set_size(row.button, width_ - 16, 27);
    lv_obj_set_pos(row.button, 8, row_y + i * 29);
    lv_obj_set_style_pad_all(row.button, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(row.button, 4, LV_PART_MAIN);
    lv_obj_add_event_cb(row.button, pickerRowCb, LV_EVENT_CLICKED, &row);
    row.label = lv_label_create(row.button);
    lv_obj_set_width(row.label, width_ - 28);
    lv_obj_set_style_text_align(row.label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_obj_center(row.label);
  }
  const int by = (int)lv_obj_get_height(overlay_) - 31;
  overlayButton("<", 7, by, 36, pickerPrevCb);
  overlayButton(">", 47, by, 36, pickerNextCb);
  overlay_primary_ = overlayButton("START", width_ - 151, by, 76, pickerStartCb);
  overlayButton("BACK", width_ - 71, by, 64, overlayBackCb);
  refreshPicker();
}

void ArtilleryGame::refreshPicker() {
  if (!overlay_ || view_ != VIEW_PICKER) return;
  const uint8_t* self = the_mesh.getSelfPubKey();
  int eligible = 0;
  const int first = picker_page_ * kPickerRows;
  int shown = 0;
  for (int i = 0; i < the_mesh.getNumContacts(); ++i) {
    ContactInfo contact{};
    if (!the_mesh.getContactByIdx((uint32_t)i, contact) || !contact.name[0]) continue;
    const bool type_ok = picker_rooms_ ? contact.type == ADV_TYPE_ROOM : contact.type == ADV_TYPE_CHAT;
    if (!type_ok || (self && memcmp(contact.id.pub_key, self, 32) == 0)) continue;
    if (eligible >= first && shown < kPickerRows) {
      PickerRow& row = picker_rows_[shown++];
      row.mesh_index = i;
      bool selected = false;
      if (picker_rooms_) selected = memcmp(transport_pub_, contact.id.pub_key, 32) == 0;
      else {
        for (const InviteTarget& target : invite_targets_)
          if (target.used && memcmp(target.pub, contact.id.pub_key, 32) == 0) selected = true;
      }
      lv_label_set_text_fmt(row.label, "%s %.28s", selected ? "[x]" : "[ ]", contact.name);
      lv_obj_set_style_bg_color(row.button,
          lv_color_hex((shown - 1) == picker_cursor_ ? 0x287D83 : (selected ? 0x315B57 : 0x233746)),
          LV_PART_MAIN);
      lv_obj_clear_flag(row.button, LV_OBJ_FLAG_HIDDEN);
    }
    ++eligible;
  }
  picker_total_ = eligible;
  const int pages = eligible > 0 ? (eligible + kPickerRows - 1) / kPickerRows : 1;
  if (picker_page_ >= pages) {
    picker_page_ = pages - 1;
    refreshPicker();
    return;
  }
  for (int i = shown; i < kPickerRows; ++i) {
    picker_rows_[i].mesh_index = -1;
    if (picker_rows_[i].button) lv_obj_add_flag(picker_rows_[i].button, LV_OBJ_FLAG_HIDDEN);
  }
  if (shown == 0) picker_cursor_ = 0;
  else if (picker_cursor_ >= shown) picker_cursor_ = shown - 1;
  lv_label_set_text_fmt(overlay_title_, "%s  %d/%d  (%d selected)",
      picker_rooms_ ? "Room" : "Private", picker_page_ + 1, pages,
      picker_rooms_ ? (transport_name_[0] ? 1 : 0) : invite_target_count_);
  const bool ready = picker_rooms_ ? transport_name_[0] != '\0' : invite_target_count_ > 0;
  if (overlay_primary_) {
    if (ready) lv_obj_clear_state(overlay_primary_, LV_STATE_DISABLED);
    else       lv_obj_add_state(overlay_primary_, LV_STATE_DISABLED);
  }
}

void ArtilleryGame::movePicker(int delta) {
  if (view_ != VIEW_PICKER || picker_total_ <= 0) return;
  int visible = 0;
  for (const PickerRow& row : picker_rows_) if (row.mesh_index >= 0) ++visible;
  if (visible <= 0) return;
  picker_cursor_ += delta;
  if (picker_cursor_ < 0) {
    if (picker_page_ > 0) { --picker_page_; picker_cursor_ = kPickerRows - 1; }
    else picker_cursor_ = 0;
  } else if (picker_cursor_ >= visible) {
    if ((picker_page_ + 1) * kPickerRows < picker_total_) { ++picker_page_; picker_cursor_ = 0; }
    else picker_cursor_ = visible - 1;
  }
  refreshPicker();
}

void ArtilleryGame::togglePickerRow(int mesh_index) {
  ContactInfo contact{};
  if (mesh_index < 0 || !the_mesh.getContactByIdx((uint32_t)mesh_index, contact)) return;
  if (picker_rooms_) {
    memcpy(transport_pub_, contact.id.pub_key, 32);
    safeName(transport_name_, sizeof transport_name_, contact.name);
  } else {
    for (InviteTarget& target : invite_targets_) {
      if (target.used && memcmp(target.pub, contact.id.pub_key, 32) == 0) {
        target = InviteTarget{};
        --invite_target_count_;
        refreshPicker();
        return;
      }
    }
    if (invite_target_count_ >= kDmMaxPlayers - 1) return;
    for (InviteTarget& target : invite_targets_) {
      if (!target.used) {
        target.used = true;
        memcpy(target.pub, contact.id.pub_key, 32);
        safeName(target.name, sizeof target.name, contact.name);
        ++invite_target_count_;
        break;
      }
    }
  }
  refreshPicker();
}

int ArtilleryGame::findMeshPlayer(const uint8_t id[4]) const {
  if (!id) return -1;
  for (int i = 0; i < mesh_player_count_; ++i)
    if (mesh_players_[i].used && memcmp(mesh_players_[i].id, id, 4) == 0) return i;
  return -1;
}

int ArtilleryGame::addMeshPlayer(const uint8_t id[4], const uint8_t* full_pub,
                                 const char* name, uint8_t team, uint32_t revision) {
  int seat = findMeshPlayer(id);
  if (seat < 0) {
    const int cap = mode_ == MODE_DM ? kDmMaxPlayers : kMaxPlayers;
    if (mesh_player_count_ >= cap) return -1;
    seat = mesh_player_count_++;
    mesh_players_[seat] = MeshPlayer{};
    mesh_players_[seat].used = true;
    memcpy(mesh_players_[seat].id, id, 4);
    mesh_players_[seat].tank.health = 3;
    mesh_players_[seat].tank.power = 65;
  }
  MeshPlayer& player = mesh_players_[seat];
  if (revision >= player.revision) {
    player.revision = revision;
    player.team = team & 1;
    safeName(player.name, sizeof player.name, name);
  }
  if (full_pub) {
    player.has_pub = true;
    memcpy(player.pub, full_pub, 32);
  }
  return seat;
}

void ArtilleryGame::beginHostLobby() {
  if (view_ != VIEW_PICKER) return;
  if ((mode_ == MODE_DM && invite_target_count_ == 0)
      || (mode_ == MODE_ROOM && !transport_name_[0])) return;
  if (mode_ == MODE_ROOM && !the_mesh.uiHasConnectionTo(transport_pub_)) {
    lv_label_set_text(overlay_title_, "Join this room in Contacts first");
    return;
  }

  memset(mesh_players_, 0, sizeof(mesh_players_));
  mesh_player_count_ = 0;
  local_seat_ = 0;
  is_host_ = true;
  match_started_ = false;
  pending_begin_ = false;
  teams_enabled_ = false;
  roster_revision_ = 1;
  match_id_ = (uint32_t)random(1, 0x7FFFFFFF);
  terrain_seed_ = (uint32_t)random(1, 0x7FFFFFFF);
  const uint8_t* self = the_mesh.getSelfPubKey();
  memcpy(host_id_, self, 4);
  const char* self_name = the_mesh.getNodePrefs()->node_name;
  addMeshPlayer(host_id_, self, self_name, 0, roster_revision_);
  showLobby();
  sendInviteFrames();
}

void ArtilleryGame::showInvitePrompt() {
  if (!s_pending_invite.valid || match_started_) return;
  view_ = VIEW_INVITE;
  if (timer_) lv_timer_pause(timer_);
  createOverlay("ARTILLERY INVITE");
  char body[128];
  snprintf(body, sizeof body, "%.15s invited you\n%s match - up to %d players\n%s",
           s_pending_invite.host_name,
           s_pending_invite.room ? "Room" : "Private",
           s_pending_invite.room ? kMaxPlayers : kDmMaxPlayers,
           s_pending_invite.teams ? "Teams enabled" : "Free for all");
  lv_label_set_text(overlay_body_, body);
  const int y = (int)lv_obj_get_height(overlay_) - 39;
  overlayButton("ACCEPT", width_ / 2 - 104, y, 96, inviteAcceptCb);
  overlayButton("DECLINE", width_ / 2 + 8, y, 96, inviteDeclineCb);
}

void ArtilleryGame::acceptInvite() {
  if (!s_pending_invite.valid) { showModeMenu(); return; }
  mode_ = s_pending_invite.room ? MODE_ROOM : MODE_DM;
  view_ = VIEW_LOBBY;
  is_host_ = false;
  match_started_ = false;
  pending_begin_ = false;
  teams_enabled_ = s_pending_invite.teams;
  match_id_ = s_pending_invite.match;
  terrain_seed_ = s_pending_invite.seed;
  memcpy(host_id_, s_pending_invite.host_id, 4);
  memcpy(transport_pub_, s_pending_invite.transport_pub, 32);
  safeName(transport_name_, sizeof transport_name_, s_pending_invite.transport_name);
  memset(mesh_players_, 0, sizeof(mesh_players_));
  mesh_player_count_ = 0;
  local_seat_ = -1;
  roster_revision_ = 0;
  addMeshPlayer(host_id_, mode_ == MODE_DM ? transport_pub_ : nullptr,
                s_pending_invite.host_name, 0, 0);
  s_pending_invite.valid = false;
  showLobby();
  sendJoin();
  mesh_retry_at_ = millis() + 10000;
  if (timer_) { lv_timer_set_period(timer_, 250); lv_timer_resume(timer_); }
}

void ArtilleryGame::declineInvite() {
  s_pending_invite.valid = false;
  showModeMenu();
}

void ArtilleryGame::showLobby() {
  view_ = VIEW_LOBBY;
  if (timer_) {
    if (!is_host_ && local_seat_ < 0) {
      lv_timer_set_period(timer_, 250);
      lv_timer_resume(timer_);
    } else {
      lv_timer_pause(timer_);
    }
  }
  createOverlay(is_host_ ? "MATCH LOBBY - HOST" : "MATCH LOBBY");
  const int y = (int)lv_obj_get_height(overlay_) - 34;
  if (is_host_) {
    overlay_primary_ = overlayButton("START", 5, y, 68, lobbyPrimaryCb);
    mode_buttons_[0] = overlayButton("FFA", 77, y, 68, lobbyModeCb);
    overlay_secondary_ = overlayButton("MY A", 149, y, 68, lobbyTeamCb);
    overlayButton("BACK", width_ - 73, y, 68, overlayBackCb);
  } else {
    overlay_secondary_ = overlayButton("MY TEAM", width_ / 2 - 75, y, 72, lobbyTeamCb);
    overlayButton("LEAVE", width_ / 2 + 3, y, 72, overlayBackCb);
  }
  refreshLobby();
}

void ArtilleryGame::refreshLobby() {
  if (!overlay_ || view_ != VIEW_LOBBY) return;
  char body[512];
  int p = snprintf(body, sizeof body, "%s  %d/%d\n",
                   teams_enabled_ ? "TEAMS" : "FREE FOR ALL",
                   mesh_player_count_, mode_ == MODE_DM ? kDmMaxPlayers : kMaxPlayers);
  for (int row = 0; row < 5 && p < (int)sizeof(body); ++row) {
    const int left = row;
    const int right = row + 5;
    if (left < mesh_player_count_ && mesh_players_[left].used) {
      p += snprintf(body + p, sizeof(body) - (size_t)p, "%d%c %-9.9s",
                    left + 1, (char)('A' + (mesh_players_[left].team & 1)),
                    mesh_players_[left].name);
    } else {
      p += snprintf(body + p, sizeof(body) - (size_t)p, "             ");
    }
    if (right < mesh_player_count_ && mesh_players_[right].used) {
      p += snprintf(body + p, sizeof(body) - (size_t)p, "  %d%c %.9s",
                    right + 1, (char)('A' + (mesh_players_[right].team & 1)),
                    mesh_players_[right].name);
    }
    if (row < 4) p += snprintf(body + p, sizeof(body) - (size_t)p, "\n");
  }
  if (!is_host_ && local_seat_ < 0)
    snprintf(body + p, sizeof(body) - (size_t)p, "\nWaiting for host...");
  lv_label_set_text(overlay_body_, body);
  if (mode_buttons_[0]) {
    lv_obj_t* label = lv_obj_get_child(mode_buttons_[0], 0);
    if (label) lv_label_set_text(label, teams_enabled_ ? "TEAMS" : "FFA");
  }
  if (overlay_secondary_) {
    lv_obj_t* label = lv_obj_get_child(overlay_secondary_, 0);
    char team_text[12] = "TEAM ?";
    if (local_seat_ >= 0 && local_seat_ < mesh_player_count_)
      snprintf(team_text, sizeof team_text, "MY %c", 'A' + (mesh_players_[local_seat_].team & 1));
    if (label) lv_label_set_text(label, team_text);
  }
  if (overlay_primary_) {
    if (mesh_player_count_ >= 2) lv_obj_clear_state(overlay_primary_, LV_STATE_DISABLED);
    else                         lv_obj_add_state(overlay_primary_, LV_STATE_DISABLED);
  }
}

void ArtilleryGame::toggleTeams() {
  if (!is_host_ || view_ != VIEW_LOBBY) return;
  teams_enabled_ = !teams_enabled_;
  ++roster_revision_;
  char frame[64];
  snprintf(frame, sizeof frame, "~SG1|C|%08lX|%lu|%u|%d",
           (unsigned long)match_id_, (unsigned long)roster_revision_,
           teams_enabled_ ? 1u : 0u, mesh_player_count_);
  broadcast(frame);
  refreshLobby();
}

void ArtilleryGame::toggleLocalTeam() {
  if (view_ != VIEW_LOBBY || local_seat_ < 0 || local_seat_ >= mesh_player_count_) return;
  MeshPlayer& local = mesh_players_[local_seat_];
  local.team ^= 1;
  if (is_host_) {
    local.revision = ++roster_revision_;
    sendPlayerRecord(local_seat_);
  } else {
    sendTeamRequest(local.team);
  }
  refreshLobby();
}

void ArtilleryGame::configureMeshTanks() {
  for (int i = 0; i < mesh_player_count_; ++i) {
    if (!mesh_players_[i].used) continue;
    Tank& tank = mesh_players_[i].tank;
    tank.x = (i + 1) * width_ / (mesh_player_count_ + 1);
    tank.health = 3;
    tank.power = 65;
    tank.angle = tank.x < width_ / 2 ? 45 : 135;
  }
  generateTerrain();
  for (int i = 0; i < mesh_player_count_; ++i)
    if (mesh_players_[i].used) flattenForTank(mesh_players_[i].tank.x);
}

void ArtilleryGame::startMeshMatch(uint8_t expected_players) {
  int used = 0;
  for (int i = 0; i < mesh_player_count_; ++i) if (mesh_players_[i].used) ++used;
  if (used < expected_players || expected_players < 2) {
    pending_begin_ = true;
    pending_begin_count_ = expected_players;
    mesh_retry_at_ = millis() + 2000;
    if (timer_) { lv_timer_set_period(timer_, 250); lv_timer_resume(timer_); }
    return;
  }
  pending_begin_ = false;
  mesh_player_count_ = expected_players;
  match_started_ = true;
  view_ = VIEW_GAME;
  mesh_turn_ = 0;
  last_result_turn_ = UINT16_MAX;
  turn_seat_ = 0;
  shot_seat_ = -1;
  pending_next_seat_ = -1;
  mesh_result_received_ = false;
  last_result_frame_[0] = '\0';
  configureMeshTanks();
  clearOverlay();
  hideReplay();
  state_ = turn_seat_ == local_seat_ ? PLAYER_AIM : MESH_WAIT;
  state_since_ms_ = millis();
  if (timer_) lv_timer_pause(timer_);
  updateHud();
  render();
}

void ArtilleryGame::startHostedMatch() {
  if (!is_host_ || view_ != VIEW_LOBBY || mesh_player_count_ < 2) return;
  if (teams_enabled_) {
    int team_mask = 0;
    for (int i = 0; i < mesh_player_count_; ++i) team_mask |= 1 << (mesh_players_[i].team & 1);
    if (team_mask != 3) {
      ++roster_revision_;
      for (int i = 0; i < mesh_player_count_; ++i) {
        mesh_players_[i].team = i & 1;
        mesh_players_[i].revision = roster_revision_;
      }
    }
  }
  wind_ = (int)random(-5, 6);
  uint32_t team_bits = 0;
  for (int i = 0; i < mesh_player_count_; ++i)
    if (mesh_players_[i].team & 1) team_bits |= 1u << i;
  snprintf(last_begin_frame_, sizeof last_begin_frame_,
           "~SG1|B|%08lX|%08lX|%d|0|%d|%u|%lX",
           (unsigned long)match_id_, (unsigned long)terrain_seed_, mesh_player_count_,
           wind_, teams_enabled_ ? 1u : 0u, (unsigned long)team_bits);
  broadcast(last_begin_frame_);
  startMeshMatch((uint8_t)mesh_player_count_);
}

bool ArtilleryGame::sendProtocolTo(const ContactInfo& recipient, const char* text) {
  if (!text || strlen(text) > MAX_TEXT_LEN) return false;
  static uint8_t attempt = 4;
  ContactInfo tx = recipient;
  uint32_t expected_ack = 0, est_timeout = 0;
  const uint32_t ts = the_mesh.getRTCClock()->getCurrentTimeUnique();
  const int result = the_mesh.sendMessage(tx, ts, attempt++, text, expected_ack, est_timeout);
  if (attempt < 4) attempt = 4;
  if (result == MSG_SEND_FAILED) return false;
  the_mesh.uiRegisterExpectedAck(expected_ack, recipient.id.pub_key);
  return true;
}

bool ArtilleryGame::sendToPub(const uint8_t pub[32], const char* text) {
  ContactInfo* contact = the_mesh.lookupContactByPubKey((uint8_t*)pub, PUB_KEY_SIZE);
  return contact && sendProtocolTo(*contact, text);
}

bool ArtilleryGame::sendTransport(const char* text) {
  return sendToPub(transport_pub_, text);
}

void ArtilleryGame::broadcast(const char* text) {
  if (mode_ == MODE_ROOM) {
    sendTransport(text);
    return;
  }
  if (!is_host_) {
    sendTransport(text);
    return;
  }
  for (int i = 1; i < mesh_player_count_; ++i) {
    const MeshPlayer& player = mesh_players_[i];
    if (player.used && player.has_pub) sendToPub(player.pub, text);
  }
}

void ArtilleryGame::sendInviteFrames() {
  char host_name[kMaxName + 1];
  safeName(host_name, sizeof host_name, the_mesh.getNodePrefs()->node_name);
  char frame[112];
  snprintf(frame, sizeof frame, "~SG1|I|%08lX|%08lX|%c|%u|%s",
           (unsigned long)match_id_, (unsigned long)terrain_seed_,
           mode_ == MODE_ROOM ? 'R' : 'D', teams_enabled_ ? 1u : 0u, host_name);
  if (mode_ == MODE_ROOM) sendTransport(frame);
  else for (const InviteTarget& target : invite_targets_)
    if (target.used) sendToPub(target.pub, frame);
}

void ArtilleryGame::sendJoin() {
  char name[kMaxName + 1];
  safeName(name, sizeof name, the_mesh.getNodePrefs()->node_name);
  char frame[80];
  snprintf(frame, sizeof frame, "~SG1|J|%08lX|1|%s", (unsigned long)match_id_, name);
  sendTransport(frame);
}

void ArtilleryGame::sendPlayerRecord(int seat, const uint8_t* only_pub) {
  if (seat < 0 || seat >= mesh_player_count_ || !mesh_players_[seat].used) return;
  const MeshPlayer& player = mesh_players_[seat];
  char idhex[9];
  idToHex(player.id, idhex);
  char frame[104];
  snprintf(frame, sizeof frame, "~SG1|P|%08lX|%lu|%d|%s|%u|%s",
           (unsigned long)match_id_, (unsigned long)roster_revision_, seat,
           idhex, (unsigned)(player.team & 1), player.name);
  if (only_pub) sendToPub(only_pub, frame);
  else broadcast(frame);
}

void ArtilleryGame::sendRosterSnapshot(const uint8_t* only_pub) {
  if (mode_ == MODE_ROOM && !only_pub) {
    for (int start = 0; start < mesh_player_count_; start += 4) {
      char frame[MAX_TEXT_LEN + 1];
      int p = snprintf(frame, sizeof frame, "~SG1|V|%08lX|%lu|%d|",
                       (unsigned long)match_id_, (unsigned long)roster_revision_, start);
      const int end = (start + 4 < mesh_player_count_) ? start + 4 : mesh_player_count_;
      for (int i = start; i < end && p > 0 && p < (int)sizeof(frame); ++i) {
        char idhex[9];
        idToHex(mesh_players_[i].id, idhex);
        p += snprintf(frame + p, sizeof(frame) - (size_t)p, "%s%s,%u,%s",
                      i == start ? "" : ";", idhex,
                      (unsigned)(mesh_players_[i].team & 1), mesh_players_[i].name);
      }
      if (p > 0 && p <= MAX_TEXT_LEN) sendTransport(frame);
    }
    return;
  }
  for (int i = 0; i < mesh_player_count_; ++i) sendPlayerRecord(i, only_pub);
}

void ArtilleryGame::sendTeamRequest(uint8_t team) {
  char frame[56];
  snprintf(frame, sizeof frame, "~SG1|T|%08lX|%u", (unsigned long)match_id_, (unsigned)(team & 1));
  sendTransport(frame);
}

void ArtilleryGame::sendShotRequest() {
  if (local_seat_ < 0 || local_seat_ >= mesh_player_count_) return;
  const Tank& tank = mesh_players_[local_seat_].tank;
  char frame[72];
  snprintf(frame, sizeof frame, "~SG1|F|%08lX|%u|%d|%d",
           (unsigned long)match_id_, (unsigned)mesh_turn_, tank.angle, tank.power);
  sendTransport(frame);
}

void ArtilleryGame::broadcastShot(int seat, int angle, int power) {
  if (!is_host_ || seat != turn_seat_ || state_ == PLAYER_FLIGHT || state_ == EXPLODING) return;
  char frame[88];
  snprintf(frame, sizeof frame, "~SG1|S|%08lX|%u|%d|%d|%d|%d",
           (unsigned long)match_id_, (unsigned)mesh_turn_, seat, angle, power, wind_);
  broadcast(frame);
  launchMeshShot(seat, angle, power, wind_);
}

void ArtilleryGame::resolveMeshShot(int x, int y, bool missed) {
  if (!is_host_ || last_result_turn_ == mesh_turn_) return;
  if (!missed) {
    for (int i = 0; i < mesh_player_count_; ++i)
      if (mesh_players_[i].used) damageTank(mesh_players_[i].tank, x, y);
    carveCrater(x, y);
  }
  char health[kMaxPlayers + 1];
  for (int i = 0; i < mesh_player_count_; ++i)
    health[i] = (char)('0' + (mesh_players_[i].tank.health < 0 ? 0 : mesh_players_[i].tank.health));
  health[mesh_player_count_] = '\0';
  pending_next_seat_ = nextAliveSeat(turn_seat_);
  pending_next_wind_ = (int)random(-5, 6);
  const int nx = missed ? -1 : x * (kWorldWidth - 1) / (width_ > 1 ? width_ - 1 : 1);
  const int ny = missed ? -1 : y * (kWorldHeight - 1) / (height_ > 1 ? height_ - 1 : 1);
  snprintf(last_result_frame_, sizeof last_result_frame_,
           "~SG1|X|%08lX|%u|%d|%d|%s|%d|%d",
           (unsigned long)match_id_, (unsigned)mesh_turn_, nx, ny, health,
           pending_next_seat_, pending_next_wind_);
  last_result_turn_ = mesh_turn_;
  mesh_result_received_ = true;
  broadcast(last_result_frame_);
}

void ArtilleryGame::applyMeshResult(uint16_t turn, int nx, int ny, const char* health,
                                    int next_seat, int next_wind) {
  if (turn != mesh_turn_ || last_result_turn_ == turn || !health
      || strlen(health) != (size_t)mesh_player_count_) return;
  for (int i = 0; i < mesh_player_count_; ++i) {
    if (health[i] < '0' || health[i] > '3') return;
  }
  for (int i = 0; i < mesh_player_count_; ++i)
    mesh_players_[i].tank.health = health[i] - '0';
  if (nx >= 0 && ny >= 0) {
    const int x = nx * (width_ - 1) / (kWorldWidth - 1);
    const int y = ny * (height_ - 1) / (kWorldHeight - 1);
    explosion_x_ = x;
    explosion_y_ = y;
    carveCrater(x, y);
  } else {
    explosion_x_ = explosion_y_ = -1;
  }
  pending_next_seat_ = next_seat;
  pending_next_wind_ = next_wind;
  last_result_turn_ = turn;
  mesh_result_received_ = true;
  state_ = EXPLODING;
  state_since_ms_ = millis();
  if (timer_) { lv_timer_set_period(timer_, kTickMs); lv_timer_resume(timer_); }
  updateHud();
  render();
}

bool ArtilleryGame::authorIsHost(const uint8_t* author_id, size_t author_id_len) const {
  if (!author_id || author_id_len < 4 || memcmp(author_id, host_id_, 4) != 0) return false;
  if (mode_ == MODE_DM && !is_host_)
    return author_id_len == 32 && memcmp(author_id, transport_pub_, 32) == 0;
  return true;
}

bool ArtilleryGame::senderAllowedToJoin(const uint8_t* author_id, size_t author_id_len) const {
  if (!author_id || author_id_len < 4) return false;
  if (mode_ == MODE_ROOM) return true;
  if (author_id_len != 32) return false;
  for (const InviteTarget& target : invite_targets_)
    if (target.used && memcmp(target.pub, author_id, 32) == 0) return true;
  return false;
}

void ArtilleryGame::receiveProtocol(const ContactInfo& transport,
                                    const uint8_t* author_id, size_t author_id_len,
                                    bool via_room, const char* text) {
  ProtocolFields frame;
  if (!splitProtocol(text, frame) || frame.count < 3 || !author_id || author_id_len < 4) return;
  uint32_t match = 0;
  if (!parseUnsigned(frame.field[2], match, 16) || match != match_id_) return;
  if ((mode_ == MODE_ROOM) != via_room) return;
  if (mode_ == MODE_ROOM && memcmp(transport.id.pub_key, transport_pub_, 32) != 0) return;
  if (mode_ == MODE_DM && !is_host_ && memcmp(transport.id.pub_key, transport_pub_, 32) != 0) return;

  const char command = frame.field[1][0];
  if (command == 'J' && frame.count == 5 && is_host_ && !match_started_) {
    uint32_t team = 0;
    if (!parseUnsigned(frame.field[3], team) || team > 1 || !senderAllowedToJoin(author_id, author_id_len)) return;
    if (memcmp(author_id, host_id_, 4) == 0) return;  // our own room echo
    int seat = findMeshPlayer(author_id);
    if (seat < 0) {
      seat = addMeshPlayer(author_id, mode_ == MODE_DM ? author_id : nullptr,
                           frame.field[4], (uint8_t)team, ++roster_revision_);
      if (seat < 0) return;
    } else {
      mesh_players_[seat].team = (uint8_t)team;
      mesh_players_[seat].revision = ++roster_revision_;
      safeName(mesh_players_[seat].name, sizeof mesh_players_[seat].name, frame.field[4]);
    }
    char config[72];
    snprintf(config, sizeof config, "~SG1|C|%08lX|%lu|%u|%d",
             (unsigned long)match_id_, (unsigned long)roster_revision_,
             teams_enabled_ ? 1u : 0u, mesh_player_count_);
    broadcast(config);
    if (mode_ == MODE_DM) {
      sendRosterSnapshot(author_id);
      sendPlayerRecord(seat);
    } else {
      sendRosterSnapshot();
    }
    refreshLobby();
    return;
  }

  if (command == 'V' && frame.count == 6 && !is_host_ && authorIsHost(author_id, author_id_len)
      && !match_started_) {
    uint32_t revision = 0, start_u = 0;
    const int cap = mode_ == MODE_DM ? kDmMaxPlayers : kMaxPlayers;
    if (!parseUnsigned(frame.field[3], revision) || revision < roster_revision_
        || !parseUnsigned(frame.field[4], start_u) || start_u >= (uint32_t)cap) return;
    struct TempPlayer { uint8_t id[4]; uint8_t team; char name[kMaxName + 1]; } temp[4] = {};
    int records = 0;
    char* record = frame.field[5];
    while (record && *record && records < 4) {
      char* next = strchr(record, ';');
      if (next) *next++ = '\0';
      char* comma1 = strchr(record, ',');
      if (!comma1) return;
      *comma1++ = '\0';
      char* comma2 = strchr(comma1, ',');
      if (!comma2) return;
      *comma2++ = '\0';
      uint32_t team = 0;
      if (!hexToId(record, temp[records].id) || !parseUnsigned(comma1, team)
          || team > 1 || !comma2[0] || start_u + (uint32_t)records >= (uint32_t)cap) return;
      temp[records].team = (uint8_t)team;
      safeName(temp[records].name, sizeof temp[records].name, comma2);
      ++records;
      record = next;
    }
    if (records == 0 || (record && *record)) return;
    const uint8_t* self = the_mesh.getSelfPubKey();
    for (int r = 0; r < records; ++r) {
      const int seat = (int)start_u + r;
      for (int i = 0; i < mesh_player_count_; ++i) {
        if (i != seat && mesh_players_[i].used
            && memcmp(mesh_players_[i].id, temp[r].id, 4) == 0) mesh_players_[i] = MeshPlayer{};
      }
      MeshPlayer& player = mesh_players_[seat];
      player = MeshPlayer{};
      player.used = true;
      memcpy(player.id, temp[r].id, 4);
      player.team = temp[r].team;
      player.revision = revision;
      player.tank.health = 3;
      player.tank.power = 65;
      safeName(player.name, sizeof player.name, temp[r].name);
      if (seat + 1 > mesh_player_count_) mesh_player_count_ = seat + 1;
      if (self && memcmp(player.id, self, 4) == 0) {
        local_seat_ = seat;
        player.has_pub = true;
        memcpy(player.pub, self, 32);
        if (timer_ && view_ == VIEW_LOBBY) lv_timer_pause(timer_);
      }
    }
    roster_revision_ = revision;
    refreshLobby();
    if (pending_begin_) startMeshMatch(pending_begin_count_);
    return;
  }

  if (command == 'P' && frame.count == 8 && !is_host_ && authorIsHost(author_id, author_id_len)
      && !match_started_) {
    uint32_t revision = 0, seat_u = 0, team = 0;
    uint8_t id[4];
    const int cap = mode_ == MODE_DM ? kDmMaxPlayers : kMaxPlayers;
    if (!parseUnsigned(frame.field[3], revision) || !parseUnsigned(frame.field[4], seat_u)
        || seat_u >= (uint32_t)cap || !hexToId(frame.field[5], id)
        || !parseUnsigned(frame.field[6], team) || team > 1) return;
    const int seat = (int)seat_u;
    if (revision < roster_revision_
        || (mesh_players_[seat].used && revision < mesh_players_[seat].revision)) return;
    for (int i = 0; i < mesh_player_count_; ++i) {
      if (i != seat && mesh_players_[i].used && memcmp(mesh_players_[i].id, id, 4) == 0)
        mesh_players_[i] = MeshPlayer{};
    }
    MeshPlayer& player = mesh_players_[seat];
    player.used = true;
    memcpy(player.id, id, 4);
    player.team = (uint8_t)team;
    player.revision = revision;
    player.tank.health = 3;
    player.tank.power = 65;
    safeName(player.name, sizeof player.name, frame.field[7]);
    if (seat + 1 > mesh_player_count_) mesh_player_count_ = seat + 1;
    if (revision > roster_revision_) roster_revision_ = revision;
    const uint8_t* self = the_mesh.getSelfPubKey();
    if (self && memcmp(id, self, 4) == 0) {
      local_seat_ = seat;
      player.has_pub = true;
      memcpy(player.pub, self, 32);
      if (timer_ && view_ == VIEW_LOBBY) lv_timer_pause(timer_);
    }
    refreshLobby();
    if (pending_begin_) startMeshMatch(pending_begin_count_);
    return;
  }

  if (command == 'C' && frame.count == 6 && !is_host_ && authorIsHost(author_id, author_id_len)
      && !match_started_) {
    uint32_t revision = 0, enabled = 0, count = 0;
    const int cap = mode_ == MODE_DM ? kDmMaxPlayers : kMaxPlayers;
    if (!parseUnsigned(frame.field[3], revision) || !parseUnsigned(frame.field[4], enabled)
        || enabled > 1 || !parseUnsigned(frame.field[5], count)
        || count < 1 || count > (uint32_t)cap) return;
    if (revision < roster_revision_) return;
    const bool shrank = count < (uint32_t)mesh_player_count_;
    if (shrank) local_seat_ = -1;
    for (int i = (int)count; i < kMaxPlayers; ++i) mesh_players_[i] = MeshPlayer{};
    mesh_player_count_ = (int)count;
    if (shrank) {
      const uint8_t* self = the_mesh.getSelfPubKey();
      for (int i = 0; self && i < mesh_player_count_; ++i) {
        if (mesh_players_[i].used && memcmp(mesh_players_[i].id, self, 4) == 0) {
          local_seat_ = i;
          break;
        }
      }
    }
    roster_revision_ = revision;
    teams_enabled_ = enabled != 0;
    if (local_seat_ < 0 && timer_) {
      lv_timer_set_period(timer_, 250);
      lv_timer_resume(timer_);
      mesh_retry_at_ = millis() + 10000;
    }
    refreshLobby();
    return;
  }

  if (command == 'B' && frame.count == 9 && !is_host_ && authorIsHost(author_id, author_id_len)
      && !match_started_) {
    uint32_t seed = 0, count = 0, first = 0, teams = 0, team_bits = 0;
    int start_wind = 0;
    if (!parseUnsigned(frame.field[3], seed, 16) || seed == 0
        || !parseUnsigned(frame.field[4], count) || count < 2 || count > kMaxPlayers
        || !parseUnsigned(frame.field[5], first) || first >= count
        || !parseSigned(frame.field[6], start_wind) || start_wind < -5 || start_wind > 5
        || !parseUnsigned(frame.field[7], teams) || teams > 1
        || !parseUnsigned(frame.field[8], team_bits, 16)
        || team_bits >= (1u << count)) return;
    terrain_seed_ = seed;
    turn_seat_ = (int)first;
    wind_ = start_wind;
    teams_enabled_ = teams != 0;
    for (uint32_t i = 0; i < count; ++i)
      mesh_players_[i].team = (uint8_t)((team_bits >> i) & 1u);
    startMeshMatch((uint8_t)count);
    return;
  }

  if (command == 'T' && frame.count == 4 && is_host_ && !match_started_) {
    uint32_t team = 0;
    const int seat = findMeshPlayer(author_id);
    if (seat < 0 || !parseUnsigned(frame.field[3], team) || team > 1) return;
    mesh_players_[seat].team = (uint8_t)team;
    mesh_players_[seat].revision = ++roster_revision_;
    sendPlayerRecord(seat);
    refreshLobby();
    return;
  }

  if (command == 'Q' && frame.count == 7 && !is_host_ && match_started_
      && authorIsHost(author_id, author_id_len)) {
    uint32_t seat = 0, turn = 0;
    int next = 0, next_wind = 0;
    if (!parseUnsigned(frame.field[3], seat) || seat >= (uint32_t)mesh_player_count_
        || !parseSigned(frame.field[4], next) || next < -1 || next >= mesh_player_count_
        || !parseSigned(frame.field[5], next_wind) || next_wind < -5 || next_wind > 5
        || !parseUnsigned(frame.field[6], turn) || turn > UINT16_MAX
        || (turn != mesh_turn_ && turn != (uint32_t)mesh_turn_ + 1u)) return;
    mesh_players_[seat].tank.health = 0;
    if (turn == (uint32_t)mesh_turn_ + 1u) {
      mesh_turn_ = (uint16_t)turn;
      turn_seat_ = next;
      wind_ = next_wind;
      state_ = (turn_seat_ == local_seat_ && local_seat_ >= 0
                && mesh_players_[local_seat_].tank.health > 0) ? PLAYER_AIM : MESH_WAIT;
      if (timer_) lv_timer_pause(timer_);
      if (state_ == PLAYER_AIM) touchArtilleryTurnAlert();
    }
    if (state_ != PLAYER_FLIGHT && state_ != EXPLODING) showMeshWinnerIfDone();
    updateHud();
    render();
    return;
  }

  if (command == 'Q' && frame.count == 3) {
    if (!is_host_ && authorIsHost(author_id, author_id_len)) {
      match_id_ = 0;  // suppress showModeMenu's leave frame
      showModeMenu();
      if (overlay_title_) lv_label_set_text(overlay_title_, "MATCH CLOSED BY HOST");
      return;
    }
    if (is_host_ && match_started_) {
      const int seat = findMeshPlayer(author_id);
      if (seat <= 0 || seat >= mesh_player_count_) return;
      mesh_players_[seat].tank.health = 0;
      if (seat == turn_seat_ && state_ != PLAYER_FLIGHT && state_ != EXPLODING) {
        turn_seat_ = nextAliveSeat(seat);
        wind_ = (int)random(-5, 6);
        ++mesh_turn_;
        state_ = (turn_seat_ == local_seat_ && local_seat_ >= 0
                  && mesh_players_[local_seat_].tank.health > 0) ? PLAYER_AIM : MESH_WAIT;
        if (timer_) lv_timer_pause(timer_);
        if (state_ == PLAYER_AIM) touchArtilleryTurnAlert();
      }
      char leave[80];
      snprintf(leave, sizeof leave, "~SG1|Q|%08lX|%d|%d|%d|%u",
               (unsigned long)match_id_, seat, turn_seat_, wind_, (unsigned)mesh_turn_);
      broadcast(leave);
      if (state_ != PLAYER_FLIGHT && state_ != EXPLODING) showMeshWinnerIfDone();
      updateHud();
      render();
    } else if (is_host_ && !match_started_) {
      const int seat = findMeshPlayer(author_id);
      if (seat <= 0 || seat >= mesh_player_count_) return;
      for (int i = seat; i + 1 < mesh_player_count_; ++i) mesh_players_[i] = mesh_players_[i + 1];
      mesh_players_[mesh_player_count_ - 1] = MeshPlayer{};
      --mesh_player_count_;
      ++roster_revision_;
      char config[72];
      snprintf(config, sizeof config, "~SG1|C|%08lX|%lu|%u|%d",
               (unsigned long)match_id_, (unsigned long)roster_revision_,
               teams_enabled_ ? 1u : 0u, mesh_player_count_);
      broadcast(config);
      sendRosterSnapshot();
      refreshLobby();
    }
    return;
  }

  if (command == 'F' && frame.count == 6 && is_host_ && match_started_) {
    uint32_t turn = 0, angle = 0, power = 0;
    const int seat = findMeshPlayer(author_id);
    if (seat != turn_seat_ || !parseUnsigned(frame.field[3], turn) || turn != mesh_turn_
        || !parseUnsigned(frame.field[4], angle) || angle < 5 || angle > 175
        || !parseUnsigned(frame.field[5], power) || power < 20 || power > 100) return;
    broadcastShot(seat, (int)angle, (int)power);
    return;
  }

  if (command == 'S' && frame.count == 8 && !is_host_ && match_started_
      && authorIsHost(author_id, author_id_len)) {
    uint32_t turn = 0, seat = 0, angle = 0, power = 0;
    int shot_wind = 0;
    if (!parseUnsigned(frame.field[3], turn) || turn != mesh_turn_
        || !parseUnsigned(frame.field[4], seat) || seat != (uint32_t)turn_seat_
        || !parseUnsigned(frame.field[5], angle) || angle < 5 || angle > 175
        || !parseUnsigned(frame.field[6], power) || power < 20 || power > 100
        || !parseSigned(frame.field[7], shot_wind) || shot_wind < -5 || shot_wind > 5) return;
    if (state_ == PLAYER_FLIGHT || state_ == EXPLODING) return;
    launchMeshShot((int)seat, (int)angle, (int)power, shot_wind);
    return;
  }

  if (command == 'X' && frame.count == 9 && !is_host_ && match_started_
      && authorIsHost(author_id, author_id_len)) {
    uint32_t turn = 0;
    int nx = 0, ny = 0, next = 0, next_wind = 0;
    if (!parseUnsigned(frame.field[3], turn) || turn > UINT16_MAX
        || !parseSigned(frame.field[4], nx) || nx < -1 || nx >= kWorldWidth
        || !parseSigned(frame.field[5], ny) || ny < -1 || ny >= kWorldHeight
        || !parseSigned(frame.field[7], next) || next < -1 || next >= mesh_player_count_
        || !parseSigned(frame.field[8], next_wind) || next_wind < -5 || next_wind > 5) return;
    applyMeshResult((uint16_t)turn, nx, ny, frame.field[6], next, next_wind);
    return;
  }

  if (command == 'R' && frame.count == 4 && is_host_ && match_started_) {
    if (strcmp(frame.field[3], "L") == 0) {
      if (!last_begin_frame_[0]) return;
      if (mode_ == MODE_DM && author_id_len == 32) {
        sendRosterSnapshot(author_id);
        sendToPub(author_id, last_begin_frame_);
      } else if (mode_ == MODE_ROOM) {
        sendRosterSnapshot();
        sendTransport(last_begin_frame_);
      }
      return;
    }
    uint32_t turn = 0;
    if (!parseUnsigned(frame.field[3], turn) || turn != last_result_turn_
        || !last_result_frame_[0]) return;
    if (mode_ == MODE_DM && author_id_len == 32) sendToPub(author_id, last_result_frame_);
    else if (mode_ == MODE_ROOM) sendTransport(last_result_frame_);
  }
}

void ArtilleryGame::pixel(int x, int y, lv_color_t color) {
  if ((unsigned)x < (unsigned)width_ && (unsigned)y < (unsigned)height_)
    buf_[y * width_ + x] = color;
}

void ArtilleryGame::fillRect(int x, int y, int w, int h, lv_color_t color) {
  int x0 = x < 0 ? 0 : x;
  int y0 = y < 0 ? 0 : y;
  int x1 = x + w; if (x1 > width_) x1 = width_;
  int y1 = y + h; if (y1 > height_) y1 = height_;
  for (int py = y0; py < y1; ++py)
    for (int px = x0; px < x1; ++px) buf_[py * width_ + px] = color;
}

void ArtilleryGame::line(int x0, int y0, int x1, int y1, lv_color_t color) {
  int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  for (;;) {
    pixel(x0, y0, color);
    if (x0 == x1 && y0 == y1) break;
    const int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

void ArtilleryGame::fillCircle(int cx, int cy, int radius, lv_color_t color) {
  const int r2 = radius * radius;
  for (int y = -radius; y <= radius; ++y)
    for (int x = -radius; x <= radius; ++x)
      if (x * x + y * y <= r2) pixel(cx + x, cy + y, color);
}

void ArtilleryGame::drawTank(const Tank& tank, lv_color_t body) {
  if (tank.health <= 0) return;
  const int y = tankY(tank);
  const lv_color_t dark = lv_color_hex(0x152129);
  fillRect(tank.x - 8, y - 5, 17, 5, dark);
  fillRect(tank.x - 6, y - 9, 13, 5, body);
  fillCircle(tank.x, y - 9, 3, body);
  const float angle = (float)tank.angle * kPi / 180.0f;
  const int muzzle_x = tank.x + (int)(cosf(angle) * 12.0f);
  const int muzzle_y = y - 9 - (int)(sinf(angle) * 12.0f);
  line(tank.x, y - 9, muzzle_x, muzzle_y, body);
  for (int i = 0; i < tank.health; ++i)
    fillRect(tank.x - 7 + i * 5, y - 19, 3, 2, body);
}

void ArtilleryGame::drawAimGuide() {
  if (!isLocalTurn()) return;
  const Tank& tank = mode_ == MODE_LOCAL ? player_ : mesh_players_[local_seat_].tank;
  const float angle = (float)tank.angle * kPi / 180.0f;
  const float scale = sqrtf((float)width_ / 320.0f);
  float x = (float)tank.x + cosf(angle) * 12.0f;
  float y = (float)tankY(tank) - 6.0f - sinf(angle) * 12.0f;
  float vx = cosf(angle) * (float)tank.power * kShotScale * scale;
  float vy = -sinf(angle) * (float)tank.power * kShotScale * scale;
  const lv_color_t guide = lv_color_hex(0x91C9D6);
  for (int step = 0; step < 20; ++step) {
    vx += (float)wind_ * kWindAccel;
    vy += kGravity;
    x += vx;
    y += vy;
    if ((step & 2) == 0) fillCircle((int)x, (int)y, 1, guide);
    if (x < 0 || x >= width_ || y >= height_) break;
  }
}

void ArtilleryGame::render() {
  if (!canvas_ || !buf_) return;
  const uint8_t r0 = (kSkyTop >> 16) & 0xFF, g0 = (kSkyTop >> 8) & 0xFF, b0 = kSkyTop & 0xFF;
  const uint8_t r1 = (kSkyBottom >> 16) & 0xFF, g1 = (kSkyBottom >> 8) & 0xFF, b1 = kSkyBottom & 0xFF;
  for (int y = 0; y < height_; ++y) {
    const int den = height_ > 1 ? height_ - 1 : 1;
    const lv_color_t sky = lv_color_make(
        (uint8_t)(r0 + ((int)r1 - r0) * y / den),
        (uint8_t)(g0 + ((int)g1 - g0) * y / den),
        (uint8_t)(b0 + ((int)b1 - b0) * y / den));
    for (int x = 0; x < width_; ++x) buf_[y * width_ + x] = sky;
  }
  fillCircle(width_ * 72 / 100, height_ * 20 / 100, 8, lv_color_hex(0xF2C56B));
  const lv_color_t soil = lv_color_hex(kSoil);
  const lv_color_t deep = lv_color_hex(kSoilDeep);
  const lv_color_t grass = lv_color_hex(kGrass);
  for (int x = 0; x < width_; ++x) {
    int top = terrain_[x];
    if (top < 0) top = 0;
    if (top >= height_) top = height_ - 1;
    pixel(x, top, grass);
    for (int y = top + 1; y < height_; ++y)
      buf_[y * width_ + x] = (((x >> 2) + (y >> 2)) & 3) ? soil : deep;
  }

  drawAimGuide();
  if (mode_ == MODE_LOCAL || !match_started_) {
    drawTank(player_, lv_color_hex(kPlayer));
    drawTank(cpu_, lv_color_hex(kCpu));
  } else {
    static const uint32_t palette[kMaxPlayers] = {
      0x29C7B8, 0xE45C4F, 0xE8B44C, 0x8D72E1, 0x5BC56D,
      0xE77EB4, 0x65A9E8, 0xD67A42, 0xB9C44D, 0x9C8B7A
    };
    for (int i = 0; i < mesh_player_count_; ++i) {
      if (!mesh_players_[i].used) continue;
      const uint32_t color = teams_enabled_
          ? ((mesh_players_[i].team & 1) ? kCpu : kPlayer)
          : palette[i % kMaxPlayers];
      drawTank(mesh_players_[i].tank, lv_color_hex(color));
    }
  }

  if (state_ == PLAYER_FLIGHT || state_ == CPU_FLIGHT)
    fillCircle((int)shell_x_, (int)shell_y_, 2, lv_color_hex(kShell));
  if (state_ == EXPLODING && explosion_x_ >= 0) {
    uint32_t elapsed = millis() - state_since_ms_;
    int radius = (int)(elapsed * kBlastRadius / 240u);
    if (radius > kBlastRadius) radius = kBlastRadius;
    if (radius < 2) radius = 2;
    fillCircle(explosion_x_, explosion_y_, radius, lv_color_hex(kBlast));
    fillCircle(explosion_x_, explosion_y_, radius / 2, lv_color_hex(0xFFF2A6));
  }
  lv_obj_invalidate(canvas_);
}

void ArtilleryGame::timerCb(lv_timer_t* timer) {
  auto* game = static_cast<ArtilleryGame*>(timer->user_data);
  if (game) game->tick();
}

static ArtilleryGame* artilleryFromEvent(lv_event_t* e) {
  return static_cast<ArtilleryGame*>(lv_event_get_user_data(e));
}

void ArtilleryGame::angleMinusCb(lv_event_t* e) { if (auto* g = artilleryFromEvent(e)) g->adjustAngle(-1); }
void ArtilleryGame::anglePlusCb (lv_event_t* e) { if (auto* g = artilleryFromEvent(e)) g->adjustAngle(+1); }
void ArtilleryGame::powerMinusCb(lv_event_t* e) { if (auto* g = artilleryFromEvent(e)) g->adjustPower(-2); }
void ArtilleryGame::powerPlusCb (lv_event_t* e) { if (auto* g = artilleryFromEvent(e)) g->adjustPower(+2); }
void ArtilleryGame::fireCb      (lv_event_t* e) { if (auto* g = artilleryFromEvent(e)) g->action(); }
void ArtilleryGame::replayCb    (lv_event_t* e) { if (auto* g = artilleryFromEvent(e)) g->replayGame(); }
void ArtilleryGame::localModeCb (lv_event_t* e) { if (auto* g = artilleryFromEvent(e)) g->startLocalGame(); }
void ArtilleryGame::dmModeCb    (lv_event_t* e) { if (auto* g = artilleryFromEvent(e)) g->showContactPicker(false); }
void ArtilleryGame::roomModeCb  (lv_event_t* e) { if (auto* g = artilleryFromEvent(e)) g->showContactPicker(true); }

void ArtilleryGame::pickerRowCb(lv_event_t* e) {
  auto* row = static_cast<PickerRow*>(lv_event_get_user_data(e));
  if (row && row->game && row->mesh_index >= 0) row->game->togglePickerRow(row->mesh_index);
}
void ArtilleryGame::pickerPrevCb(lv_event_t* e) {
  if (auto* g = artilleryFromEvent(e)) { if (g->picker_page_ > 0) --g->picker_page_; g->refreshPicker(); }
}
void ArtilleryGame::pickerNextCb(lv_event_t* e) {
  if (auto* g = artilleryFromEvent(e)) { ++g->picker_page_; g->refreshPicker(); }
}
void ArtilleryGame::pickerStartCb(lv_event_t* e) { if (auto* g = artilleryFromEvent(e)) g->beginHostLobby(); }
void ArtilleryGame::overlayBackCb(lv_event_t* e) { if (auto* g = artilleryFromEvent(e)) g->showModeMenu(); }
void ArtilleryGame::inviteAcceptCb(lv_event_t* e) { if (auto* g = artilleryFromEvent(e)) g->acceptInvite(); }
void ArtilleryGame::inviteDeclineCb(lv_event_t* e) { if (auto* g = artilleryFromEvent(e)) g->declineInvite(); }
void ArtilleryGame::lobbyPrimaryCb(lv_event_t* e) { if (auto* g = artilleryFromEvent(e)) g->startHostedMatch(); }
void ArtilleryGame::lobbyTeamCb(lv_event_t* e) { if (auto* g = artilleryFromEvent(e)) g->toggleLocalTeam(); }
void ArtilleryGame::lobbyModeCb(lv_event_t* e) { if (auto* g = artilleryFromEvent(e)) g->toggleTeams(); }

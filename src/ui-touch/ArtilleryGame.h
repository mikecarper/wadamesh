// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ArtilleryPhysics.h"

#include <lvgl.h>
#include <stddef.h>
#include <stdint.h>

struct ContactInfo;

// Compact Scorched-Earth-style artillery game for the touch UI.  Local play
// uses a CPU opponent. Mesh play uses encrypted MeshCore private messages or a
// signed room, with a host-authoritative roster and turn order. Every peer
// calculates deterministic shot results in the same canonical world.
class ArtilleryGame {
public:
  static void launch();
  static bool isOpen();
  static void steer(int dx, int dy);   // aim/power, or picker navigation
  static void setFire(bool down);      // edge-triggered trackball/encoder button
  static bool keyChar(char c);         // true when the open game consumed the key
  static void dismiss();               // AppPage back hook; async-safe

  // Called before an incoming private payload is added to chat. Returns true
  // only for a valid Artillery protocol frame, keeping game traffic out of the
  // chat timeline and companion message history.
  static bool handleMeshMessage(const ContactInfo& transport,
                                const uint8_t* author_id, size_t author_id_len,
                                bool via_room, const char* text);
  static bool hasPendingInvite();

private:
  static constexpr int kMaxWidth = 800;
  static constexpr int kBlastRadius = artillery::kBlastRadius;
  static constexpr int kMaxPlayers = artillery::kMaxPlayers;
  static constexpr int kDmMaxPlayers = 4;
  static constexpr int kMaxName = 15;
  static constexpr int kPickerRows = 4;
  static constexpr int kWorldWidth = artillery::kWorldWidth;
  static constexpr int kWorldHeight = artillery::kWorldHeight;

  using Tank = artillery::Tank;

  struct MeshPlayer {
    Tank tank;
    bool used = false;
    bool has_pub = false;
    uint8_t id[4] = {};
    uint8_t pub[32] = {};
    uint8_t team = 0;
    uint32_t revision = 0;
    char name[kMaxName + 1] = {};
  };

  struct InviteTarget {
    bool used = false;
    uint8_t pub[32] = {};
    char name[kMaxName + 1] = {};
  };

  struct PickerRow {
    class ArtilleryGame* game = nullptr;
    int mesh_index = -1;
    lv_obj_t* button = nullptr;
    lv_obj_t* label = nullptr;
  };

  enum MeshEventType : uint8_t {
    MESH_EVENT_SHOT = 1,
    MESH_EVENT_FORFEIT = 2,
  };

  struct MeshEventRecord {
    uint32_t pre_hash = 0;
    uint16_t sequence = 0;
    uint16_t turn = 0;
    uint8_t type = 0;
    uint8_t seat = 0;
    uint8_t angle = 0;
    uint8_t power = 0;
  };

  enum State : uint8_t {
    PLAYER_AIM,
    PLAYER_FLIGHT,
    CPU_THINK,
    CPU_FLIGHT,
    MESH_WAIT,
    SHOT_PENDING,
    EXPLODING,
    GAME_OVER,
  };

  enum Mode : uint8_t {
    MODE_NONE,
    MODE_LOCAL,
    MODE_DM,
    MODE_ROOM,
  };

  enum View : uint8_t {
    VIEW_GAME,
    VIEW_MODE,
    VIEW_PICKER,
    VIEW_INVITE,
    VIEW_LOBBY,
  };

  ~ArtilleryGame();

  lv_obj_t* root_ = nullptr;
  lv_obj_t* canvas_ = nullptr;
  lv_color_t* buf_ = nullptr;
  lv_obj_t* angle_label_ = nullptr;
  lv_obj_t* power_label_ = nullptr;
  lv_obj_t* info_label_ = nullptr;
  lv_obj_t* turn_label_ = nullptr;
  lv_obj_t* angle_minus_ = nullptr;
  lv_obj_t* angle_plus_ = nullptr;
  lv_obj_t* power_minus_ = nullptr;
  lv_obj_t* power_plus_ = nullptr;
  lv_obj_t* fire_btn_ = nullptr;
  lv_obj_t* replay_btn_ = nullptr;
  lv_obj_t* replay_label_ = nullptr;
  lv_obj_t* overlay_ = nullptr;
  lv_obj_t* overlay_title_ = nullptr;
  lv_obj_t* overlay_body_ = nullptr;
  lv_obj_t* overlay_primary_ = nullptr;
  lv_obj_t* overlay_secondary_ = nullptr;
  lv_obj_t* mode_buttons_[3] = {};
  PickerRow picker_rows_[kPickerRows] = {};
  lv_timer_t* timer_ = nullptr;

  int width_ = 0;
  int height_ = 0;
  int canvas_y_ = 0;
  int16_t terrain_[kWorldWidth] = {};
  Tank player_;
  Tank cpu_;
  State state_ = PLAYER_AIM;
  Mode mode_ = MODE_NONE;
  View view_ = VIEW_MODE;
  bool shot_by_cpu_ = false;
  bool fire_down_ = false;
  int wind_ = 0;                    // negative = left, positive = right
  uint32_t terrain_seed_ = 1;

  artillery::Shot shell_;
  int explosion_x_ = -1;
  int explosion_y_ = -1;
  uint32_t state_since_ms_ = 0;

  // Mesh lobby and match state. The host validates and orders player actions;
  // every peer derives health, terrain damage, wind, and turns from that log.
  MeshPlayer mesh_players_[kMaxPlayers] = {};
  InviteTarget invite_targets_[kDmMaxPlayers - 1] = {};
  int mesh_player_count_ = 0;
  int invite_target_count_ = 0;
  int local_seat_ = -1;
  uint8_t initial_turn_seat_ = 0;
  int turn_seat_ = 0;
  int shot_seat_ = -1;
  int pending_next_seat_ = -1;
  int picker_page_ = 0;
  int picker_cursor_ = 0;
  int picker_total_ = 0;
  bool picker_rooms_ = false;
  bool is_host_ = false;
  bool teams_enabled_ = false;
  bool match_started_ = false;
  bool pending_begin_ = false;
  uint8_t pending_begin_count_ = 0;
  uint8_t pending_begin_first_ = 0;
  uint32_t pending_begin_hash_ = 0;
  bool pending_begin_hash_valid_ = false;
  bool resume_after_begin_ = false;
  uint16_t resume_event_count_ = 0;
  uint16_t mesh_turn_ = 0;
  uint16_t mesh_event_sequence_ = 0;
  uint16_t mesh_event_count_ = 0;
  uint16_t recovery_target_event_ = 0;
  uint16_t pending_forfeit_mask_ = 0;
  bool mesh_recovering_ = false;
  bool recovery_reset_attempted_ = false;
  uint32_t match_id_ = 0;
  uint32_t roster_revision_ = 0;
  uint32_t mesh_retry_at_ = 0;
  uint8_t host_id_[4] = {};
  uint8_t transport_pub_[32] = {};  // host contact in DM; room server in room mode
  char transport_name_[kMaxName + 1] = {};
  char last_begin_frame_[128] = {};
  static constexpr int kMaxMeshEvents = 256;
  MeshEventRecord mesh_events_[kMaxMeshEvents] = {};

  bool open();
  void close();
  void startLocalGame();
  void replayGame();
  void buildControls(int local_y);
  lv_obj_t* makeButton(int x, int y, int w, const char* text, lv_event_cb_t cb);
  void generateTerrain();
  void flattenForTank(int x);
  int tankY(const Tank& tank) const;
  void adjustAngle(int delta);
  void adjustPower(int delta);
  void action();
  void launchShot(bool cpu_shot);
  void launchMeshShot(int seat, int angle, int power, int shot_wind);
  void chooseCpuShot();
  int simulateCpuImpact(int angle, int power) const;
  void tick();
  void advanceShell();
  void beginExplosion(int x, int y);
  void finishMiss();
  void finishExplosion();
  void damageTank(Tank& tank, int x, int y);
  void carveCrater(int x, int y);
  int worldToScreenX(int x) const;
  int worldToScreenY(int y) const;
  void showReplay(const char* text);
  void hideReplay();
  void updateHud();
  void render();

  // Menus/lobby.
  lv_obj_t* createOverlay(const char* title);
  lv_obj_t* overlayButton(const char* text, int x, int y, int w, lv_event_cb_t cb);
  void clearOverlay();
  void showModeMenu();
  void showContactPicker(bool rooms);
  void refreshPicker();
  void movePicker(int delta);
  void togglePickerRow(int mesh_index);
  void beginHostLobby();
  void showInvitePrompt();
  void acceptInvite();
  void declineInvite();
  void showLobby();
  void refreshLobby();
  void toggleTeams();
  void toggleLocalTeam();
  void startHostedMatch();

  // Mesh protocol and host authority.
  void receiveProtocol(const ContactInfo& transport,
                       const uint8_t* author_id, size_t author_id_len,
                       bool via_room, const char* text);
  bool sendToPub(const uint8_t pub[32], const char* text);
  bool sendTransport(const char* text);
  void broadcast(const char* text);
  void sendInviteFrames();
  void sendJoin();
  void sendPlayerRecord(int seat, const uint8_t* only_pub = nullptr);
  void sendRosterSnapshot(const uint8_t* only_pub = nullptr);
  void sendTeamRequest(uint8_t team);
  void sendShotRequest();
  void broadcastShot(int seat, int angle, int power);
  void resolveMeshShot(int x, int y, bool missed);
  uint32_t meshStateHash() const;
  bool storeMeshEvent(const MeshEventRecord& event);
  bool formatMeshEvent(const MeshEventRecord& event, char* frame, size_t cap) const;
  bool applyMeshEventInstant(const MeshEventRecord& event);
  bool rebuildMeshState();
  void resetMeshState(bool clear_history);
  void beginMeshRecovery(uint16_t target_event, bool reset);
  void requestMeshEvent(uint16_t sequence);
  void requestMeshRosterSnapshot();
  void restartMeshRecoveryFromRoster(uint16_t event_count);
  void continueMeshRecovery();
  void sendMeshCheckpoint(const uint8_t* recipient, size_t recipient_len);
  void resendMeshEvent(uint16_t sequence, const uint8_t* recipient, size_t recipient_len);
  void applyMeshForfeit(int seat);
  void broadcastMeshForfeit(int seat);
  void processPendingForfeits();
  void settleMeshState(bool alert_turn);
  void startMeshMatch(uint8_t expected_players, uint8_t first_seat = 0);
  void configureMeshTanks();
  int findMeshPlayer(const uint8_t id[4]) const;
  int addMeshPlayer(const uint8_t id[4], const uint8_t* full_pub,
                    const char* name, uint8_t team, uint32_t revision);
  int nextAliveSeat(int after) const;
  int aliveTeamMask() const;
  bool showMeshWinnerIfDone();
  bool authorIsHost(const uint8_t* author_id, size_t author_id_len) const;
  bool senderAllowedToJoin(const uint8_t* author_id, size_t author_id_len) const;
  bool isLocalTurn() const;
  bool sendProtocolTo(const ContactInfo& recipient, const char* text);

  void pixel(int x, int y, lv_color_t color);
  void fillRect(int x, int y, int w, int h, lv_color_t color);
  void line(int x0, int y0, int x1, int y1, lv_color_t color);
  void fillCircle(int cx, int cy, int radius, lv_color_t color);
  void drawTank(const Tank& tank, lv_color_t body);
  void drawAimGuide();

  static ArtilleryGame* s_active;
  static void timerCb(lv_timer_t* timer);
  static void destroyAsync(void* p);
  static void angleMinusCb(lv_event_t* e);
  static void anglePlusCb(lv_event_t* e);
  static void powerMinusCb(lv_event_t* e);
  static void powerPlusCb(lv_event_t* e);
  static void fireCb(lv_event_t* e);
  static void replayCb(lv_event_t* e);
  static void localModeCb(lv_event_t* e);
  static void dmModeCb(lv_event_t* e);
  static void roomModeCb(lv_event_t* e);
  static void pickerRowCb(lv_event_t* e);
  static void pickerPrevCb(lv_event_t* e);
  static void pickerNextCb(lv_event_t* e);
  static void pickerStartCb(lv_event_t* e);
  static void overlayBackCb(lv_event_t* e);
  static void inviteAcceptCb(lv_event_t* e);
  static void inviteDeclineCb(lv_event_t* e);
  static void lobbyPrimaryCb(lv_event_t* e);
  static void lobbyTeamCb(lv_event_t* e);
  static void lobbyModeCb(lv_event_t* e);
};

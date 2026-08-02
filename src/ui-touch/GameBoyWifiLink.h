// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>

#include "GameBoyLinkProtocol.h"

class GameBoyWifiLink {
public:
  enum class Role : uint8_t { Off, Host, Join };
  enum class State : uint8_t { Off, NoWifi, Searching, Connected, Error };

  bool start(Role role, uint32_t game_id);
  void stop();
  void service(uint32_t now_ms);

  // Called when the emulated game starts a serial byte. Returning true tells
  // gnuboy that the WiFi link owns completion of this transfer.
  bool beginTransfer(uint8_t outgoing, bool internal_clock);
  bool takeCompleted(uint8_t& incoming);
  void cancelTransfers();

  Role role() const { return _role; }
  State state() const { return _state; }
  bool active() const { return _role != Role::Off; }
  bool connected() const { return _state == State::Connected; }
  bool sameGame() const { return _same_game; }
  uint16_t lastRttMs() const { return _last_rtt_ms; }
  uint32_t revision() const { return _revision; }
  void statusText(char* out, size_t cap) const;

private:
  struct LocalTransfer {
    bool pending = false;
    bool internal = false;
    uint8_t value = 0xFF;
    uint32_t sequence = 0;
    uint32_t started_ms = 0;
    uint32_t last_send_ms = 0;
  };

  struct RemoteTransfer {
    bool pending = false;
    uint8_t value = 0xFF;
    uint32_t sequence = 0;
    uint32_t started_ms = 0;
  };

  static constexpr uint16_t kPort = 45971;
  static constexpr uint32_t kAnnounceMs = 400;
  static constexpr uint32_t kKeepAliveMs = 500;
  static constexpr uint32_t kPeerTimeoutMs = 3500;
  static constexpr uint32_t kHandshakeTimeoutMs = 1800;
  static constexpr uint32_t kTransferRetryMs = 24;
  static constexpr uint32_t kTransferTimeoutMs = 1500;

  bool wifiReady() const;
  bool openSocket();
  void closeSocket();
  void setState(State state);
  void clearPeer(bool complete_local);
  void queueCompletion(uint8_t value);
  void pairTransfers(uint32_t now_ms);
  void serviceTransferTimeouts(uint32_t now_ms);
  void receivePackets(uint32_t now_ms);
  void handlePacket(const gameboy_link::Packet& packet, const IPAddress& source,
                    uint32_t now_ms);
  bool sendPacket(gameboy_link::PacketType type, const IPAddress& destination,
                  uint32_t target, uint32_t sequence = 0, uint8_t value = 0,
                  uint8_t flags = 0);
  void sendAnnouncement(uint32_t now_ms);
  void sendKeepAlive(uint32_t now_ms);
  bool isPeerPacket(const gameboy_link::Packet& packet,
                    const IPAddress& source) const;

  WiFiUDP _udp;
  bool _socket_open = false;
  IPAddress _broadcast_ip;
  IPAddress _peer_ip;
  Role _role = Role::Off;
  State _state = State::Off;
  uint32_t _game_id = 0;
  uint32_t _instance = 0;
  uint32_t _session = 0;
  uint32_t _peer_instance = 0;
  uint32_t _next_sequence = 0;
  uint32_t _last_announce_ms = 0;
  uint32_t _last_keepalive_ms = 0;
  uint32_t _last_peer_ms = 0;
  uint32_t _revision = 0;
  bool _peer_confirmed = false;
  bool _same_game = true;

  LocalTransfer _local;
  RemoteTransfer _remote;
  bool _completion_ready = false;
  uint8_t _completion_value = 0xFF;
  bool _last_reply_valid = false;
  uint32_t _last_reply_sequence = 0;
  uint8_t _last_reply_value = 0xFF;
  uint16_t _last_rtt_ms = 0;

  bool _sleep_saved = false;
  wifi_ps_type_t _saved_sleep = WIFI_PS_MIN_MODEM;
};

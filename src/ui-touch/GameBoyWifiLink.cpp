// SPDX-License-Identifier: GPL-3.0-or-later
#include "GameBoyWifiLink.h"

#include <esp_system.h>
#include <stdio.h>

using gameboy_link::Packet;
using gameboy_link::PacketType;

namespace {

uint32_t nonzeroRandom() {
  uint32_t value = esp_random();
  return value ? value : 1u;
}

bool sameIp(const IPAddress& left, const IPAddress& right) {
  return (uint32_t)left == (uint32_t)right;
}

}  // namespace

bool GameBoyWifiLink::wifiReady() const {
  return WiFi.status() == WL_CONNECTED && (uint32_t)WiFi.localIP() != 0;
}

void GameBoyWifiLink::setState(State state) {
  if (_state == state) return;
  _state = state;
  ++_revision;
}

bool GameBoyWifiLink::openSocket() {
  if (_socket_open) return true;
  if (!wifiReady() || _udp.begin(kPort) != 1) return false;

  const IPAddress local = WiFi.localIP();
  const IPAddress mask = WiFi.subnetMask();
  _broadcast_ip = IPAddress((uint8_t)(local[0] | (uint8_t)~mask[0]),
                            (uint8_t)(local[1] | (uint8_t)~mask[1]),
                            (uint8_t)(local[2] | (uint8_t)~mask[2]),
                            (uint8_t)(local[3] | (uint8_t)~mask[3]));
  _socket_open = true;
  return true;
}

void GameBoyWifiLink::closeSocket() {
  if (_socket_open) _udp.stop();
  _socket_open = false;
}

bool GameBoyWifiLink::start(Role role, uint32_t game_id) {
  stop();
  if (role == Role::Off) return true;

  _role = role;
  _game_id = game_id;
  _instance = nonzeroRandom();
  _session = role == Role::Host ? nonzeroRandom() : 0;
  _next_sequence = nonzeroRandom();
  _same_game = true;
  _last_announce_ms = 0;
  _last_keepalive_ms = 0;
  _last_peer_ms = 0;

  _saved_sleep = WiFi.getSleep();
  _sleep_saved = true;
  WiFi.setSleep(false);

  if (!wifiReady()) {
    setState(State::NoWifi);
    return false;
  }
  if (!openSocket()) {
    setState(State::Error);
    return false;
  }

  setState(State::Searching);
  service(millis());
  return true;
}

void GameBoyWifiLink::stop() {
  if (_state == State::Connected && _socket_open && _peer_instance != 0)
    sendPacket(PacketType::Bye, _peer_ip, _peer_instance);

  closeSocket();
  _role = Role::Off;
  _peer_instance = 0;
  _peer_ip = IPAddress();
  _session = 0;
  _game_id = 0;
  _peer_confirmed = false;
  _same_game = true;
  _last_rtt_ms = 0;
  cancelTransfers();
  if (_sleep_saved) {
    WiFi.setSleep(_saved_sleep);
    _sleep_saved = false;
  }
  setState(State::Off);
}

void GameBoyWifiLink::cancelTransfers() {
  _local = LocalTransfer{};
  _remote = RemoteTransfer{};
  _completion_ready = false;
  _last_reply_valid = false;
}

void GameBoyWifiLink::queueCompletion(uint8_t value) {
  _completion_value = value;
  _completion_ready = true;
}

void GameBoyWifiLink::clearPeer(bool complete_local) {
  if (complete_local && _local.pending) queueCompletion(0xFF);
  _local = LocalTransfer{};
  _remote = RemoteTransfer{};
  _last_reply_valid = false;
  _peer_instance = 0;
  _peer_ip = IPAddress();
  _peer_confirmed = false;
  _same_game = true;
  _last_rtt_ms = 0;
  _last_peer_ms = 0;
  _last_keepalive_ms = 0;
  _last_announce_ms = 0;
  if (_role == Role::Host) _session = nonzeroRandom();
  else if (_role == Role::Join) _session = 0;
  if (_role != Role::Off) setState(wifiReady() ? State::Searching : State::NoWifi);
}

bool GameBoyWifiLink::sendPacket(PacketType type, const IPAddress& destination,
                                 uint32_t target, uint32_t sequence,
                                 uint8_t value, uint8_t flags) {
  if (!_socket_open) return false;
  Packet packet;
  packet.type = type;
  packet.flags = flags;
  packet.value = value;
  packet.session = _session;
  packet.sender = _instance;
  packet.target = target;
  packet.sequence = sequence;
  packet.game_id = _game_id;
  uint8_t wire[gameboy_link::kWireSize];
  gameboy_link::encode(packet, wire);
  if (_udp.beginPacket(destination, kPort) != 1) return false;
  const size_t written = _udp.write(wire, sizeof(wire));
  const int ended = _udp.endPacket();
  return written == sizeof(wire) && ended == 1;
}

void GameBoyWifiLink::sendAnnouncement(uint32_t now_ms) {
  if (!_socket_open || (uint32_t)(now_ms - _last_announce_ms) < kAnnounceMs)
    return;
  _last_announce_ms = now_ms;
  if (_role == Role::Host) {
    sendPacket(PacketType::Advertise, _broadcast_ip, 0);
  } else if (_role == Role::Join && _peer_instance == 0) {
    sendPacket(PacketType::Discover, _broadcast_ip, 0);
  } else if (_role == Role::Join && _peer_instance != 0) {
    sendPacket(PacketType::Join, _peer_ip, _peer_instance);
  }
}

void GameBoyWifiLink::sendKeepAlive(uint32_t now_ms) {
  if (_state != State::Connected || _peer_instance == 0
      || (uint32_t)(now_ms - _last_keepalive_ms) < kKeepAliveMs) return;
  _last_keepalive_ms = now_ms;
  sendPacket(PacketType::KeepAlive, _peer_ip, _peer_instance);
}

bool GameBoyWifiLink::isPeerPacket(const Packet& packet,
                                   const IPAddress& source) const {
  return _peer_instance != 0 && packet.sender == _peer_instance
      && packet.target == _instance && packet.session == _session
      && sameIp(source, _peer_ip);
}

void GameBoyWifiLink::handlePacket(const Packet& packet,
                                   const IPAddress& source,
                                   uint32_t now_ms) {
  if (packet.sender == 0 || packet.sender == _instance) return;

  if (packet.type == PacketType::Discover && _role == Role::Host
      && _peer_instance == 0) {
    sendPacket(PacketType::Advertise, source, packet.sender);
    return;
  }

  if (_role == Role::Join && _state == State::Searching
      && packet.type == PacketType::Advertise
      && (packet.target == 0 || packet.target == _instance)) {
    if (_peer_instance == 0) {
      _peer_instance = packet.sender;
      _peer_ip = source;
      _session = packet.session;
      _same_game = packet.game_id == _game_id;
    }
    if (packet.sender == _peer_instance && packet.session == _session
        && sameIp(source, _peer_ip)) {
      _last_peer_ms = now_ms;
      sendPacket(PacketType::Join, _peer_ip, _peer_instance);
    }
    return;
  }

  if (_role == Role::Host && packet.type == PacketType::Join
      && packet.target == _instance && packet.session == _session
      && (_peer_instance == 0 || (packet.sender == _peer_instance
          && sameIp(source, _peer_ip)))) {
    if (_peer_instance == 0) {
      _peer_instance = packet.sender;
      _peer_ip = source;
      _same_game = packet.game_id == _game_id;
      _peer_confirmed = false;
    }
    _last_peer_ms = now_ms;
    setState(State::Connected);
    sendPacket(PacketType::Accept, _peer_ip, _peer_instance);
    return;
  }

  if (_role == Role::Join && packet.type == PacketType::Accept
      && packet.target == _instance && packet.sender == _peer_instance
      && packet.session == _session && sameIp(source, _peer_ip)) {
    _last_peer_ms = now_ms;
    _peer_confirmed = true;
    setState(State::Connected);
    sendPacket(PacketType::KeepAlive, _peer_ip, _peer_instance);
    return;
  }

  if (!isPeerPacket(packet, source)) return;
  _last_peer_ms = now_ms;

  switch (packet.type) {
    case PacketType::KeepAlive:
      _peer_confirmed = true;
      break;

    case PacketType::Join:
      if (_role == Role::Host) sendPacket(PacketType::Accept, _peer_ip, _peer_instance);
      break;

    case PacketType::Transfer:
      if (_last_reply_valid && packet.sequence == _last_reply_sequence) {
        sendPacket(PacketType::Reply, _peer_ip, _peer_instance,
                   _last_reply_sequence, _last_reply_value);
        break;
      }
      if (!_remote.pending || packet.sequence != _remote.sequence) {
        if (_remote.pending)
          sendPacket(PacketType::Reply, _peer_ip, _peer_instance,
                     _remote.sequence, 0xFF);
        _remote.pending = true;
        _remote.value = packet.value;
        _remote.sequence = packet.sequence;
        _remote.started_ms = now_ms;
      }
      break;

    case PacketType::Reply:
      if (_local.pending && _local.internal
          && packet.sequence == _local.sequence) {
        const uint32_t elapsed = now_ms - _local.started_ms;
        _last_rtt_ms = (uint16_t)(elapsed > 0xFFFFu ? 0xFFFFu : elapsed);
        _local = LocalTransfer{};
        queueCompletion(packet.value);
        ++_revision;
      }
      break;

    case PacketType::Bye:
      clearPeer(true);
      break;

    default:
      break;
  }
}

void GameBoyWifiLink::receivePackets(uint32_t now_ms) {
  uint8_t wire[gameboy_link::kWireSize];
  for (int handled = 0; handled < 8; ++handled) {
    const int packet_size = _udp.parsePacket();
    if (packet_size <= 0) break;
    const IPAddress source = _udp.remoteIP();
    const int got = _udp.read(wire, sizeof(wire));
    _udp.flush();
    if (packet_size != (int)sizeof(wire) || got != (int)sizeof(wire)) continue;
    Packet packet;
    if (gameboy_link::decode(wire, sizeof(wire), packet))
      handlePacket(packet, source, now_ms);
  }
}

void GameBoyWifiLink::pairTransfers(uint32_t now_ms) {
  if (!_local.pending || !_remote.pending || _state != State::Connected) return;

  sendPacket(PacketType::Reply, _peer_ip, _peer_instance,
             _remote.sequence, _local.value);
  _last_reply_valid = true;
  _last_reply_sequence = _remote.sequence;
  _last_reply_value = _local.value;

  const uint8_t incoming = _remote.value;
  const uint32_t elapsed = now_ms - _local.started_ms;
  _last_rtt_ms = (uint16_t)(elapsed > 0xFFFFu ? 0xFFFFu : elapsed);
  _local = LocalTransfer{};
  _remote = RemoteTransfer{};
  queueCompletion(incoming);
  ++_revision;
}

void GameBoyWifiLink::serviceTransferTimeouts(uint32_t now_ms) {
  if (_local.pending) {
    if ((uint32_t)(now_ms - _local.started_ms) >= kTransferTimeoutMs) {
      _local = LocalTransfer{};
      queueCompletion(0xFF);
      ++_revision;
    } else if (_state == State::Connected && _peer_instance != 0
               && _local.internal
               && (uint32_t)(now_ms - _local.last_send_ms) >= kTransferRetryMs) {
      sendPacket(PacketType::Transfer, _peer_ip, _peer_instance,
                 _local.sequence, _local.value, gameboy_link::kInternalClock);
      _local.last_send_ms = now_ms;
    }
  }

  if (_remote.pending
      && (uint32_t)(now_ms - _remote.started_ms) >= kTransferTimeoutMs) {
    sendPacket(PacketType::Reply, _peer_ip, _peer_instance,
               _remote.sequence, 0xFF);
    _last_reply_valid = true;
    _last_reply_sequence = _remote.sequence;
    _last_reply_value = 0xFF;
    _remote = RemoteTransfer{};
  }
}

void GameBoyWifiLink::service(uint32_t now_ms) {
  if (_role == Role::Off) return;

  // main.cpp normally enables DTIM sleep once after association. Keep it off
  // only while an emulator link is active so serial replies are not delayed.
  if (WiFi.getSleep() != WIFI_PS_NONE) WiFi.setSleep(false);

  if (!wifiReady()) {
    closeSocket();
    if (_local.pending) queueCompletion(0xFF);
    _local = LocalTransfer{};
    _remote = RemoteTransfer{};
    _peer_instance = 0;
    _peer_ip = IPAddress();
    _peer_confirmed = false;
    _same_game = true;
    _last_rtt_ms = 0;
    _last_peer_ms = 0;
    _last_keepalive_ms = 0;
    if (_state != State::NoWifi) {
      if (_role == Role::Host) _session = nonzeroRandom();
      else if (_role == Role::Join) _session = 0;
    }
    setState(State::NoWifi);
    return;
  }

  if (!_socket_open) {
    if (!openSocket()) {
      setState(State::Error);
      return;
    }
    if (_role == Role::Host && _session == 0) _session = nonzeroRandom();
    setState(State::Searching);
    _last_announce_ms = 0;
  }

  receivePackets(now_ms);
  pairTransfers(now_ms);
  serviceTransferTimeouts(now_ms);

  if (_state == State::Connected) {
    if (_last_peer_ms != 0
        && (uint32_t)(now_ms - _last_peer_ms) >= kPeerTimeoutMs) {
      clearPeer(true);
    } else {
      if (_role == Role::Host && !_peer_confirmed
          && (uint32_t)(now_ms - _last_announce_ms) >= kAnnounceMs) {
        _last_announce_ms = now_ms;
        sendPacket(PacketType::Accept, _peer_ip, _peer_instance);
      }
      sendKeepAlive(now_ms);
    }
  } else {
    if (_role == Role::Join && _peer_instance != 0 && _last_peer_ms != 0
        && (uint32_t)(now_ms - _last_peer_ms) >= kHandshakeTimeoutMs) {
      clearPeer(false);
    }
    sendAnnouncement(now_ms);
  }
}

bool GameBoyWifiLink::beginTransfer(uint8_t outgoing, bool internal_clock) {
  if (_role == Role::Off) return false;
  if (_local.pending) return true;

  _local.pending = true;
  _local.internal = internal_clock;
  _local.value = outgoing;
  _local.sequence = ++_next_sequence;
  if (_local.sequence == 0) _local.sequence = ++_next_sequence;
  _local.started_ms = millis();
  _local.last_send_ms = 0;

  if (internal_clock && _state == State::Connected && _peer_instance != 0) {
    sendPacket(PacketType::Transfer, _peer_ip, _peer_instance,
               _local.sequence, _local.value, gameboy_link::kInternalClock);
    _local.last_send_ms = _local.started_ms;
  }
  return true;
}

bool GameBoyWifiLink::takeCompleted(uint8_t& incoming) {
  if (!_completion_ready) return false;
  incoming = _completion_value;
  _completion_ready = false;
  return true;
}

void GameBoyWifiLink::statusText(char* out, size_t cap) const {
  if (!out || cap == 0) return;
  switch (_state) {
    case State::Off:
      snprintf(out, cap, "WiFi link: Off");
      break;
    case State::NoWifi:
      snprintf(out, cap, "WiFi link: connect WiFi first");
      break;
    case State::Searching:
      snprintf(out, cap, _role == Role::Host ? "WiFi link: Hosting..."
                                              : "WiFi link: Finding host...");
      break;
    case State::Connected:
      if (_last_rtt_ms)
        snprintf(out, cap, "WiFi link: Connected (%u ms)%s",
                 (unsigned)_last_rtt_ms, _same_game ? "" : " - different game");
      else
        snprintf(out, cap, "WiFi link: Connected%s",
                 _same_game ? "" : " - different game");
      break;
    case State::Error:
      snprintf(out, cap, "WiFi link: socket error");
      break;
  }
  out[cap - 1] = '\0';
}

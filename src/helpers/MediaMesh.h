// SPDX-License-Identifier: GPL-3.0-or-later
//
// MediaMesh / radio2 glue: the saved settings and the RAM-only temporary
// profiles and schedules, applying them to the radio, the per-packet TX choice,
// where received packets arrived, and the radio-profile CLI. The rules are in
// MediaMeshCore.h; see MEDIAMESH.md.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <helpers/StaticPoolPacketManager.h>

#include "MediaMeshCore.h"

namespace mesh { class Packet; }

namespace mediamesh {

/** True when this build's radio can run a second profile (SX126x, LR1110,
 * LR1121 or LR2021 on an ESP32). */
bool supported();
/** Why not, when !supported(), for the settings: "needs an SX1262", or for an
 * SX1262 whose DIO1 is polled (behind an I/O expander, as on the T-Display P4)
 * that the radio can't flag packets to the processor. */
bool sx1262Polled();

/** The saved settings (loaded on first use). */
const Config& config();

/** The running state: saved settings plus temporary profiles and schedules. */
const Runtime& runtime();

/** The saved primary profile (the node's radio settings), for tempradio's restore. */
using SavedPrimaryFn = Params (*)();

/** Load from storage and apply to the radio. Call once the main radio is set up. */
void begin(SavedPrimaryFn saved_primary);

/** Timers: temporary sessions ending, schedules starting. Call every loop. */
void loop();

/** Client Repeat on or off: turning it on switches radio2 Off (and ends a
 *  temporary radio2), remembering the mode it had; turning it off puts that
 *  mode back. Call every loop with the current setting (MediaMeshCore.h
 *  holdForClientRepeat). */
void setClientRepeat(bool on);

/** Replace the saved settings: saves and applies. Returns false if invalid. */
bool set(const Config& c);

// ---- TX choices -------------------------------------------------------------

/** Mark a packet the firmware is about to send with an explicit choice
 *  (Policy); TX_AUTO leaves the global rules to decide when it is queued. */
void markPacket(const mesh::Packet* pkt, uint8_t policy);

/** Whether a packet already carries a choice (markPacket / markMedia). */
bool isMarked(const mesh::Packet* pkt);

/** Mark a photo or voice packet: tx.media, or `channel_policy` when tx.media
 *  is auto and the packet belongs to a channel. */
void markMedia(const mesh::Packet* pkt, uint8_t channel_policy = TX_AUTO);

/** An Open TAK fetch request for session `sid` was received: remembers the
 *  profile it arrived on (rxOrigin()), for markTransfer with mediamesh.share. */
void noteFetch(uint32_t sid);

/** Mark an Open TAK transfer packet (photo/voice data or a fetch request):
 *  tx.media's route (or `channel_policy` when tx.media is auto), widened by
 *  mediamesh.share for nodes only on the main mesh (MediaMeshCore.h
 *  transferPolicy). `sid` is the packet's session when known (has_sid);
 *  `channel` that it floods on a channel. */
void markTransfer(const mesh::Packet* pkt, bool request, bool has_sid, uint32_t sid, bool channel,
                  uint8_t channel_policy = TX_AUTO);

/** Packets queued while a context is set get its choice (contact or channel
 *  traffic, including ACKs and replies). Returns the previous context. */
uint8_t setTxContext(uint8_t policy);
static constexpr uint8_t kNoContext = 0xFF;

/** Where a packet goes under the current settings and its choice, as a mask
 *  (bit 0 primary, bit 1 radio2): for getters' `active=`. */
uint8_t policyMaskNow(uint8_t policy);

/** The policy (Policy, or kNoContext when unknown) of the contact a packet is
 *  addressed to, for direct sends no wrapper names (the core's keep-alives,
 *  replies). MyMesh supplies it. */
using ContactPolicyResolver = uint8_t (*)(const mesh::Packet* pkt);
void setContactPolicyResolver(ContactPolicyResolver fn);

/** The packet manager MyMesh uses: settles each packet's profiles as it is
 *  queued (a packet for both becomes two, one per profile), and at send time
 *  drops profiles that changed since. */
class MarkingPacketManager : public StaticPoolPacketManager {
 public:
  explicit MarkingPacketManager(int pool_size) : StaticPoolPacketManager(pool_size) {}
  void queueOutbound(mesh::Packet* packet, uint8_t priority, uint32_t scheduled_for) override;
  mesh::Packet* getNextOutbound(uint32_t now) override;
  void free(mesh::Packet* packet) override;
};

// ---- receive ----------------------------------------------------------------

/** Around processing a received packet: takes the profile it arrived on (0
 *  primary, 1 radio2) and makes it the origin of anything queued meanwhile
 *  (forwards, ACKs, replies). Returns the previous state for endRx(). */
uint8_t beginRx(const mesh::Packet* pkt);
void endRx(uint8_t prev);
/** The profile the packet being processed arrived on (0 outside one). */
uint8_t rxOrigin();
/** Whether rxOrigin() was found for the packet being processed. When the
 *  radio's record of it is missing, rxOrigin() says 0 (the main mesh) for
 *  routing, but that is a guess, not something to show. */
bool rxOriginKnown();

/** onRecvPacket is forwarding `pkt` (Client Repeat): the dispatcher queues
 *  it after the receive context has ended, so this carries the profile it
 *  arrived on to the queue. It then goes out by the radio2.cross rules from
 *  that profile (with MediaMesh's cross off: the same one), not the main
 *  profile's. Call it when onRecvPacket returns a retransmit. */
void noteForward(const mesh::Packet* pkt);

/** Client Repeat: this packet was already forwarded, but not on the profile
 *  it has just arrived on (its sender put it on both radios). The core would
 *  drop it as seen; it still needs forwarding on this one. */
bool forwardedOnOtherRadioOnly(const mesh::Packet* pkt);

/** radio2.repeat: the packet being processed arrived on radio2 and this
 *  node relays radio2 traffic (it goes back out on radio2, by noteForward). */
bool relayingRadio2Packet();

/** Whether a second profile is configured now (rx or rxtx): `#r2-` channels
 *  are bound to it. */
bool radio2Defined();

/** Whether photos and voice are currently sent on radio2. */
bool mediaActive();

/** Whether photos and voice can go out at all: only on radio2 alone. Off and
 *  without radio2 to transmit on, they don't go out, so the UI shouldn't offer
 *  to send them. */
bool mediaSendable();

/** Whether a valid packet (passed the radio's CRC, parses as MeshCore) was
 *  heard on radio2 in the last `window_ms`: a MediaMesh is on the air nearby.
 *  RAM only, so a reboot starts dark. */
bool radio2HeardWithin(uint32_t window_ms);

/** Airtime in ms of a `payload_len`-byte packet on radio2 when photos and voice
 *  go out on radio2 alone (tx.media radio2, share off), for pacing a transfer;
 *  0 when they (also) use the main radio, which then sets the pace. */
uint32_t mediaRadio2AirMs(int payload_len);

/** Airtime in ms of a `payload_len`-byte packet on radio2 while it is on (rx
 *  or rxtx), for timing what arrives over it; 0 when radio2 is off. */
uint32_t radio2AirMs(int payload_len);

// ---- per-contact / per-channel choices ---------------------------------------

/** The saved choice for a contact (first 8 bytes of its public key) or a
 *  channel (Routes id of its secret). TX_AUTO when none. */
uint8_t userPolicy(const uint8_t* pub_key);
uint8_t channelPolicyById(const uint8_t id[Routes::kIdSize]);
/** Saves a choice; false when the table is full or storage failed. */
bool setUserPolicy(const uint8_t* pub_key, uint8_t policy);
bool setChannelPolicyById(const uint8_t id[Routes::kIdSize], uint8_t policy);
int routeCount(uint8_t kind);
/** A channel's Routes id: SHA-256 of its 32-byte secret, first 8 bytes. */
void channelRouteId(const uint8_t secret[32], uint8_t out[Routes::kIdSize]);

// ---- CLI ----------------------------------------------------------------------

/** Runs the radio-profile commands (radio2, tempradio2, radioat2,
 *  tempradioat2, tempradio, normalradio, radio2.cross, radio2.status,
 *  radio2.scan, radio2.timing, tx.media, mediamesh). False when `cmd` is none
 *  of these; tx.user / tx.channel are MyMesh's. */
bool runCli(const char* cmd, char* reply, size_t cap);

/** Live state for the settings page. */
struct Status {
  bool scanning = false;
  uint8_t profile = 0;
  uint32_t switches = 0;
  uint32_t switch_us = 0;
  uint32_t tx[2] = {};
  uint32_t rx[2] = {};
  bool temp2 = false;             // tempradio2 running
  uint32_t temp2_left_ms = 0;
  bool temp1 = false;             // tempradio running
  uint32_t temp1_left_ms = 0;
  uint8_t switch_samples = 0;     // retunes measured since the last profile change
  uint32_t switch_budget_us = 0;  // guarded per-retune allowance in use
  uint32_t switch_errors = 0;
  Profile2 temp2_prof;            // the temporary radio2, when temp2
  Timing timing;
  uint8_t mode = 0;               // radio2 in use: off / rx / rxtx
  uint32_t dropped = 0;           // radio2 packets that never went out
  uint32_t scan_busy_us = 0;      // the scan's time holding the radio (and its SPI bus), since boot
  bool timing_valid = false;
  Params r2;                      // the radio2 profile in use (to compare radios)
};
Status status();

}  // namespace mediamesh

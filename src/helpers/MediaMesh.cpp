// SPDX-License-Identifier: GPL-3.0-or-later

#include "ClientRepeat.h"
#include "MediaMesh.h"

#include <Arduino.h>
#include <Packet.h>
#include <Utils.h>
#include <target.h>

#include "MediaMeshRadio.h"
#include "esp32/TouchPrefsStore.h"
#include "PsramStatic.h"

namespace mediamesh {

namespace {

constexpr const char* kPrefsKey = "mediamesh";
constexpr const char* kRoutesKey = "mmroutes";

Runtime s_rt;
Routes& s_routes = psramObject<Routes>();   // these rings: ~6 KB, in PSRAM, not internal RAM
bool s_loaded = false;
MarkRing& s_marks = psramObject<MarkRing>();            // choices made before a packet is queued, by hash
QueueMarks& s_queued = psramObject<QueueMarks>();         // each queued packet's profiles, by packet
OriginRing& s_origins = psramObject<OriginRing>();
SavedPrimaryFn s_saved_primary = nullptr;
ContactPolicyResolver s_resolver = nullptr;
volatile uint8_t s_ctx = kNoContext;
uint8_t s_rx_origin = kNoMark;   // while a received packet is being processed
bool s_rx_known = false;         // s_rx_origin came from the radio's record
// A forward (Client Repeat): the dispatcher queues it after onRecvPacket has
// returned, outside the receive context, so the profile it arrived on is
// carried here from noteForward() to queueOutbound().
const mesh::Packet* s_fwd_pkt = nullptr;
uint8_t s_fwd_origin = 0;
MarkRing& s_forwarded = psramObject<MarkRing>();            // the profiles each forwarded packet went out on, by hash
bool s_temp1_applied = false;

// Profile generations: bumped whenever a profile in use changes, so queued
// packets bound to the old one are not sent there.
uint32_t s_gen[2] = {1, 1};
Profile2 s_applied2;
bool s_applied2_known = false;
Params s_applied1;
bool s_applied1_temp = false;

// Queued packets dropped before sending: their profile changed, or radio2
// couldn't transmit (radio2.status drop=, About's MediaMesh lines).
uint32_t s_queue_dropped = 0;
// Listen before talk on radio2 (MediaMeshCore.h lbtHold): packets held back,
// and when the current wait began (0: none).
uint32_t s_lbt_waits = 0;
uint32_t s_lbt_since = 0;
uint32_t s_lbt_quiet = 0;

// The packet the dispatcher is about to send (getNextOutbound -> startSendRaw).
uint8_t s_next_mask = 1;
bool s_next_valid = false;

bool s_never_saved = false;   // no MediaMesh settings stored yet: begin() picks the default

void ensureLoaded() {
  if (s_loaded) return;
  s_loaded = true;
  defaults(s_rt.saved);
  uint8_t blob[kBlobSize + 8];
  size_t n = touchPrefsGetBlob(kPrefsKey, blob, sizeof blob);
  Config c;
  if (n && decode(blob, n, c)) s_rt.saved = c;
  else s_never_saved = true;
  static auto& rblob = psramStatic<uint8_t[Routes::kBlobMax]>();
  n = touchPrefsGetBlob(kRoutesKey, rblob, sizeof rblob);
  if (n) s_routes.decode(rblob, n);
}

bool save() {
  uint8_t blob[kBlobSize];
  const size_t n = encode(s_rt.saved, blob);
  return touchPrefsSetBlob(kPrefsKey, blob, n);
}

bool saveRoutes() {
  static auto& blob = psramStatic<uint8_t[Routes::kBlobMax]>();
  const size_t n = s_routes.encode(blob);
  return touchPrefsSetBlob(kRoutesKey, blob, n);
}

// When a valid packet was last heard on radio2 (radio2HeardWithin).
static volatile uint32_t s_r2_heard_ms = 0;
static volatile bool s_r2_heard = false;

#if defined(MEDIAMESH_RADIO)
void packetHash(const uint8_t* raw, int len, uint8_t out[kHashSize], bool* ok) {
  *ok = false;
  if (!raw || len <= 0 || len > MAX_TRANS_UNIT) return;
  mesh::Packet pkt;
  if (!pkt.readFrom(raw, (uint8_t)len)) return;
  pkt.calculatePacketHash(out);
  *ok = true;
}

// Called from the radio for every packet about to be transmitted: the profiles
// getNextOutbound settled for it.
uint8_t classifyRaw(const uint8_t* raw, int len) {
  if (s_next_valid) {
    s_next_valid = false;
    return s_next_mask;
  }
  bool ok;
  uint8_t hash[kHashSize];
  packetHash(raw, len, hash, &ok);
  if (ok) {
    const uint8_t m = s_marks.find(hash, millis());
    if (m != kNoMark) return m;
  }
  return transmitMask(s_rt, 0);
}

// For the dispatcher's airtime estimate of that packet; 0 = not known.
uint8_t peekMask() { return s_next_valid ? s_next_mask : 0; }

// Called from the radio for every packet received.
void noteOrigin(const uint8_t* raw, int len, uint8_t origin) {
  bool ok;
  uint8_t hash[kHashSize];
  packetHash(raw, len, hash, &ok);
  if (ok) s_origins.add(hash, millis(), origin);
  // A whole MeshCore packet (it passed the radio's CRC and parses) on radio2:
  // a MediaMesh is on the air here.
  if (ok && origin == 1) {
    s_r2_heard_ms = millis();
    s_r2_heard = true;
  }
}
#endif

Params savedPrimary() {
  if (s_saved_primary) return s_saved_primary();
#if defined(MEDIAMESH_RADIO)
  return radio_driver.mainSavedParams();
#else
  return Params();
#endif
}

void applySecondary() {
  const Profile2 a = s_rt.active2();
  if (!s_applied2_known || a.mode != s_applied2.mode || !sameParams(a.p, s_applied2.p)) {
    if (s_applied2_known) ++s_gen[1];
    s_applied2 = a;
    s_applied2_known = true;
  }
#if defined(MEDIAMESH_RADIO)
  radio_driver.setMediaClassifier(classifyRaw, peekMask);
  radio_driver.setOriginSink(noteOrigin);
  radio_driver.mediaMeshConfigure(a);
#endif
}

void applyPrimary() {
  if (s_rt.temp1 != s_applied1_temp || (s_rt.temp1 && !sameParams(s_rt.temp1_p, s_applied1))) ++s_gen[0];
  s_applied1_temp = s_rt.temp1;
  s_applied1 = s_rt.temp1_p;
#if defined(MEDIAMESH_RADIO)
  radio_driver.setTempPrimary(s_rt.temp1 ? &s_rt.temp1_p : nullptr);
#else
  // Boards without the scanning wrapper: retune the plain radio, and back.
  const Params p = s_rt.temp1 ? s_rt.temp1_p : savedPrimary();
  if (validParams(p)) radio_driver.setParams(p.freq, p.bw, p.sf, p.cr);
#endif
  s_temp1_applied = s_rt.temp1;
}

Env makeEnv() {
  Env e;
  e.supported = supported();
  e.now_ms = millis();
  e.epoch = rtc_clock.getCurrentTime();
  if (e.epoch < 1600000000UL) e.epoch = 0;   // the clock was never set
#if defined(MEDIAMESH_RADIO)
  e.main = radio_driver.mainParams();
  e.main_saved = radio_driver.mainSavedParams();
  e.main_preamble = radio_driver.mainPreambleSymbols();
  e.switch_us = radio_driver.mediaMeshSwitchBudgetUs();
  for (uint8_t i = 0; i < 2; ++i) {
    e.stats.rx[i] = radio_driver.mediaMeshRxCount(i);
    e.stats.tx[i] = radio_driver.mediaMeshTxCount(i);
  }
  e.stats.switches = radio_driver.mediaMeshSwitches();
  e.stats.errors = radio_driver.mediaMeshSwitchErrors();
  e.stats.max_us = radio_driver.mediaMeshSwitchMaxUs();
  e.stats.stalls = radio_driver.mediaMeshSwitchStalls();
  e.stats.typ_us = radio_driver.mediaMeshSwitchTypUs();
  if (s_r2_heard) e.stats.heard_s = (uint32_t)(millis() - s_r2_heard_ms) / 1000;
  e.stats.samples = radio_driver.mediaMeshSwitchSamples();
  e.stats.dropped = radio_driver.mediaMeshRefused() + s_queue_dropped;
  e.stats.waits = s_lbt_waits;
#else
  e.main_saved = savedPrimary();
  e.main = s_rt.temp1 ? s_rt.temp1_p : e.main_saved;
#endif
  return e;
}

}  // namespace

bool radio2HeardWithin(uint32_t window_ms) {
  return s_r2_heard && (uint32_t)(millis() - s_r2_heard_ms) < window_ms;
}

bool supported() {
#if defined(MEDIAMESH_RADIO)
  return true;
#else
  return false;
#endif
}

bool sx1262Polled() {
#if !defined(MEDIAMESH_RADIO) && (defined(USE_SX1262) || defined(USE_SX1268)) && defined(MESH_RADIO_DIO1_POLLED)
  return true;
#else
  return false;
#endif
}

const Config& config() {
  ensureLoaded();
  return s_rt.saved;
}

const Runtime& runtime() {
  ensureLoaded();
  return s_rt;
}

void begin(SavedPrimaryFn saved_primary) {
  s_saved_primary = saved_primary;
#if defined(MEDIAMESH_RADIO) && defined(MEDIAMESH_HAS_24GHZ)
  // Ask the chip whether it really does 2.4 GHz (the build only says it might).
  setHas24GHz(radio_driver.chipDoes24GHz());
#endif
  ensureLoaded();
  if (is24GHz(s_rt.saved.p.freq) && !has24GHz()) {
    // A 2.4 GHz radio2 saved on a chip without it: the sub-GHz default for
    // now, and the saved choice left as it is for a radio that can.
    Config d;
    defaults(d);
    s_rt.saved.p = d.p;
  }
  // Never set up: radio2 on where its default frequency is legal (decided at
  // each boot until something is saved, so a radio moved to another band
  // doesn't keep transmitting on 911 MHz).
  if (s_never_saved && s_rt.saved.mode == MODE_OFF && defaultOnFor(savedPrimary().freq)) s_rt.saved.mode = MODE_RXTX;
  // "Sent on radio2": Photos & voice by default, and once for a radio still on
  // the old automatic pair. The key remembers the move, so a later choice of
  // that pair from the Terminal stays.
  {
    static const char kPvKey[] = "mm_pv_def";
    uint8_t done = 0;
    if (!touchPrefsGetBlob(kPvKey, &done, 1) || !done) {
      const bool changed = photoVoiceDefault(s_rt.saved);
      done = 1;
      touchPrefsSetBlob(kPvKey, &done, 1);
      if (changed && !s_never_saved) save();
    }
  }
  if (!supported()) s_rt.saved.mode = MODE_OFF;
  applySecondary();
}

void loop() {
  ensureLoaded();
  const TickResult t = tick(s_rt, millis());
  if (t.save) save();
  if (t.apply) applySecondary();
  if (t.apply_primary || s_temp1_applied != s_rt.temp1) applyPrimary();
}

void setClientRepeat(bool on) {
  ensureLoaded();
  // radio2 beside the off-grid main channel (US band), else off.
  Params beside;
  bool has_beside = false;
  if (on && supported()) {
    const Params m = savedPrimary();
    clientrepeat::Radio main, r2;
    main.freq = m.freq; main.bw = m.bw; main.sf = m.sf; main.cr = m.cr;
    if (clientrepeat::radio2For(main, r2)) {
      beside.freq = r2.freq; beside.bw = r2.bw; beside.sf = r2.sf; beside.cr = r2.cr; beside.preamble = 0;
      has_beside = validParams(beside);
    }
  }
  if (!holdForClientRepeat(s_rt, on, has_beside ? &beside : nullptr)) return;
  save();
  applySecondary();
}

bool set(const Config& c) {
  ensureLoaded();
  if (c.mode != MODE_OFF && !validParams(c.p)) return false;
  const bool profile_changed = c.mode != s_rt.saved.mode || !sameParams(c.p, s_rt.saved.p);
  s_rt.saved = c;
  if (!supported()) s_rt.saved.mode = MODE_OFF;
  if (profile_changed && c.mode == MODE_OFF) {
    // As `set radio2 off`: no temporary session or schedule outlives it.
    endTemp2(s_rt);
    for (auto& s : s_rt.sched) s = Schedule();
  }
  save();
  applySecondary();
  return true;
}

// ---- TX choices -------------------------------------------------------------

void markPacket(const mesh::Packet* pkt, uint8_t policy) {
  if (!pkt || policy == TX_AUTO) return;
  ensureLoaded();
  uint8_t hash[MAX_HASH_SIZE];
  pkt->calculatePacketHash(hash);
  s_marks.add(hash, millis(), policyMask(s_rt, policy));
}

bool isMarked(const mesh::Packet* pkt) {
  if (!pkt) return false;
  uint8_t hash[MAX_HASH_SIZE];
  pkt->calculatePacketHash(hash);
  return s_marks.has(hash, millis());
}

void markMedia(const mesh::Packet* pkt, uint8_t channel_policy) {
  ensureLoaded();
  (void)channel_policy;   // photos and voice: radio2 alone, never the main mesh
  markPacket(pkt, effectiveMedia(s_rt.saved));
}

static FetchLog s_fetch;

void noteFetch(uint32_t sid) { s_fetch.note(sid, rxOrigin(), millis()); }

void markTransfer(const mesh::Packet* pkt, bool request, bool has_sid, uint32_t sid, bool channel,
                  uint8_t channel_policy) {
  ensureLoaded();
  // Radio2 alone (effectiveMedia), share off: never the main mesh.
  (void)channel_policy;
  const uint8_t media = effectiveMedia(s_rt.saved);
  const uint8_t asked = has_sid ? s_fetch.mask(sid, millis()) : 0;
  markPacket(pkt, transferPolicy(effectiveShare(s_rt.saved), request, asked, channel, media));
}

uint8_t setTxContext(uint8_t policy) {
  const uint8_t prev = s_ctx;
  s_ctx = policy;
  return prev;
}

uint8_t policyMaskNow(uint8_t policy) {
  ensureLoaded();
  return policyMask(s_rt, policy);
}

void setContactPolicyResolver(ContactPolicyResolver fn) { s_resolver = fn; }

uint8_t beginRx(const mesh::Packet* pkt) {
  uint8_t o = kNoMark;
  if (pkt) {
    uint8_t hash[MAX_HASH_SIZE];
    pkt->calculatePacketHash(hash);
    o = s_origins.take(hash, millis());
  }
  const bool known = o != kNoMark;
  if (!known) o = 0;
  // The previous state, for endRx: its origin, with bit 7 set when that
  // one was a guess (kNoMark, 0xFF, is never a guess: nothing was handled).
  const uint8_t prev = s_rx_origin == kNoMark ? kNoMark : (uint8_t)(s_rx_origin | (s_rx_known ? 0 : 0x80));
  s_rx_origin = o;
  s_rx_known = known;
  return prev;
}

void endRx(uint8_t prev) {
  if (prev == kNoMark) {
    s_rx_origin = kNoMark;
    s_rx_known = false;
  } else {
    s_rx_origin = (uint8_t)(prev & 0x7F);
    s_rx_known = (prev & 0x80) == 0;
  }
}

bool rxOriginKnown() { return s_rx_origin != kNoMark && s_rx_known; }

void noteForward(const mesh::Packet* pkt) {
  s_fwd_pkt = pkt;
  s_fwd_origin = rxOrigin();
}

bool forwardedOnOtherRadioOnly(const mesh::Packet* pkt) {
  if (!pkt) return false;
  ensureLoaded();
  uint8_t hash[MAX_HASH_SIZE];
  pkt->calculatePacketHash(hash);
  const uint8_t done = s_forwarded.find(hash, millis());
  return done != kNoMark && forwardMask(s_rt, rxOrigin(), done) != 0;
}

uint8_t rxOrigin() { return s_rx_origin == kNoMark ? 0 : s_rx_origin; }

bool relayingRadio2Packet() {
  ensureLoaded();
  // Only a packet the radio says came in on radio2: a guess would relay
  // main-mesh traffic onto radio2.
  return rxOriginKnown() && s_rx_origin == 1 && relaysRadio2(s_rt);
}

// A packet's profiles, decided as it is queued (mikecarper/MeshCore fixes them
// when the packet is created):
//   1. a choice marked on it before (photo/voice, channel, #r2-);
//   2. the contact/channel context it was made in, when explicit;
//   3. the contact a direct packet is addressed to (core sends nothing wraps);
//   4. made while handling a received packet (forwards, ACKs, replies): the
//      global rules from the profile that packet arrived on;
//   5. otherwise the global rules for local traffic.
// A packet for both profiles becomes two packets, one per profile, each sent
// (and checked for a busy channel) on its own.
void MarkingPacketManager::queueOutbound(mesh::Packet* packet, uint8_t priority, uint32_t scheduled_for) {
  if (!packet) return;
  ensureLoaded();
  if (s_queued.has(packet)) {   // re-queued (a held text put back): keeps its profiles
    StaticPoolPacketManager::queueOutbound(packet, priority, scheduled_for);
    return;
  }
  uint8_t hash[MAX_HASH_SIZE];
  packet->calculatePacketHash(hash);
  const uint32_t now = millis();
  uint8_t mask = kNoMark;
  if (packet == s_fwd_pkt) {
    // A forward: the rules from the profile it arrived on, less any profile
    // the same packet was already forwarded on (it came in on both).
    s_fwd_pkt = nullptr;
    const uint8_t done = s_forwarded.find(hash, now);
    mask = forwardMask(s_rt, s_fwd_origin, done);
    s_forwarded.add(hash, now, (uint8_t)((done == kNoMark ? 0 : done) | mask));
  }
  if (mask == kNoMark) mask = s_marks.find(hash, now);
  if (mask == kNoMark && s_ctx != kNoContext && s_ctx != TX_AUTO) mask = policyMask(s_rt, s_ctx);
  if (mask == kNoMark && s_ctx == kNoContext && s_rx_origin == kNoMark && s_resolver &&
      s_routes.count(Routes::USER) > 0) {
    const uint8_t p = s_resolver(packet);
    if (p != kNoContext && p != TX_AUTO) mask = policyMask(s_rt, p);
  }
  if (mask == kNoMark) mask = transmitMask(s_rt, s_rx_origin != kNoMark ? s_rx_origin : 0);

  if (mask == 3 && getFreeCount() > 2) {
    mesh::Packet* copy = allocNew();
    if (copy) {
      *copy = *packet;
      if (s_queued.set(packet, 1, s_gen) && s_queued.set(copy, 2, s_gen)) {
        StaticPoolPacketManager::queueOutbound(packet, priority, scheduled_for);
        StaticPoolPacketManager::queueOutbound(copy, priority, scheduled_for);
        return;
      }
      s_queued.erase(packet);
      free(copy);
    }
  }
  s_queued.set(packet, mask, s_gen);   // when full, getNextOutbound falls back to the rules
  StaticPoolPacketManager::queueOutbound(packet, priority, scheduled_for);
}

mesh::Packet* MarkingPacketManager::getNextOutbound(uint32_t now) {
  for (;;) {
    mesh::Packet* p = StaticPoolPacketManager::getNextOutbound(now);
    if (!p) return nullptr;
    uint8_t mask = s_queued.live(p, s_gen);
    if (mask == kNoMark) mask = transmitMask(s_rt, 0);
    if (mask == 0) {
      // Its profile changed or went away since it was queued, or its choice
      // can't be sent now (radio2 while rx-only): drop it, as a failed send.
      ++s_queue_dropped;
      free(p);
      continue;
    }
#if defined(MEDIAMESH_RADIO)
    // radio2 alone, and radio2 busy (a packet heard on it lately): back in the
    // queue until it has been quiet a while. Main-mesh packets go meanwhile.
    if (mask == 2) {
      if (!s_lbt_quiet) s_lbt_quiet = lbtQuietMs(radio2AirMs(MAX_TRANS_UNIT), (uint32_t)random(0x7FFFFFFF));
      const uint32_t since = (uint32_t)(now - s_r2_heard_ms);
      const uint32_t waiting = s_lbt_since ? (uint32_t)(now - s_lbt_since) : 0;
      if (lbtHold(s_r2_heard, since, s_lbt_quiet, waiting)) {
        if (!s_lbt_since) { s_lbt_since = now ? now : 1; ++s_lbt_waits; }
        StaticPoolPacketManager::queueOutbound(p, 1, now + (s_lbt_quiet - since));
        continue;
      }
      s_lbt_since = 0;
      s_lbt_quiet = 0;   // a fresh random part for the next wait
    }
#endif
    s_next_mask = mask;
    s_next_valid = true;
    return p;
  }
}

void MarkingPacketManager::free(mesh::Packet* packet) {
  s_queued.erase(packet);
  StaticPoolPacketManager::free(packet);
}

// ---- receive ----------------------------------------------------------------

bool radio2Defined() {
  ensureLoaded();
  return supported() && s_rt.enabled();
}

bool mediaActive() {
  ensureLoaded();
  return supported() && (policyMask(s_rt, effectiveMedia(s_rt.saved)) & 2);
}

bool mediaSendable() {
  ensureLoaded();
  return supported() && policyMask(s_rt, effectiveMedia(s_rt.saved)) == 2;
}

uint32_t mediaRadio2AirMs(int payload_len) {
  ensureLoaded();
  const uint8_t media = effectiveMedia(s_rt.saved);
  if (!supported() || effectiveShare(s_rt.saved) || media == TX_AUTO) return 0;
  if (policyMask(s_rt, media) != 2) return 0;
  return radio2AirMs(payload_len);
}

uint32_t radio2AirMs(int payload_len) {
  if (!radio2Defined()) return 0;
  const Profile2 a = s_rt.active2();
  uint16_t pre = a.p.preamble;
#if defined(MEDIAMESH_RADIO)
  const Timing& t = radio_driver.mediaMeshTiming();
  if (t.valid) pre = t.preamble[1];
#endif
  if (!pre) pre = kMaxAutoPreamble;
  return (timeOnAirUs(a.p, pre, payload_len) + 999) / 1000;
}

// ---- per-contact / per-channel choices ---------------------------------------

uint8_t userPolicy(const uint8_t* pub_key) {
  ensureLoaded();
  return pub_key ? s_routes.get(Routes::USER, pub_key) : (uint8_t)TX_AUTO;
}

uint8_t channelPolicyById(const uint8_t id[Routes::kIdSize]) {
  ensureLoaded();
  return s_routes.get(Routes::CHANNEL, id);
}

bool setUserPolicy(const uint8_t* pub_key, uint8_t policy) {
  ensureLoaded();
  if (!s_routes.set(Routes::USER, pub_key, policy)) return false;
  return saveRoutes();
}

bool setChannelPolicyById(const uint8_t id[Routes::kIdSize], uint8_t policy) {
  ensureLoaded();
  if (!s_routes.set(Routes::CHANNEL, id, policy)) return false;
  return saveRoutes();
}

int routeCount(uint8_t kind) {
  ensureLoaded();
  return s_routes.count(kind);
}

void channelRouteId(const uint8_t secret[32], uint8_t out[Routes::kIdSize]) {
  mesh::Utils::sha256(out, Routes::kIdSize, secret, 32);
}

// ---- CLI ----------------------------------------------------------------------

bool runCli(const char* cmd, char* reply, size_t cap) {
  ensureLoaded();
  const Env env = makeEnv();
  const CliResult r = handleCli(cmd, s_rt, env, reply, cap);
  if (!r.handled) return false;
  if (r.save) save();
  if (r.apply) applySecondary();
  if (r.apply_primary) applyPrimary();
  return true;
}

Status status() {
  ensureLoaded();
  Status s;
  const uint32_t now = millis();
  s.temp2 = s_rt.temp2;
  s.temp2_left_ms = s_rt.temp2 ? msLeft(now, s_rt.temp2_end_ms) : 0;
  s.temp1 = s_rt.temp1;
  s.temp1_left_ms = s_rt.temp1 ? msLeft(now, s_rt.temp1_end_ms) : 0;
  s.temp2_prof = s_rt.temp2_prof;
#if defined(MEDIAMESH_RADIO)
  s.scanning = radio_driver.mediaMeshScanning();
  s.profile = radio_driver.mediaMeshProfile();
  s.switches = radio_driver.mediaMeshSwitches();
  s.switch_us = radio_driver.mediaMeshSwitchUs();
  for (uint8_t i = 0; i < 2; ++i) {
    s.tx[i] = radio_driver.mediaMeshTxCount(i);
    s.rx[i] = radio_driver.mediaMeshRxCount(i);
  }
  s.timing = radio_driver.mediaMeshTiming();
  s.switch_samples = radio_driver.mediaMeshSwitchSamples();
  s.switch_budget_us = radio_driver.mediaMeshSwitchBudgetUs();
  s.switch_errors = radio_driver.mediaMeshSwitchErrors();
  s.dropped = radio_driver.mediaMeshRefused() + s_queue_dropped;
  s.scan_busy_us = radio_driver.mediaMeshScanBusyUs();
  s.timing_valid = radio_driver.mediaMeshTimingValid();
#endif
  s.mode = s_rt.active2().mode;
  s.r2 = s_rt.active2().p;
  return s;
}

}  // namespace mediamesh

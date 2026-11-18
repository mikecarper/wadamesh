// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Wire formats for MeshCore Open TAK photo and voice messages (VOX), so a
// wadamesh radio can fetch photos and voice clips sent from that app. Mirrors
// lib/vox/voice_wire.dart in jade-on-mesh/meshcore-open-tak. Pure C++, no
// Arduino, so it is host-testable (test/test_vox_wire.cpp).
//
//   Envelope (ordinary text message, DM or channel), all fields lowercase base36:
//     VE3:{sid}:{mode}:{total}:{seconds}   Codec2 voice (the published VE3 format)
//     VM1:{sid}:{mode}:{total}:{seconds}   Mimi neural voice (VOX only)
//     VI1:{sid}:{mode}:{total}:{aspect}    AV1 still image (VOX only)
//   then, after a space, a plain word for people on other clients: IMAGE
//   after VI1, VOICE after VM1 (none after VE3: MeshCore SAR, where VE3 comes
//   from, can't play one with anything after it; a talk's is "PTT"), then
//   any caption: "VI1:79e6a5:14:10:46 IMAGE Look at this".
//       sid    32-bit session id
//       mode   VE3: Codec2 mode 0..6 (700C, 1200, 2400, 1300, 1400, 1600, 3200);
//              VM1: 32 ("w"); VI1: 40 ("14")
//       total  packet count, 1..255
//       last   clip length in whole seconds, or for a photo width * 100 / height
//
//   Data packet (raw data on a direct route, or GRP_DATA type 0x5643 on a channel):
//     [0x56][sid:4 BE][index:1][bytes...]          plain
//     [0x57][sid:4 BE][index:1][bytes...][salt:1]  salted resend
//     [0x58][sid:4 BE][index:1][bytes...][mark:1]  marked: mark = (resend << 6) |
//              min(left, 63), `left` the packets of this answer still to come.
//              Channel answers only, to listeners who asked with flag 0x08 (raw
//              0x58 would go out on the main mesh: Keymind routes raw media by its
//              first byte, 0x56/0x57). Same size as 0x57.
//   Every packet but the last carries packetBytes(mode): as many whole codec
//   frames as fit in 156 bytes (156 for photos).
//
//   Fetch request (same planes as the data):
//     [0x72][sid:4 BE][flags:1][requesterKey6:6][count:1][missing indices...]
//       flags bit0 = only the listed indices are wanted; otherwise all.
//             bit3 = reads marked packets (0x58); channel requests only (Keymind
//             sends a raw request on radio2 only with flags in 0x07).
//
//   Text fetch request (flooded DM when there is no direct route):
//     VR3:{sid}:{a | m<ranges>}:{requesterKey6 hex}
//
// Reassembled payload: a photo is [0x01 version][AV1 OBUs, temporal delimiter
// stripped]; a voice clip is the codec's frames back to back.

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace VoxWire {

constexpr uint8_t  kPacketMagic       = 0x56;   // 'V'
constexpr uint8_t  kPacketSaltedMagic = 0x57;   // 'W'
constexpr uint8_t  kPacketMarkedMagic = 0x58;   // 'X': counts down its answer (channel only)
constexpr uint8_t  kFetchMagic        = 0x72;   // 'r'
constexpr uint8_t  kFetchFlagMissing  = 0x01;
constexpr uint8_t  kFetchFlagSaltOk   = 0x02;   // requester takes salted (0x57) resends
constexpr uint8_t  kFetchFlagHasPath  = 0x04;   // [len][path] follows: requester -> sender route
constexpr uint8_t  kFetchFlagMarkOk   = 0x08;   // requester reads marked (0x58) packets: channel only
constexpr uint8_t  kFetchFlagFountainOk = 0x10; // requester rebuilds from repair packets (VoxFountain.h): channel only
constexpr int      kMaxReplyPathBytes = 8;      // senders ignore longer reply paths
constexpr uint16_t kChannelDataType   = 0x5643; // 'VC'
constexpr int      kImageModeId       = 40;
constexpr int      kBytesPerPacket    = 156;
constexpr int      kPacketHeaderLen   = 6;
constexpr int      kMaxPackets        = 255;
constexpr int      kFetchMinLen       = 13;
constexpr int      kMaxFetchIndices   = 140;    // keeps a request (and a reply path) inside one packet
constexpr int      kSaltedFetchIndices = kMaxFetchIndices - 16;   // listed in a salted request
// A photo's payload: [1][AV1 OBUs] (version 1), or what the app sends now,
// [2][thumbnail length][thumbnail OBUs][photo OBUs] (version 2): a thumbnail of
// up to 96 px, all of it in the first kThumbPackets packets (length 0: none).
// The app sent 64 px in one packet at first; both read the same way.
constexpr uint8_t  kImagePayloadVersion = 0x01;
constexpr uint8_t  kImagePayloadV2      = 0x02;
constexpr int      kThumbPackets        = 2;
inline bool isImagePayloadVersion(uint8_t v) { return v == kImagePayloadVersion || v == kImagePayloadV2; }

constexpr int      kMimiModeId        = 32;

// Largest clip Open TAK sends is a "Super Sharp" Mesh photo (38.5 KB: 247
// packets), so take what the wire can say: 255 x 156 B = 40 KB, in PSRAM, only
// while it downloads. (This was 96, from when Sharp's 71 was the most: a Super
// Sharp photo was dropped without a word, its bubble stuck at "tap to
// download".)
constexpr int      kMaxClipPackets    = kMaxPackets;
constexpr int      kMaxImagePackets   = kMaxClipPackets;

enum Kind : uint8_t { kImage = 0, kCodec2, kMimi };

// Bytes in one codec frame for a wire mode id, or 0 for an unknown mode.
inline int bytesPerFrame(int mode) {
  switch (mode) {
    case 0: return 4;                       // 700C
    case 1: case 2: return 6;               // 1200, 2400
    case 3: case 4: return 7;               // 1300, 1400
    case 5: case 6: return 8;               // 1600, 3200
    case kMimiModeId: return 11;
    case kImageModeId: return 1;
    default: return 0;
  }
}
// Codec bytes in every packet but the last: whole frames that fit in 156.
inline int packetBytes(int mode) {
  const int f = bytesPerFrame(mode);
  return f ? (kBytesPerPacket / f) * f : 0;
}
// The CODEC2_MODE_* value for a VE3 wire mode id, or -1.
inline int codec2Mode(int mode) {
  static const int8_t ids[7] = {8, 5, 1, 4, 3, 2, 0};   // 700C 1200 2400 1300 1400 1600 3200
  return mode >= 0 && mode <= 6 ? ids[mode] : -1;
}

struct Envelope {
  uint32_t sid = 0;
  uint8_t  total = 0;    // packets
  uint16_t aspect = 0;   // photo: width * 100 / height; voice: length in seconds
  uint8_t  mode = kImageModeId;   // wire mode id
  Kind     kind = kImage;
  int packetBytes() const { return VoxWire::packetBytes(mode); }
  uint16_t seconds() const { return kind == kImage ? 0 : aspect; }
};

inline bool parseBase36(const char* s, size_t n, uint32_t* out) {
  if (n == 0 || n > 7) return false;
  uint32_t v = 0;
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    uint32_t d;
    if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
    else if (c >= 'a' && c <= 'z') d = (uint32_t)(c - 'a' + 10);
    else return false;
    uint64_t nv = (uint64_t)v * 36u + d;
    if (nv > 0xFFFFFFFFull) return false;
    v = (uint32_t)nv;
  }
  *out = v;
  return true;
}

// Writes `v` in lowercase base36 (at most 7 chars + NUL). Returns chars written.
inline int toBase36(uint32_t v, char* out) {
  char tmp[8];
  int n = 0;
  do { uint32_t d = v % 36u; tmp[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v /= 36u; } while (v);
  for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
  out[n] = '\0';
  return n;
}

// ---- Codec2 copies of Mimi clips (lib/vox/voice_wire.dart) ----------------------
// A standalone can't play Mimi, so it asks the sender for a Codec2 copy by
// fetching a session id made from the Mimi clip's:
//     copy sid = mimi sid XOR (0xC2C20000 | seconds)
// The copy is always Codec2 1200, exactly the Mimi envelope's seconds long, so
// its packet count follows from that and no second envelope is needed. The
// sender's app makes it when first asked; one that doesn't know copies has no
// such session and sends nothing.
constexpr uint32_t kCodec2CopyTag = 0xC2C20000u;
constexpr int      kCodec2CopyMode = 1;           // 1200
constexpr int      kCodec2CopyBytesPerSecond = 150;   // 6 bytes per 40 ms frame
constexpr int      kCodec2CopyMaxSeconds = kMaxPackets * 156 / kCodec2CopyBytesPerSecond;

// Packets in a copy `seconds` long (at least 1 s).
inline int codec2CopyPackets(int seconds) {
  if (seconds < 1) seconds = 1;
  const int per = 156;   // packetBytes(kCodec2CopyMode)
  return (seconds * kCodec2CopyBytesPerSecond + per - 1) / per;
}

// The Codec2 copy's envelope for a Mimi clip's. False for anything else, or a
// clip too long for a copy.
inline bool codec2Copy(const Envelope& mimi, Envelope* copy) {
  if (mimi.kind != kMimi || !copy) return false;
  const int seconds = mimi.aspect < 1 ? 1 : mimi.aspect;
  if (seconds > kCodec2CopyMaxSeconds) return false;
  Envelope c;
  c.sid = mimi.sid ^ kCodec2CopyTag ^ (uint32_t)seconds;
  c.total = (uint8_t)codec2CopyPackets(seconds);
  c.aspect = (uint16_t)seconds;
  c.mode = kCodec2CopyMode;
  c.kind = kCodec2;
  *copy = c;
  return true;
}

// Parses a VE3 / VM1 / VI1 envelope (surrounding whitespace allowed). Text
// after it, past whitespace, is a caption sent with the photo or clip
// ("VI1:79e6a5:14:10:46 Look at this"); envelopeCaption() finds it.
inline bool parseEnvelope(const char* text, Envelope* env) {
  if (!text) return false;
  while (*text == ' ' || *text == '\t' || *text == '\n' || *text == '\r') text++;
  Kind kind;
  if (strncmp(text, "VI1:", 4) == 0) kind = kImage;
  else if (strncmp(text, "VE3:", 4) == 0) kind = kCodec2;
  else if (strncmp(text, "VM1:", 4) == 0) kind = kMimi;
  else return false;
  const char* p = text + 4;
  uint32_t f[4];
  for (int k = 0; k < 4; k++) {
    const char* start = p;
    while (*p && *p != ':' && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
    if (!parseBase36(start, (size_t)(p - start), &f[k])) return false;
    if (k < 3) { if (*p != ':') return false; p++; }
  }
  // The last field ends at whitespace (a caption follows) or the end.
  if (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') return false;
  // The mode must belong to the prefix, as voice_wire.dart checks.
  if (kind == kImage && f[1] != (uint32_t)kImageModeId) return false;
  if (kind == kMimi && f[1] != (uint32_t)kMimiModeId) return false;
  if (kind == kCodec2 && f[1] > 6) return false;
  if (f[2] < 1 || f[2] > (uint32_t)kMaxPackets) return false;
  if (f[3] > 600) return false;   // VE3's duration range; photo aspect is clamped to 20..600
  env->sid = f[0];
  env->total = (uint8_t)f[2];
  env->aspect = (uint16_t)f[3];
  env->mode = (uint8_t)f[1];
  env->kind = kind;
  return true;
}

// The plain word after an envelope of `kind` (above); null for Codec2.
inline const char* kindLabel(Kind kind) {
  return kind == kImage ? "IMAGE" : kind == kMimi ? "VOICE" : nullptr;
}

// The caption sent with a photo or clip, past its kind's plain label: where it
// starts in `text` and, in `len`, how long it is without trailing whitespace.
// Null when `text` isn't an envelope; an empty string (len 0) when it has no
// caption.
inline const char* envelopeCaption(const char* text, size_t* len) {
  Envelope e;
  if (len) *len = 0;
  if (!parseEnvelope(text, &e)) return nullptr;
  const char* p = text;
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
  while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
  if (const char* label = kindLabel(e.kind)) {   // "IMAGE" / "VOICE": not part of the caption
    const size_t ln = strlen(label);
    if (strncmp(p, label, ln) == 0 && (!p[ln] || p[ln] == ' ' || p[ln] == '\t' || p[ln] == '\n' || p[ln] == '\r')) {
      p += ln;
      while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    }
  }
  size_t n = strlen(p);
  while (n && (p[n - 1] == ' ' || p[n - 1] == '\t' || p[n - 1] == '\n' || p[n - 1] == '\r')) n--;
  if (len) *len = n;
  return p;
}

// True for a well-formed VI1 photo envelope.
inline bool parseImageEnvelope(const char* text, Envelope* env) {
  Envelope e;
  if (!parseEnvelope(text, &e) || e.kind != kImage) return false;
  *env = e;
  return true;
}

struct Packet {
  uint32_t sid;
  uint8_t  index;
  const uint8_t* data;
  int      len;
  bool     marked;   // a marked packet (0x58): `left` of its answer still to come
  uint8_t  left;     // (63: that many or more)
};

inline uint32_t readSid(const uint8_t* b) {
  return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
}
inline void writeSid(uint32_t sid, uint8_t* b) {
  b[0] = (uint8_t)(sid >> 24); b[1] = (uint8_t)(sid >> 16); b[2] = (uint8_t)(sid >> 8); b[3] = (uint8_t)sid;
}

// Parses a data packet (plain or salted). `pkt->data` points into `buf`.
inline bool parsePacket(const uint8_t* buf, size_t len, Packet* pkt) {
  if (!buf || len == 0) return false;
  if (buf[0] != kPacketMagic && buf[0] != kPacketSaltedMagic && buf[0] != kPacketMarkedMagic) return false;
  const size_t end = buf[0] == kPacketMagic ? len : len - 1;
  if (end <= (size_t)kPacketHeaderLen) return false;
  pkt->sid = readSid(buf + 1);
  pkt->index = buf[5];
  pkt->data = buf + kPacketHeaderLen;
  pkt->len = (int)(end - kPacketHeaderLen);
  pkt->marked = buf[0] == kPacketMarkedMagic;
  pkt->left = pkt->marked ? (uint8_t)(buf[len - 1] & 0x3F) : 0;
  return pkt->index < kMaxPackets;
}

// Binary fetch request. `missing` == nullptr asks for every packet. Indices are
// written sorted and de-duplicated, capped at kMaxFetchIndices. `salt_ok` lets
// the sender salt resends (0x57). `reply_path` (requester -> sender, 1-byte
// hashes) asks the sender to answer along the reverse of the route the request
// took; pass nullptr to let it use its own route to us.
//
// `salt` >= 0 makes this request differ from an earlier one for the same
// packets: listed index (salt >> 4) % n is repeated (salt & 15) + 1 times (the
// app repeats one 1-8 times). Radios drop any packet whose payload they have
// already seen, so an unsalted retry for the same packets is byte-identical to
// the last and dies at the first repeater that relayed it (or at our own
// radio). Parsers read the indices as a set. Consecutive salts give distinct
// requests for 16 * n tries. With a salt, at most kSaltedFetchIndices are
// listed. An all-packets request has nothing to repeat and isn't salted.
// Returns bytes written, or 0 if `cap` is too small.
inline int encodeFetch(uint32_t sid, const uint8_t key6[6], const uint8_t* missing, int n_missing,
                       uint8_t* out, size_t cap, bool salt_ok = false,
                       const uint8_t* reply_path = nullptr, int reply_path_len = 0, int salt = -1,
                       bool mark_ok = false, bool fountain_ok = false) {
  uint8_t sorted[kMaxPackets];
  int n = 0;
  if (missing) {
    const int room = salt >= 0 ? kSaltedFetchIndices : kMaxFetchIndices;
    bool seen[kMaxPackets] = {false};
    for (int i = 0; i < n_missing; i++) if (missing[i] < kMaxPackets) seen[missing[i]] = true;
    for (int v = 0; v < kMaxPackets && n < room; v++) if (seen[v]) sorted[n++] = (uint8_t)v;
    if (salt >= 0 && n > 0) {
      const uint8_t again = sorted[((salt & 0xFF) >> 4) % n];
      for (int i = 0; i <= (salt & 15); i++) sorted[n++] = again;
    }
  }
  // A non-null path of length 0 is meaningful: the sender is our direct neighbour.
  const bool has_path = reply_path != nullptr && reply_path_len >= 0 &&
                        reply_path_len <= kMaxReplyPathBytes;
  const size_t need = (size_t)kFetchMinLen + (size_t)n + (has_path ? 1 + (size_t)reply_path_len : 0);
  if (cap < need) return 0;
  out[0] = kFetchMagic;
  writeSid(sid, out + 1);
  out[5] = (uint8_t)((missing ? kFetchFlagMissing : 0) | (salt_ok ? kFetchFlagSaltOk : 0) |
                     (has_path ? kFetchFlagHasPath : 0) | (mark_ok ? kFetchFlagMarkOk : 0) |
                     (fountain_ok ? kFetchFlagFountainOk : 0));
  memcpy(out + 6, key6, 6);
  out[12] = (uint8_t)n;
  if (n) memcpy(out + kFetchMinLen, sorted, (size_t)n);
  if (has_path) {
    out[kFetchMinLen + n] = (uint8_t)reply_path_len;
    if (reply_path_len) memcpy(out + kFetchMinLen + n + 1, reply_path, (size_t)reply_path_len);
  }
  return (int)need;
}

// A binary fetch request (data packets start 0x56 / 0x57).
inline bool isFetchRequest(const uint8_t* buf, size_t len) {
  return buf && len >= (size_t)kFetchMinLen && buf[0] == kFetchMagic;
}

// "VR3:" DMs are fetch requests between apps; they never belong in a chat.
inline bool isControlText(const char* text) { return text && strncmp(text, "VR3:", 4) == 0; }

// "VT1:" texts are an app's transfer-test reports (transfer_test.dart): by DM,
// or on a channel as "name: VT1:...". Never shown in a chat either.
inline bool isTransferReport(const char* text) {
  if (!text) return false;
  if (strncmp(text, "VT1:", 4) == 0) return true;
  const char* sep = strstr(text, ": ");
  return sep && strncmp(sep + 2, "VT1:", 4) == 0;
}

// VR3 text request. Ranges are written like the Dart side: "m1-3.7". Returns
// chars written (excluding NUL), or 0 if `cap` is too small.
inline int encodeTextFetch(uint32_t sid, const uint8_t key6[6], const uint8_t* missing, int n_missing,
                           char* out, size_t cap) {
  char buf[512];
  int pos = snprintf(buf, sizeof buf, "VR3:");
  pos += toBase36(sid, buf + pos);
  buf[pos++] = ':';
  bool seen[kMaxPackets] = {false};
  int n = 0;
  if (missing) for (int i = 0; i < n_missing; i++) if (missing[i] < kMaxPackets && !seen[missing[i]]) { seen[missing[i]] = true; n++; }
  if (n == 0) {
    buf[pos++] = 'a';
  } else {
    buf[pos++] = 'm';
    bool first = true;
    for (int v = 0; v < kMaxPackets; ) {
      if (!seen[v]) { v++; continue; }
      int e = v;
      while (e + 1 < kMaxPackets && seen[e + 1]) e++;
      if (!first) buf[pos++] = '.';
      first = false;
      pos += toBase36((uint32_t)v, buf + pos);
      if (e > v) { buf[pos++] = '-'; pos += toBase36((uint32_t)e, buf + pos); }
      v = e + 1;
      if (pos > (int)sizeof buf - 32) return 0;
    }
  }
  buf[pos++] = ':';
  for (int i = 0; i < 6; i++) pos += snprintf(buf + pos, sizeof buf - pos, "%02x", key6[i]);
  if ((size_t)pos + 1 > cap) return 0;
  memcpy(out, buf, (size_t)pos);
  out[pos] = '\0';
  return pos;
}

// A parsed fetch request, binary or VR3 text. `all` asks for every packet;
// otherwise `missing[0..n_missing)` (sorted, de-duplicated). `has_path` means
// `path[0..path_len)` is the route the request took (requester -> sender).
struct FetchRequest {
  uint32_t sid = 0;
  uint8_t  key6[6] = {0};
  bool     all = true;
  uint8_t  missing[kMaxPackets];
  int      n_missing = 0;
  bool     salt_ok = false;
  bool     has_path = false;
  uint8_t  path[kMaxReplyPathBytes];
  int      path_len = 0;
  bool     mark_ok = false;   // reads marked (0x58) packets
  bool     fountain_ok = false;   // rebuilds from repair packets (VoxFountain.h)
};

// Binary request (see encodeFetch). Like the Dart parser it drops indices
// >= 255 and ignores a reply path longer than kMaxReplyPathBytes.
inline bool parseFetch(const uint8_t* buf, size_t len, FetchRequest* r) {
  if (!r || !isFetchRequest(buf, len)) return false;
  *r = FetchRequest();
  r->sid = readSid(buf + 1);
  const uint8_t flags = buf[5];
  memcpy(r->key6, buf + 6, 6);
  const int count = buf[12];
  r->salt_ok = (flags & kFetchFlagSaltOk) != 0;
  r->mark_ok = (flags & kFetchFlagMarkOk) != 0;
  r->fountain_ok = (flags & kFetchFlagFountainOk) != 0;
  if (flags & kFetchFlagMissing) {
    if (len < (size_t)kFetchMinLen + (size_t)count) return false;
    bool seen[kMaxPackets] = {false};
    for (int i = 0; i < count; i++) {
      const uint8_t v = buf[kFetchMinLen + i];
      if (v < kMaxPackets) seen[v] = true;
    }
    for (int v = 0; v < kMaxPackets; v++) if (seen[v]) r->missing[r->n_missing++] = (uint8_t)v;
    r->all = false;
  }
  const size_t at = (size_t)kFetchMinLen + (size_t)count;
  if ((flags & kFetchFlagHasPath) && len > at) {
    const int n = buf[at];
    if (n <= kMaxReplyPathBytes && len >= at + 1 + (size_t)n) {
      r->has_path = true;
      r->path_len = n;
      if (n) memcpy(r->path, buf + at + 1, (size_t)n);
    }
  }
  return true;
}

// "VR3:{sid}:{a | m<ranges>}:{key6 hex}", as voice_wire.dart parses it.
inline bool parseTextFetch(const char* text, FetchRequest* r) {
  if (!text || !r) return false;
  while (*text == ' ' || *text == '\t' || *text == '\n' || *text == '\r') text++;
  if (strncmp(text, "VR3:", 4) != 0) return false;
  *r = FetchRequest();
  const char* p = text + 4;
  const char* q = p;
  while (*q && *q != ':') q++;
  if (*q != ':' || !parseBase36(p, (size_t)(q - p), &r->sid)) return false;
  p = q + 1;
  q = p;
  while (*q && *q != ':') q++;
  if (*q != ':') return false;
  const char* want_end = q;
  if (q - p == 1 && *p == 'a') {
    r->all = true;
  } else if (q - p > 1 && *p == 'm') {
    bool seen[kMaxPackets] = {false};
    const char* t = p + 1;
    while (t < want_end) {
      const char* e = t;
      while (e < want_end && *e != '.') e++;
      if (e > t) {
        const char* dash = t;
        while (dash < e && *dash != '-') dash++;
        uint32_t a, b;
        if (!parseBase36(t, (size_t)(dash - t), &a)) return false;
        if (dash < e) {
          const char* d2 = dash + 1;
          for (const char* c = d2; c < e; c++) if (*c == '-') return false;   // "1-2-3"
          if (!parseBase36(d2, (size_t)(e - d2), &b)) return false;
        } else {
          b = a;
        }
        if (a > b || b >= (uint32_t)kMaxPackets) return false;
        for (uint32_t v = a; v <= b; v++) seen[v] = true;
      }
      t = e < want_end ? e + 1 : e;
    }
    for (int v = 0; v < kMaxPackets; v++) if (seen[v]) r->missing[r->n_missing++] = (uint8_t)v;
    if (r->n_missing == 0) return false;
    r->all = false;
  } else {
    return false;
  }
  p = q + 1;
  for (int i = 0; i < 6; i++) {
    uint8_t v = 0;
    for (int k = 0; k < 2; k++) {
      char c = p[2 * i + k];
      if (c >= 'A' && c <= 'F') c = (char)(c - 'A' + 'a');
      int d;
      if (c >= '0' && c <= '9') d = c - '0';
      else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
      else return false;
      v = (uint8_t)(v << 4 | d);
    }
    r->key6[i] = v;
  }
  const char* tail = p + 12;
  while (*tail == ' ' || *tail == '\t' || *tail == '\n' || *tail == '\r') tail++;
  return *tail == '\0';
}

// The VE3 envelope for a Codec2 clip this radio recorded. Returns chars written
// (excluding NUL), or 0 if `cap` is too small or the fields are out of range.
inline int encodeVoiceEnvelope(uint32_t sid, int wire_mode, int total, int seconds, char* out, size_t cap) {
  if (wire_mode < 0 || wire_mode > 6 || total < 1 || total > kMaxPackets || seconds < 0 || seconds > 600)
    return 0;
  char buf[48];
  int n = snprintf(buf, sizeof buf, "VE3:");
  n += toBase36(sid, buf + n);
  buf[n++] = ':';
  n += toBase36((uint32_t)wire_mode, buf + n);
  buf[n++] = ':';
  n += toBase36((uint32_t)total, buf + n);
  buf[n++] = ':';
  n += toBase36((uint32_t)seconds, buf + n);
  if ((size_t)n + 1 > cap) return 0;
  memcpy(out, buf, (size_t)n);
  out[n] = '\0';
  return n;
}

// One data packet for a clip: `[0x56][sid][index][bytes]`, or with `salt` >= 0
// the salted form `[0x57]...[salt]`. Returns bytes written, or 0.
// With `left` >= 0 (and a salt), a marked packet (0x58): the trailing byte is
// (salt << 6) | min(left, 63), so `salt` should count this packet's sends.
inline int encodePacket(uint32_t sid, uint8_t index, const uint8_t* data, int len, int salt, uint8_t* out,
                        size_t cap, int left = -1) {
  const size_t need = (size_t)kPacketHeaderLen + (size_t)len + (salt >= 0 ? 1 : 0);
  if (len < 1 || cap < need) return 0;
  const bool mark = salt >= 0 && left >= 0;
  out[0] = salt < 0 ? kPacketMagic : mark ? kPacketMarkedMagic : kPacketSaltedMagic;
  writeSid(sid, out + 1);
  out[5] = index;
  memcpy(out + kPacketHeaderLen, data, (size_t)len);
  if (salt >= 0)
    out[need - 1] = mark ? (uint8_t)(((salt & 3) << 6) | (left < 63 ? left : 63)) : (uint8_t)salt;
  return (int)need;
}

// Collects one clip's packets into caller-owned storage of
// total * packet_bytes bytes (PSRAM on the device).
class Assembly {
 public:
  bool begin(uint32_t sid, uint8_t total, uint8_t* storage, size_t storage_len,
             int packet_bytes = kBytesPerPacket) {
    if (total < 1 || total > kMaxClipPackets) return false;
    if (packet_bytes < 1 || packet_bytes > kBytesPerPacket) return false;
    if (!storage || storage_len < (size_t)total * (size_t)packet_bytes) return false;
    sid_ = sid; total_ = total; buf_ = storage; last_len_ = -1; have_count_ = 0; pb_ = packet_bytes;
    memset(have_, 0, sizeof have_);
    return true;
  }

  // Returns true if the packet belonged to this photo and was new.
  bool add(const Packet& p) {
    if (!buf_ || p.sid != sid_ || p.index >= total_) return false;
    const bool last = p.index == total_ - 1;
    if (last ? (p.len < 1 || p.len > pb_) : p.len != pb_) return false;
    if (have(p.index)) return false;
    memcpy(buf_ + (size_t)p.index * (size_t)pb_, p.data, (size_t)p.len);
    if (last) last_len_ = p.len;
    have_[p.index >> 3] |= (uint8_t)(1u << (p.index & 7));
    have_count_++;
    return true;
  }

  bool have(uint8_t i) const { return (have_[i >> 3] >> (i & 7)) & 1; }
  bool complete() const { return buf_ && have_count_ == total_; }
  int  received() const { return have_count_; }
  uint8_t total() const { return total_; }
  uint32_t sid() const { return sid_; }

  // Missing indices into `out` (capacity kMaxPackets). Returns the count.
  int missing(uint8_t* out) const {
    int n = 0;
    for (int i = 0; i < total_; i++) if (!have((uint8_t)i)) out[n++] = (uint8_t)i;
    return n;
  }

  // The reassembled payload; valid once complete().
  const uint8_t* payload() const { return buf_; }
  size_t payloadLen() const {
    return complete() ? (size_t)(total_ - 1) * (size_t)pb_ + (size_t)last_len_ : 0;
  }

 private:
  uint32_t sid_ = 0;
  uint8_t  total_ = 0;
  uint8_t* buf_ = nullptr;
  int      last_len_ = -1;
  int      have_count_ = 0;
  int      pb_ = kBytesPerPacket;
  uint8_t  have_[(kMaxPackets + 7) / 8] = {0};
};

// The photo's AV1 OBUs inside a complete payload, or nullptr if this build
// can't read it.
inline const uint8_t* imageObus(const uint8_t* payload, size_t len, size_t* obu_len) {
  if (!payload || len < 2) return nullptr;
  size_t head;
  if (payload[0] == kImagePayloadVersion) head = 1;
  else if (payload[0] == kImagePayloadV2) head = 2 + (size_t)payload[1];
  else return nullptr;
  if (len <= head) return nullptr;
  *obu_len = len - head;
  return payload + head;
}

// The thumbnail's AV1 OBUs from the start of a payload (the first packet is
// enough), or nullptr when it has none or not all of it is in `prefix` yet.
inline const uint8_t* imageThumbObus(const uint8_t* prefix, size_t len, size_t* obu_len) {
  if (!prefix || len < 2 || prefix[0] != kImagePayloadV2 || !prefix[1]) return nullptr;
  if (len < 2 + (size_t)prefix[1]) return nullptr;
  *obu_len = prefix[1];
  return prefix + 2;
}

}  // namespace VoxWire

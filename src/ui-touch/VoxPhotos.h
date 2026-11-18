// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// MeshCore Open TAK photos and voice clips on this radio: wires VoxFetch
// (downloading) and VoxServe (answering downloads of clips recorded here) to the
// mesh (MyMesh), the internal filesystem (finished photos /vox_<sid>.vi1, voice
// clips /vox_<sid>.ve3, received or recorded) and the chat UI.
// Everything runs on the Arduino loop task, like the mesh and LVGL.

#include <stddef.h>
#include <stdint.h>

#include "VoxFetch.h"
#include "VoxWire.h"

namespace fs { class FS; }

namespace VoxPhotos {

// Where finished photos are kept (the internal filesystem; set once by the UI).
// `root`: the folder the files go in ("" for the filesystem's root).
void setStore(fs::FS* fs, const char* root = "");

// Where photos and clips go when internal storage is full: the SD card, in
// `root`. `mount` returns the card (mounting it on demand) or nullptr without
// one; it is called now, so files already there are found, and again (at most
// every 30 s) when a write doesn't fit and no card was found yet. Reads look
// in internal storage first, then here.
void setOverflow(fs::FS* (*mount)(), const char* root = "/meshcomod");

// Mesh hooks. onDirectText returns true for a VR3 fetch request (a control
// message between apps; it never belongs in the chat).
bool onDirectText(const char* text, const uint8_t sender_pub[32]);
void onChannelText(const char* text, int channel_idx);   // "Name: VI1:..." as posted
// raw (-1) or GRP_DATA 0x5643; `via` is the radio profile it arrived on
// (VoxFetch::kViaMain / kViaRadio2, or kViaUnknown).
// `hops`: how far a channel packet came (0 = heard directly; -1 unknown).
// `last_hop`: the path hash (1-3 bytes) of the repeater that relayed a flooded
// packet to us last, when it had one.
void onPayload(const uint8_t* data, size_t len, int channel_idx, uint8_t via = VoxFetch::kViaUnknown, int hops = -1,
               const uint8_t* last_hop = nullptr, uint8_t last_hop_len = 0);
void tick();

// UI side. `text` is a message body as shown (sender already split off).
bool parse(const char* text, VoxWire::Envelope* env);
// Registers what the bubble knows about the photo (after a reboot the mesh
// hooks never saw it) and starts or retries the download.
void request(const VoxWire::Envelope& env, const uint8_t* dm_pub, const char* sender_name,
             int channel_idx);
VoxFetch::Status status(uint32_t sid, int* received = nullptr, int* total = nullptr);
const char* statusText(uint32_t sid, char* buf, size_t cap);
// "Received via MediaMesh" etc. for a finished download, or nullptr when not known.
const char* viaText(uint32_t sid);
// The MediaMesh repeater the last radio2 packet of a finished download came
// through, as hex ("1ffb" for a 2-byte path hash). False when none did.
bool viaRepeater(uint32_t sid, char* hex, size_t cap);
// Whether this radio fetches the clip (photos; Codec2 voice where there's a
// speaker and the mode is built in) and whether it can play it.
bool canFetch(const VoxWire::Envelope& env);
bool canPlay(const VoxWire::Envelope& env);
// A packet of the Mimi clip `mimi_sid` was heard in the last kMimiQuietMs: a
// transfer of it is going on, and its Codec2 copy waits for that to finish.
constexpr uint32_t kMimiQuietMs = 5000;
bool mimiBusy(uint32_t mimi_sid);
// A finished photo's payload ([0x01][OBUs]); free with free().
bool load(uint32_t sid, uint8_t** data, size_t* len);
// A finished voice clip: its VE3 wire mode and codec frames; free with free().
bool loadVoice(uint32_t sid, int* mode, uint8_t** frames, size_t* len);
bool loadFile(uint32_t sid, bool voice, uint8_t** data, size_t* len);
// Bumped whenever any photo's status changes; the chat redraws on a change.
uint32_t version();

// The newest photo, and the newest voice message or talk, announced to this
// radio since it started (Home's Latest photo and Voice widgets). RAM only.
struct Latest {
  VoxWire::Envelope env;
  bool talk = false;           // a push-to-talk (caption "PTT")
  uint32_t at_ms = 0;          // when it was announced (millis)
  char name[32] = {0};         // a channel post's author
  uint8_t pub[32] = {0};       // a DM's sender
  bool has_pub = false;
  int channel = -1;            // the channel it was posted on, or -1 for a DM
};
bool latest(bool voice, Latest* out);
// Filesystem lookups made for photo/voice state since boot (About's chat-open timing).
uint32_t fsChecks();
// While on, "is it stored" questions never wait on the filesystem: before the
// store's index is ready, what isn't known yet counts as not stored. The chat
// turns it on while it lays out and builds bubbles.
void setQuick(bool on);

// Fast MediaMesh (prototype): ask for and answer with repair packets
// (VoxFountain.h), and pace flooded answers closer (FloodPace.h).
void setFast(bool on);
bool fast();
// Moves when answers that change a bubble's height may have changed (the index
// landed): the open chat is laid out again.
uint32_t layoutVersion();
// Bumped only when a photo or talk changes state (requesting, complete,
// failed, recorded), not for each packet of one arriving.
uint32_t stateVersion();

// ---- Voice clips recorded on this radio --------------------------------------
// Stores a clip (taking ownership of `bits`, a malloc'd buffer), makes it
// available to anyone who asks for it, and writes its VE3 envelope (the text
// to post in the chat) to `envelope`. False if it couldn't be stored.
// `want_sid` keeps a session id already used on the air (push-to-talk); 0 picks one.
bool addRecording(uint8_t wire_mode, uint8_t* bits, size_t len, uint32_t ms, char* envelope, size_t cap,
                  uint32_t* sid_out = nullptr, uint32_t want_sid = 0);
// Keeps a talk heard live in full (push-to-talk), so its voice message plays at
// once when it's announced, with nothing to download. False if not stored.
bool storeHeard(uint32_t sid, uint8_t wire_mode, const uint8_t* bits, size_t len);
// Deletes a recording made with addRecording that was never sent.
void discardRecording(uint32_t sid);
// Whether a voice clip with this session id is stored here (recorded or
// downloaded), so the bubble can play it.
bool haveVoice(uint32_t sid);

// A received photo's thumbnail, kept beside it (/vox_<sid>.vt1) once the photo
// has been decoded, for the chat bubble. At most kThumbMax pixels a side.
constexpr int kThumbMax = 96;
// Its size, without reading the pixels (remembered). False if there is none.
bool thumbSize(uint32_t sid, int* w, int* h);
// Its pixels (RGB565, w*h), which the caller frees with free().
bool loadThumb(uint32_t sid, int* w, int* h, uint16_t** px);
// Scales `src` (RGB565, sw x sh) down to fit kThumbMax and stores it.
bool saveThumb(uint32_t sid, const uint16_t* src, int sw, int sh);
// Asks for a photo's thumbnail to be made (a bubble showed one without). Each
// finished photo download asks on its own. The UI takes them one at a time
// (0 when none), decoding in the background.
void requestThumb(uint32_t sid);

// The photos stored on this radio (downloaded ones), newest first by file time
// (directory order where the filesystem keeps none), at most `cap`.
struct StoredPhoto { uint32_t sid; uint32_t mtime; uint32_t bytes; };
int listPhotos(StoredPhoto* out, int cap);
// Removes a stored photo and its thumbnail.
bool deletePhoto(uint32_t sid);
// The VI1 envelope that sends a stored photo: post it as a message and this
// radio serves the photo to whoever asks (it becomes ours to serve). False
// when the photo isn't stored or isn't a photo payload.
bool photoEnvelope(uint32_t sid, char* out, size_t cap);
// Stores a photo payload ([0x01][AV1 OBUs], a .vi1 file) under a new sid, so
// it can be sent and appears in Photos.
bool importPhoto(const uint8_t* data, size_t len, uint32_t* sid_out);
uint32_t nextThumbWanted();

// The thumbnail a version 2 photo carries in its first packets, as a payload
// VoxAv1Job decodes ([0x01][its OBUs]), once those are here and the photo
// isn't. False when there's none (yet).
bool earlyThumbPayload(uint32_t sid, uint8_t* out, size_t cap, size_t* len);
// Stores that thumbnail (up to 96 px, scaled up to the bubble's size). The
// photo's own thumbnail replaces it when the photo is in.
bool saveEarlyThumb(uint32_t sid, const uint16_t* src, int sw, int sh);
// A thumbnail that's the photo's own: there is one, and it isn't the early one
// of a photo since completed.
bool thumbFinal(uint32_t sid);

}  // namespace VoxPhotos

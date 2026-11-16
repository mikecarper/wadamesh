// SPDX-License-Identifier: GPL-3.0-or-later
// ---- Linked pictures ---------------------------------------------------------------
// Someone posts a link to a picture (https://.../cat.jpg, .png, .gif, .webp) in a
// channel or a DM. With Settings > General > Linked images on, the radio downloads it
// over Wi-Fi and the bubble shows it above the text; a tap on it opens it full size.
// On by default (fetching a link tells that server this radio's address: turn it off
// to keep that). The saved value is "linkimg2", so radios that saved the old default
// (off) get the new one.
//
// One picture at a time, on a worker (the shared audio worker when it's free, as the
// web reader does: TLS needs its 16 KB stack in internal RAM), never the UI loop:
//   - get:  download (at most kLiMaxBytes), keep the file on the SD card
//           (/linkimg/<id>.img) when there is one, decode it small for the bubble;
//   - view: the kept file (or download it again), decoded to fit kLiViewSide.
// The bubble's thumbnail goes in the photo store beside the mesh photos' (VoxPhotos
// saveThumb, under the link's id), so the chat lays it out the same way.
// Decoding: helpers/LinkImage (stb_image, simplewebp). A JPEG too big for that in
// this radio's memory is decoded scaled (TJpgDec) on the UI side instead.
// Included in UITask.cpp after ChatBubbleDisplay.

#if CAP_LINK_IMAGES
#include "../helpers/LinkImage.h"
#include "GiphyText.h"

static fs::FS* backupSdFs();   // (with the Backups page) the SD card, mounted on demand

#if defined(HAS_TDISPLAY_P4) || defined(HAS_CROWPANEL_P4_101) || defined(HAS_TANMATSU)
static constexpr size_t kLiMaxBytes = 8u << 20;   // 32 MB of PSRAM
#else
static constexpr size_t kLiMaxBytes = 3u << 20;
#endif
static constexpr int kLiViewSide = 1024;                      // the viewer's own limit
// The width asked of hosts that make a smaller copy (Pexels, Unsplash): twice a
// small screen is plenty, and a third of the pixels to download and decode.
#if defined(HAS_TDISPLAY_P4) || defined(HAS_CROWPANEL_P4_101) || defined(HAS_TANMATSU)
static constexpr int kLiFetchSide = 1024;
#else
static constexpr int kLiFetchSide = 640;
#endif
static constexpr int kLiThumbSide = 2 * VoxPhotos::kThumbMax;  // saveThumb averages it down
static constexpr size_t kLiKeepFree = 1536u * 1024u;          // PSRAM left for everything else
// GIFs (GiphyText.h): the player's frames, at most this many, this big a side, this
// many bytes in all.
static constexpr int kGifMaxFrames = 150;
static constexpr int kGifSide = 200;
static constexpr size_t kGifMaxBytes = 4u << 20;
static volatile bool s_gif_busy = false;   // the GIF picker is using the network: links wait
// A GIF in its bubble: thumbnail-sized frames, a few GIFs' worth kept.
static constexpr int kGifBubbleFrames = 48;
static constexpr size_t kGifBubbleBytes = 512u * 1024u;   // ~36 frames at 96 x 72
static void gifAnimTrim(uint32_t idle_ms = 0);   // frees the bubble GIFs nothing shows (below)

// Settings > General > Linked images.
static int8_t s_li_on = -1;
static bool linkImgOn() {
  if (s_li_on < 0) {
    uint8_t v = 1;
    s_li_on = (touchPrefsGetBlob("linkimg2", &v, 1) == 1 && !v) ? 0 : 1;
  }
  return s_li_on == 1;
}
static void linkImgToggleCb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
  const bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
  s_li_on = on ? 1 : 0;
  const uint8_t v = on ? 1 : 0;
  touchPrefsSetBlob("linkimg2", &v, 1);
  ++s_chat_thumb_gen;   // the open chat lays out again: pictures in or out
  g_lv.dirty_timeline = true;
}

// Settings > General > Play GIFs in chat: on unless switched off.
static int8_t s_gif_play = -1;
static bool gifPlayOn() {
  if (s_gif_play < 0) {
    uint8_t v = 1;
    s_gif_play = (touchPrefsGetBlob("gifplay", &v, 1) == 1 && !v) ? 0 : 1;
  }
  return s_gif_play == 1;
}
static void gifPlayToggleCb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
  const uint8_t v = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED) ? 1 : 0;
  s_gif_play = (int8_t)v;
  touchPrefsSetBlob("gifplay", &v, 1);
  ++s_chat_thumb_gen;   // the open chat draws its bubbles again
  g_lv.dirty_timeline = true;
}

static bool linkImgWifiUp() { return WiFi.status() == WL_CONNECTED && (uint32_t)WiFi.localIP() != 0; }

// Links waiting to be fetched (newest last), and the ones tried since boot.
struct LiWant { uint32_t id; char url[240]; };
static LiWant   s_li_want[6];
static uint8_t  s_li_want_n = 0;
static uint32_t s_li_tried[64];
static uint8_t  s_li_tried_at = 0;
static uint32_t s_li_retry_ms = 0;   // a failure with no Wi-Fi: not before this

static bool linkImgTried(uint32_t id) {
  for (uint32_t t : s_li_tried) if (t == id) return true;
  return false;
}
static void linkImgWant(uint32_t id, const char* url) {
  if (linkImgTried(id)) return;
  for (uint8_t i = 0; i < s_li_want_n; i++) if (s_li_want[i].id == id) return;
  if (s_li_want_n == sizeof s_li_want / sizeof s_li_want[0]) {   // full: the oldest goes
    memmove(&s_li_want[0], &s_li_want[1], sizeof s_li_want[0] * (s_li_want_n - 1));
    --s_li_want_n;
  }
  s_li_want[s_li_want_n].id = id;
  snprintf(s_li_want[s_li_want_n].url, sizeof s_li_want[0].url, "%s", url);
  ++s_li_want_n;
}

// The job on the worker. state: 0 free, 1 running, 2 done (the UI collects it).
struct LiJob {
  volatile uint8_t state = 0;
  bool view = false;
  uint32_t id = 0;
  char url[240] = "";
  char title[48] = "";
  fs::FS* sd = nullptr;   // where files are kept (nullptr: no card)
  // Results.
  linkimg::Result res = linkimg::kOk;
  int http = 0;           // the HTTP status, or < 0 without a connection; 0 read from the card
  uint16_t* px = nullptr; // view: lvglPsramAlloc'd, else malloc'd
  int w = 0, h = 0;
  uint8_t* raw = nullptr; // a JPEG too big to decode here: the UI tries it scaled
  size_t raw_len = 0;
  bool progressive = false;   // ...and the scaled decoder can't do it (progressive)
  // A GIF to play (anim): all its frames, w*h each, lvglPsramAlloc'd.
  bool anim = false;
  int frames = 0;
  uint16_t delays[kGifMaxFrames] = {};
};
static LiJob s_li_job;

class LiSink final : public Stream {
 public:
  LiSink(uint8_t* d, size_t cap) : d_(d), cap_(cap) {}
  using Print::write;
  size_t write(uint8_t b) override { return write(&b, 1); }
  size_t write(const uint8_t* d, size_t n) override {
    const size_t room = n_ < cap_ ? cap_ - n_ : 0;
    if (n > room) over_ = true;
    const size_t k = n < room ? n : room;
    memcpy(d_ + n_, d, k);
    n_ += k;
    return k;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
  size_t size() const { return n_; }
  bool over() const { return over_; }

 private:
  uint8_t* d_;
  size_t cap_;
  size_t n_ = 0;
  bool over_ = false;
};

// GET `url` into buf (at most cap bytes; `over` when it had more). The HTTP status,
// or < 0 without a connection. On a worker: TLS wants its stack.
// With `*pbuf` null, the buffer is allocated here (PSRAM) once the answer says how
// big it is, at that size: a 300 KB GIF takes 300 KB, not the cap. That 3 MB block
// up front was what "Out of memory" meant on a T-Deck busy with chats, emoji and
// GIFs. A failed allocation is -2.
static int liGet(const char* url, uint8_t** pbuf, size_t cap, size_t* len, bool* over, const char* accept) {
  *len = 0;
  *over = false;
  HTTPClient http;
  http.setReuse(false);
  http.setConnectTimeout(9000);
  http.setTimeout(15000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setUserAgent("wadamesh");
  WiFiClientSecure scli;
  WiFiClient pcli;
  const bool https = !strncasecmp(url, "https://", 8);
  bool began;
  if (https) { scli.setInsecure(); began = http.begin(scli, url); }
  else began = http.begin(pcli, url);
  int code = -1;
  if (began) {
    if (accept) http.addHeader("Accept", accept);
    http.addHeader("Accept-Encoding", "identity");
    code = http.GET();
    if (code == HTTP_CODE_OK) {
      const int clen = http.getSize();
      if (clen > 0 && (size_t)clen > cap) {
        *over = true;
      } else if (!*pbuf && !(*pbuf = (uint8_t*)heap_caps_malloc(clen > 0 ? (size_t)clen : cap,
                                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT))) {
        code = -2;
      } else {
        if (clen > 0) cap = (size_t)clen;
        LiSink sink(*pbuf, cap);
        http.writeToStream(&sink);
        *len = sink.size();
        *over = sink.over();
      }
    }
  }
  http.end();
  return code;
}

// .im2: files kept before Pexels links asked for a baseline JPEG may be the
// progressive copy this can't open; they're left behind, not reopened.
static void linkImgPath(uint32_t id, char* p, size_t cap) {
  snprintf(p, cap, "/linkimg/%08lx.im2", (unsigned long)id);
}

static void* linkImgThumbAlloc(size_t n) { return malloc(n); }
static void* linkImgViewAlloc(size_t n) { return lvglPsramAlloc(n); }

// On the worker: no LVGL here.
static void linkImgJobBody() {
  LiJob& j = s_li_job;
  uint8_t* raw = nullptr;
  size_t len = 0;
  char path[40];
  linkImgPath(j.id, path, sizeof path);

  if (j.view && j.sd && j.sd->exists(path)) {   // kept from when it was fetched
    File f = j.sd->open(path, "r");
    const size_t n = f ? f.size() : 0;
    if (n && n <= kLiMaxBytes && (raw = (uint8_t*)heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)))
      len = f.read(raw, n);
    if (f) f.close();
    if (raw && len != n) { free(raw); raw = nullptr; len = 0; }
    j.http = 0;
  }
  if (!raw) {
    char fetch[sizeof j.url + 64];
    if (!linkimg::fetchUrl(j.url, kLiFetchSide, fetch, sizeof fetch)) snprintf(fetch, sizeof fetch, "%s", j.url);
    bool over = false;
    const int code = liGet(fetch, &raw, kLiMaxBytes, &len, &over,
                           "image/jpeg,image/png,image/gif,image/webp;q=0.9,*/*;q=0.5");
    if (code == -2) { j.res = linkimg::kNoMemory; j.state = 2; return; }
    j.http = code;
    if (over) { free(raw); j.res = linkimg::kTooLarge; j.state = 2; return; }
    if (code != HTTP_CODE_OK || !len) { free(raw); j.res = linkimg::kBroken; j.state = 2; return; }
    if (linkimg::sniff(raw, len) == linkimg::kUnknown) { free(raw); j.res = linkimg::kNotPicture; j.state = 2; return; }
    if (uint8_t* fit = (uint8_t*)heap_caps_realloc(raw, len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) raw = fit;
    if (j.sd) {   // keep it for viewing
      j.sd->mkdir("/linkimg");
      File f = j.sd->open(path, "w");
      if (f) {
        const bool ok = f.write(raw, len) == len;
        f.close();
        if (!ok) j.sd->remove(path);
      }
    }
  }

  const size_t free_ps = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  const size_t budget = free_ps > kLiKeepFree ? free_ps - kLiKeepFree : 0;
  if (j.anim) {   // the player's (view), or a bubble's: thumbnail-sized, fewer
    const size_t cap = j.view ? kGifMaxBytes : kGifBubbleBytes;
    j.res = linkimg::decodeFrames(raw, len, j.view ? kGifSide : VoxPhotos::kThumbMax, budget < cap ? budget : cap,
                                  j.view ? 0x0000 : 0x2104, j.view ? kGifMaxFrames : kGifBubbleFrames,
                                  lvglPsramRealloc, &j.px, &j.w, &j.h, &j.frames, j.delays);
    free(raw);
    j.state = 2;
    return;
  }
  j.res = linkimg::decode(raw, len, j.view ? kLiViewSide : kLiThumbSide, budget, 0x2104,
                          j.view ? linkImgViewAlloc : linkImgThumbAlloc, &j.px, &j.w, &j.h);
  if (j.res == linkimg::kTooLarge && linkimg::sniff(raw, len) == linkimg::kJpeg) {
    j.raw = raw;   // the UI decodes it scaled
    j.raw_len = len;
  } else {
    free(raw);
  }
  j.state = 2;
}
// At idle priority, as the photo decoder (VoxAv1Job.cpp) runs: a TLS download and
// a GIF's decode take seconds, and at the audio worker's priority (3, unpinned)
// they starved the UI loop (priority 1) for all of it. A T-Deck that got a GIF
// froze until it was done, and the GIF never seemed to play.
static void linkImgJobTaskFn(void*) { linkImgJobBody(); vTaskDelete(nullptr); }
static void linkImgJobWorkerFn(void*) {
  const UBaseType_t prio = uxTaskPriorityGet(nullptr);
  vTaskPrioritySet(nullptr, tskIDLE_PRIORITY);
  linkImgJobBody();
  vTaskPrioritySet(nullptr, prio);
}

static bool linkImgStart(bool view, uint32_t id, const char* url, const char* title, bool anim = false) {
  LiJob& j = s_li_job;
  if (j.state != 0 || s_gif_busy) return false;
  if (view || anim) gifAnimTrim();   // a GIF's frames take megabytes: hand back what's off screen
  j = LiJob();
  j.view = view;
  j.anim = anim;
  j.id = id;
  snprintf(j.url, sizeof j.url, "%s", url ? url : "");
  snprintf(j.title, sizeof j.title, "%s", title ? title : "");
  j.sd = backupSdFs();
  j.state = 1;
  if (!VoxAudioTask::run(linkImgJobWorkerFn, nullptr) &&
      xTaskCreatePinnedToCore(linkImgJobTaskFn, "linkimg", 16384, nullptr, tskIDLE_PRIORITY, nullptr, 0) != pdPASS) {
    j.state = 0;
    return false;
  }
  return true;
}

static const char* linkImgWhy(const LiJob& j) {
  if (j.progressive) return "Progressive JPEG, too big to open here";
  if (j.http < 0) return "No connection to the picture's server";
  if (j.http > 0 && j.http != HTTP_CODE_OK) return "The picture's server said no";
  return linkimg::resultText(j.res);
}

// Opens a linked picture full size: from the card, or downloaded now.
static void linkImgView(const char* url_in) {
  if (!url_in || !url_in[0] || !g_lv.task) return;
  // A GIF's still (the bubble's) plays: its animated copy, from the card or the net.
  char gid[giphy::kIdMax], anim_url[160];
  const bool anim = giphy::idOfMediaUrl(url_in, gid, sizeof gid) &&
                    giphy::mediaUrl(gid, "200w", anim_url, sizeof anim_url);
  const char* url = anim ? anim_url : url_in;
  char path[40];
  const uint32_t id = linkimg::urlId(url);
  linkImgPath(id, path, sizeof path);
  fs::FS* sd = backupSdFs();
  if (!(sd && sd->exists(path)) && !linkImgWifiUp()) {
    g_lv.task->showAlert(TR("Wi-Fi not connected (Settings \xe2\x86\x92 Wi-Fi)"), 2000);
    return;
  }
  const char* slash = strrchr(url, '/');
  char title[48];
  snprintf(title, sizeof title, "%s", slash && slash[1] ? slash + 1 : "picture");
  if (char* q = strpbrk(title, "?#")) *q = 0;
  if (anim) snprintf(title, sizeof title, "GIF");
  if (!linkImgStart(true, id, url, title, anim)) {
    g_lv.task->showAlert(TR("Busy, try again in a moment"), 1500);
    return;
  }
  g_lv.task->showAlert(anim ? TR("Opening GIF...") : TR("Opening picture..."), 1500);
}

// ---- The GIF player: a GIF's frames, looped, full screen; Back closes it ----------
static lv_obj_t* s_gp_root = nullptr;
static lv_obj_t* s_gp_img = nullptr;
static lv_timer_t* s_gp_timer = nullptr;
static lv_img_dsc_t s_gp_dsc;
static uint16_t* s_gp_px = nullptr;
static int s_gp_w = 0, s_gp_h = 0, s_gp_n = 0, s_gp_at = 0;
static uint16_t s_gp_delays[kGifMaxFrames];
// A preview from the GIF picker: the GIF it plays, and a Send button under it.
static char s_gp_send[giphy::kIdMax] = "";
static void gifSendId(const char* id);

static void gifPlayerClose() {
  s_gp_send[0] = 0;
  settingsSubPageClose(gifPlayerClose);
  if (s_gp_timer) { lv_timer_del(s_gp_timer); s_gp_timer = nullptr; }
  if (s_gp_root) {
    lv_obj_t* r = s_gp_root;
    s_gp_root = s_gp_img = nullptr;
    lv_obj_add_flag(r, LV_OBJ_FLAG_HIDDEN);   // never drawn again: its frames are freed below
    lv_obj_del_async(r);
  }
  lv_img_cache_invalidate_src(&s_gp_dsc);
  if (s_gp_px) { lvglPsramFree(s_gp_px); s_gp_px = nullptr; }
}

static void gifPlayerStep(lv_timer_t* t) {
  if (!s_gp_img || s_gp_n < 2) return;
  s_gp_at = (s_gp_at + 1) % s_gp_n;
  s_gp_dsc.data = (const uint8_t*)(s_gp_px + (size_t)s_gp_at * s_gp_w * s_gp_h);
  lv_img_cache_invalidate_src(&s_gp_dsc);
  lv_obj_invalidate(s_gp_img);
  lv_timer_set_period(t, s_gp_delays[s_gp_at]);
}

// Takes the frames (lvglPsramAlloc'd) and plays them.
static void gifPlayerShow(uint16_t* frames, int w, int h, int n, const uint16_t* delays_ms) {
  char send[giphy::kIdMax];
  snprintf(send, sizeof send, "%s", s_gp_send);
  gifPlayerClose();
  snprintf(s_gp_send, sizeof s_gp_send, "%s", send);
  s_gp_px = frames;
  s_gp_w = w;
  s_gp_h = h;
  s_gp_n = n;
  s_gp_at = 0;
  memcpy(s_gp_delays, delays_ms, sizeof(uint16_t) * (size_t)n);
  memset(&s_gp_dsc, 0, sizeof s_gp_dsc);
  s_gp_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
  s_gp_dsc.header.w = (uint32_t)w;
  s_gp_dsc.header.h = (uint32_t)h;
  s_gp_dsc.data_size = (uint32_t)w * (uint32_t)h * 2u;
  s_gp_dsc.data = (const uint8_t*)frames;
  s_gp_root = settingsSubPageOpen("GIF", gifPlayerClose);
  lv_obj_set_style_bg_color(s_gp_root, lv_color_black(), LV_PART_MAIN);
  if (!s_gp_send[0])   // a tap closes it; a preview waits for Send or Back
    lv_obj_add_event_cb(s_gp_root, [](lv_event_t*) { gifPlayerClose(); }, LV_EVENT_CLICKED, nullptr);
  s_gp_img = lv_img_create(s_gp_root);
  lv_img_set_src(s_gp_img, &s_gp_dsc);
  lv_obj_clear_flag(s_gp_img, LV_OBJ_FLAG_CLICKABLE);
  // As big as the page holds, up to three times its size.
  const lv_coord_t pw = lv_obj_get_width(s_gp_root), ph = lv_obj_get_height(s_gp_root);
  uint32_t zoom = (uint32_t)LV_MIN((int64_t)pw * 256 / w, (int64_t)ph * 256 / h);
  if (zoom > 768) zoom = 768;
  lv_img_set_zoom(s_gp_img, (uint16_t)LV_MAX(64u, zoom));
  lv_img_set_antialias(s_gp_img, false);
  lv_obj_center(s_gp_img);
  if (n > 1) s_gp_timer = lv_timer_create(gifPlayerStep, delays_ms[0], nullptr);
  if (s_gp_send[0]) {
    lv_obj_t* b = lv_btn_create(s_gp_root);
    stylePrimary(b);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_align(b, LV_ALIGN_BOTTOM_MID, 0, -SC(8));
    lv_obj_add_event_cb(b, [](lv_event_t*) {
      char id[giphy::kIdMax];
      snprintf(id, sizeof id, "%s", s_gp_send);
      gifPlayerClose();
      gifSendId(id);
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* l = lv_label_create(b);
    lv_label_set_text(l, LV_SYMBOL_OK "  " "Send");
    lv_obj_center(l);
  }
}

// ---- The GIF picker: Giphy's trending GIFs, or a search; one sends it ------------
// Opened from the chat's emoji picker ("GIF"). A GIF goes as "g:<id>", the way
// MeshCore Open sends one, so the app shows it too. The search and the grid's
// stills come over Wi-Fi on a worker, one at a time, while nothing else downloads.
// The key is the one MeshCore Open ships (lib/widgets/gif_picker.dart).
static constexpr const char* kGiphyKey = "sXpGFDGZs0Dv1mmNFvYaGUvYwKX0PWIh";
static constexpr int kGifResults = 12;
static constexpr size_t kGifJsonMax = 384u * 1024u;
static constexpr size_t kGifStillMax = 96u * 1024u;

struct GifSearch {
  volatile uint8_t state = 0;   // 0 free, 1 searching, 2 stills coming, 3 done
  volatile bool cancel = false;
  char q[64] = "";
  int side = 90;                // the grid's cell: stills fit it
  char ids[kGifResults][giphy::kIdMax] = {};
  volatile int n = 0;
  int http = 0;
  uint16_t* px[kGifResults] = {};   // malloc'd; the grid takes them
  int w[kGifResults] = {}, h[kGifResults] = {};
  volatile uint8_t got[kGifResults] = {};   // 1 here, 2 failed
  bool resume = false;                      // fetch the stills a preview stopped, not search again
};
static GifSearch s_gs;

static void gifSearchBody() {
  GifSearch& g = s_gs;
  char url[320], esc[160];
  uint8_t* buf = (uint8_t*)heap_caps_malloc(kGifJsonMax, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  size_t len = 0;
  bool over = false;
  int n = g.n;
  if (g.resume) goto stills;   // after a preview: the stills it stopped
  if (g.q[0] && giphy::queryEscape(g.q, esc, sizeof esc))
    snprintf(url, sizeof url, "https://api.giphy.com/v1/gifs/search?api_key=%s&q=%s&limit=%d&rating=g", kGiphyKey,
             esc, kGifResults);
  else
    snprintf(url, sizeof url, "https://api.giphy.com/v1/gifs/trending?api_key=%s&limit=%d&rating=g", kGiphyKey,
             kGifResults);
  g.http = buf ? liGet(url, &buf, kGifJsonMax, &len, &over, "application/json") : -1;
  n = 0;
  if (g.http == HTTP_CODE_OK && len) n = giphy::idsInJson((const char*)buf, len, g.ids, kGifResults);
  g.n = n;
stills:
  __sync_synchronize();
  g.state = 2;
  for (int i = 0; i < n && !g.cancel; ++i) {
    if (g.got[i]) continue;
    char su[160];
    uint8_t ok = 2;
    if (buf && giphy::mediaUrl(g.ids[i], "100w_s", su, sizeof su) &&
        liGet(su, &buf, kGifStillMax, &len, &over, "image/gif") == HTTP_CODE_OK && len && !over) {
      uint16_t* px = nullptr;
      int w = 0, h = 0;
      if (linkimg::decode(buf, len, g.side, 1u << 20, 0x2104, linkImgThumbAlloc, &px, &w, &h) == linkimg::kOk) {
        g.px[i] = px;
        g.w[i] = w;
        g.h[i] = h;
        ok = 1;
      }
    }
    __sync_synchronize();
    g.got[i] = ok;
  }
  free(buf);
  __sync_synchronize();
  g.state = 3;
  s_gif_busy = false;
}
// Idle priority too (see linkImgJobWorkerFn): it downloads and decodes a page of
// thumbnails.
static void gifSearchTaskFn(void*) { gifSearchBody(); vTaskDelete(nullptr); }
static void gifSearchWorkerFn(void*) {
  const UBaseType_t prio = uxTaskPriorityGet(nullptr);
  vTaskPrioritySet(nullptr, tskIDLE_PRIORITY);
  gifSearchBody();
  vTaskPrioritySet(nullptr, prio);
}

static lv_obj_t* s_gs_sheet = nullptr;
static lv_obj_t* s_gs_ta = nullptr;
static lv_obj_t* s_gs_grid = nullptr;
static lv_obj_t* s_gs_note = nullptr;
static lv_timer_t* s_gs_timer = nullptr;
static uint8_t s_gs_shown[kGifResults];    // 1 the cell is there, 2 and its still
static bool s_gs_listed = false;
static bool s_gs_again = false;            // the text changed while a search ran
static uint32_t s_gs_typed_ms = 0;         // searched this long after the last key
static lv_coord_t s_gs_cell = 90;

static void gifStillFreeCb(lv_event_t* e) {
  lv_img_dsc_t* d = (lv_img_dsc_t*)lv_event_get_user_data(e);
  if (!d) return;
  lv_img_cache_invalidate_src(d);
  free((void*)d->data);
  free(d);
}

// Starts a search for what's typed (trending when nothing is). False while the
// network is busy; the poll tries again.
static int s_gs_preview = -1;   // a GIF tapped: played (with Send) once the network is free
static bool s_gs_noted = false;  // the note says "Opening GIF...": put it back after

static bool gifWorkerStart() {
  s_gif_busy = true;
  s_gs.state = 1;
  if (!VoxAudioTask::run(gifSearchWorkerFn, nullptr) &&
      xTaskCreatePinnedToCore(gifSearchTaskFn, "gifsearch", 16384, nullptr, tskIDLE_PRIORITY, nullptr, 0) != pdPASS) {
    s_gs.state = 0;
    s_gif_busy = false;
    return false;
  }
  return true;
}

static bool gifSearchStart() {
  GifSearch& g = s_gs;
  if (g.state == 1 || g.state == 2 || s_li_job.state != 0) return false;
  g.resume = false;
  s_gs_preview = -1;
  for (int i = 0; i < kGifResults; ++i) { free(g.px[i]); g.px[i] = nullptr; }   // ones a closed sheet left
  const char* q = s_gs_ta ? lv_textarea_get_text(s_gs_ta) : "";
  g.cancel = false;
  snprintf(g.q, sizeof g.q, "%s", q ? q : "");
  g.side = s_gs_cell - 6;
  g.n = 0;
  memset((void*)g.got, 0, sizeof g.got);
  memset(s_gs_shown, 0, sizeof s_gs_shown);
  s_gs_listed = false;
  if (!gifWorkerStart()) return false;
  if (s_gs_grid) lv_obj_clean(s_gs_grid);
  if (s_gs_note) lv_label_set_text(s_gs_note, TR("Searching..."));
  return true;
}

static void gifPickerClose() {
  if (!s_gs_sheet) return;
  s_gs.cancel = true;   // the worker stops after the still it's on
  if (s_gs_timer) { lv_timer_del(s_gs_timer); s_gs_timer = nullptr; }
  hideKb();
  popupClose(&s_gs_sheet);
  s_gs_ta = s_gs_grid = s_gs_note = nullptr;
}

static void gifSendId(const char* id) {
  char msg[8 + giphy::kIdMax];
  const bool ok = giphy::message(id, msg, sizeof msg);
  gifPickerClose();
  if (ok && g_lv.task && g_lv.task->composerSend(msg)) g_lv.dirty_timeline = true;   // composerSend says why not
}

// A tap previews: the stills stop, the GIF plays full size with Send under it.
static void gifSendCb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  const int i = (int)(intptr_t)lv_event_get_user_data(e);
  if (i < 0 || i >= s_gs.n) return;
  lv_indev_t* a = lv_indev_get_act();
  if (a) lv_indev_wait_release(a);
  s_gs.cancel = true;
  s_gs_preview = i;
  s_gs_noted = true;
  if (s_gs_note) lv_label_set_text(s_gs_note, TR("Opening GIF..."));
}

// Every 300 ms while open: a pending search, then the results and stills as they land.
static void gifPickerPoll(lv_timer_t*) {
  GifSearch& g = s_gs;
  const bool idle = g.state != 1 && g.state != 2 && s_li_job.state == 0;
  if (idle && s_gs_preview >= 0) {   // the tapped GIF, now the network is free
    char url[96];
    snprintf(s_gp_send, sizeof s_gp_send, "%s", g.ids[s_gs_preview]);
    s_gs_preview = -1;
    if (!giphy::mediaUrl(s_gp_send, "200w", url, sizeof url) ||
        !linkImgStart(true, linkimg::urlId(url), url, "GIF", true))
      s_gp_send[0] = 0;
  } else if (idle && !s_gp_root && !s_gp_send[0] && g.n > 0) {
    // Back from a preview: the note, and the stills it stopped.
    if (s_gs_noted && s_gs_note) lv_label_set_text(s_gs_note, TR("Powered by GIPHY"));
    s_gs_noted = false;
    bool missing = false;
    for (int i = 0; i < g.n; ++i) missing |= g.got[i] == 0;
    if (missing) { g.cancel = false; g.resume = true; gifWorkerStart(); }
  }
  if (s_gs_again && (uint32_t)(millis() - s_gs_typed_ms) > 900 && gifSearchStart()) s_gs_again = false;
  if (!s_gs_grid || g.state < 2) return;
  if (!s_gs_listed) {
    s_gs_listed = true;
    const int n = g.n;
    if (s_gs_note) {
      const bool trending = !g.q[0];
      lv_label_set_text(s_gs_note, n ? (trending ? TR("Trending \xE2\x80\xA2 Powered by GIPHY") : TR("Powered by GIPHY"))
                                     : g.http == HTTP_CODE_OK ? TR("No GIFs found")
                                     : g.http < 0 ? TR("No connection to GIPHY") : TR("GIPHY said no"));
    }
    for (int i = 0; i < n; ++i) {
      lv_obj_t* b = lv_btn_create(s_gs_grid);
      lv_obj_set_size(b, s_gs_cell, s_gs_cell);
      styleButton(b);
      lv_obj_set_style_bg_color(b, lv_color_hex(COLOR_CONTROL), LV_PART_MAIN);
      lv_obj_set_style_pad_all(b, 0, LV_PART_MAIN);
      lv_obj_set_style_radius(b, 6, LV_PART_MAIN);
      lv_obj_set_style_clip_corner(b, true, LV_PART_MAIN);
      lv_obj_add_event_cb(b, gifSendCb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
      lv_obj_t* l = lv_label_create(b);
      lv_label_set_text(l, "GIF");
      lv_obj_set_style_text_color(l, lv_color_hex(COLOR_SUB), LV_PART_MAIN);
      lv_obj_center(l);
      s_gs_shown[i] = 1;
    }
#if CAP_KEYPAD_NAV
    navMarkDirty();
#endif
  }
  for (int i = 0; i < g.n; ++i) {
    if (s_gs_shown[i] != 1 || g.got[i] != 1 || !g.px[i]) continue;
    lv_obj_t* b = lv_obj_get_child(s_gs_grid, i);
    lv_img_dsc_t* d = (lv_img_dsc_t*)calloc(1, sizeof(lv_img_dsc_t));
    if (!b || !d) { free(d); continue; }
    d->header.cf = LV_IMG_CF_TRUE_COLOR;
    d->header.w = (uint32_t)g.w[i];
    d->header.h = (uint32_t)g.h[i];
    d->data_size = (uint32_t)g.w[i] * (uint32_t)g.h[i] * 2u;
    d->data = (const uint8_t*)g.px[i];
    g.px[i] = nullptr;   // the image owns it now
    lv_obj_clean(b);
    lv_obj_t* im = lv_img_create(b);
    lv_img_set_src(im, d);
    lv_obj_clear_flag(im, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(im, gifStillFreeCb, LV_EVENT_DELETE, d);
    lv_obj_center(im);
    s_gs_shown[i] = 2;
  }
}

static void gifPickerTaCb(lv_event_t* e) {
  const lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_VALUE_CHANGED) { s_gs_again = true; s_gs_typed_ms = millis(); }
  else if (code == LV_EVENT_READY) { s_gs_again = true; s_gs_typed_ms = millis() - 1000; }   // Enter: now
}

static void gifPickerBackdropCb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  if (lv_event_get_target(e) != lv_event_get_current_target(e)) return;   // the backdrop only
  gifPickerClose();
}

static void gifPickerOpen() {
  gifPickerClose();
  if (!g_lv.task) return;
  if (!linkImgWifiUp()) {
    g_lv.task->showAlert(TR("Wi-Fi not connected (Settings \xe2\x86\x92 Wi-Fi)"), 2000);
    return;
  }
  SearchSheet o;
  searchSheetBuild(o, "GIF", TR("Search GIPHY"), 60, gifPickerBackdropCb, gifPickerTaCb);
  s_gs_sheet = o.sheet;
  s_gs_ta = o.ta;
  s_gs_note = o.note;
  s_gs_grid = o.grid;
  lv_label_set_text(s_gs_note, TR("Searching..."));
  const lv_coord_t inner_w = lv_disp_get_hor_res(nullptr) - 32;
  // Three across at least, about 100 px each where there's room.
  int cols = (int)((inner_w + 4) / 104);
  if (cols < 3) cols = 3;
  s_gs_cell = (lv_coord_t)((inner_w - 4 * (cols - 1)) / cols);

  s_gs_again = !gifSearchStart();   // trending; busy: as soon as it's free
  s_gs_typed_ms = millis() - 1000;
  s_gs_timer = lv_timer_create(gifPickerPoll, 300, nullptr);

  searchSheetFocus(o);
}

// ---- GIFs playing in their bubbles (Settings > General > Play GIFs in chat) -----
// A bubble's GIF shows its still first; its small animated copy (Giphy's 100 px
// one) comes after, one at a time like every linked picture, and then plays in the
// still's place. The frames are kept for the last few GIFs, shared by every bubble
// showing one, and freed once no bubble is.
struct GifAnim { uint32_t id; uint16_t* px; int w, h, n; uint16_t delays[kGifBubbleFrames]; uint8_t refs; uint32_t used; };
static GifAnim s_ga[4];
struct GifLive { lv_obj_t* img; uint32_t id; lv_img_dsc_t* still; lv_img_dsc_t dsc; int8_t a; uint8_t at; uint32_t next_ms; };
static GifLive s_gl[10];
static lv_timer_t* s_gl_timer = nullptr;
static uint32_t s_ga_want[6];
static char s_ga_want_url[6][96];
static uint8_t s_ga_want_n = 0;
static uint32_t s_ga_failed[16];
static uint8_t s_ga_failed_at = 0;

// Frees the kept frames of GIFs no bubble on screen shows (the UI loop only),
// those let go of over `idle_ms` ago (0: all of them).
static void gifAnimTrim(uint32_t idle_ms) {
  const uint32_t now = millis();
  for (auto& g : s_ga) {
    if (!g.px || g.refs || (uint32_t)(now - g.used) < idle_ms) continue;
    lvglPsramFree(g.px);
    g = GifAnim();
  }
}

static int gifAnimFind(uint32_t id) {
  for (int i = 0; i < 4; ++i) if (s_ga[i].px && s_ga[i].id == id) return i;
  return -1;
}

static void gifLiveStep(lv_timer_t*) {
  const uint32_t now = millis();
  bool any = false;
  for (auto& l : s_gl) {
    if (!l.img || l.a < 0) continue;
    any = true;
    const GifAnim& g = s_ga[(int)l.a];
    if (g.n < 2 || (int32_t)(now - l.next_ms) < 0) continue;
    l.at = (uint8_t)((l.at + 1) % g.n);
    l.dsc.data = (const uint8_t*)(g.px + (size_t)l.at * g.w * g.h);
    lv_img_cache_invalidate_src(&l.dsc);
    lv_obj_invalidate(l.img);
    l.next_ms = now + g.delays[l.at];
  }
  if (!any && s_gl_timer) { lv_timer_del(s_gl_timer); s_gl_timer = nullptr; }
}

static void gifLiveAttach(GifLive& l, int a) {
  GifAnim& g = s_ga[a];
  ++g.refs;
  g.used = millis();
  l.a = (int8_t)a;
  l.at = 0;
  memset(&l.dsc, 0, sizeof l.dsc);
  l.dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
  l.dsc.header.w = (uint32_t)g.w;
  l.dsc.header.h = (uint32_t)g.h;
  l.dsc.data_size = (uint32_t)g.w * (uint32_t)g.h * 2u;
  l.dsc.data = (const uint8_t*)g.px;
  lv_img_cache_invalidate_src(&l.dsc);
  lv_img_set_src(l.img, &l.dsc);
  l.next_ms = millis() + g.delays[0];
  if (!s_gl_timer) s_gl_timer = lv_timer_create(gifLiveStep, 30, nullptr);
}

// A bubble's still that is a GIF's: it plays once its frames are here.
static void gifLiveAdd(lv_obj_t* img, lv_img_dsc_t* still, const char* still_url) {
  char gid[giphy::kIdMax], url[96];
  if (!gifPlayOn() || !giphy::idOfMediaUrl(still_url, gid, sizeof gid) || !giphy::mediaUrl(gid, "100w", url, sizeof url))
    return;
  GifLive* l = nullptr;
  for (auto& e : s_gl) if (!e.img) { l = &e; break; }
  if (!l) return;   // more GIFs on screen than this plays: the rest stay still
  const uint32_t id = linkimg::urlId(url);
  l->img = img;
  l->id = id;
  l->still = still;
  l->a = -1;
  const int a = gifAnimFind(id);
  if (a >= 0) { gifLiveAttach(*l, a); return; }
  for (uint32_t f : s_ga_failed) if (f == id) return;
  for (uint8_t i = 0; i < s_ga_want_n; ++i) if (s_ga_want[i] == id) return;
  if (s_ga_want_n == 6) {   // full: the oldest goes
    memmove(&s_ga_want[0], &s_ga_want[1], sizeof s_ga_want[0] * 5);
    memmove(&s_ga_want_url[0], &s_ga_want_url[1], sizeof s_ga_want_url[0] * 5);
    --s_ga_want_n;
  }
  s_ga_want[s_ga_want_n] = id;
  snprintf(s_ga_want_url[s_ga_want_n], sizeof s_ga_want_url[0], "%s", url);
  ++s_ga_want_n;
}

// The bubble's image is going: its still goes with it, its GIF's frames when no
// other bubble shows them. False when it wasn't a GIF's.
static bool gifLiveGone(lv_obj_t* img) {
  for (auto& l : s_gl) {
    if (l.img != img) continue;
    if (l.a >= 0 && s_ga[(int)l.a].refs) { --s_ga[(int)l.a].refs; s_ga[(int)l.a].used = millis(); }
    lv_img_cache_invalidate_src(&l.dsc);
    if (l.still) { free((void*)l.still->data); free(l.still); }
    l = GifLive();
    return true;
  }
  return false;
}

// A GIF's frames came (on the UI loop): kept, and every bubble showing it plays.
static void gifAnimLanded(uint32_t id, uint16_t* px, int w, int h, int n, const uint16_t* delays) {
  int slot = -1;
  for (int i = 0; i < 4; ++i) if (!s_ga[i].px) { slot = i; break; }
  if (slot < 0)   // the least recently started one nothing shows
    for (int i = 0; i < 4; ++i)
      if (!s_ga[i].refs && (slot < 0 || (int32_t)(s_ga[i].used - s_ga[slot].used) < 0)) slot = i;
  if (slot < 0) { lvglPsramFree(px); return; }   // all on screen: this one stays still
  GifAnim& g = s_ga[slot];
  if (g.px) lvglPsramFree(g.px);
  g.id = id;
  g.px = px;
  g.w = w;
  g.h = h;
  g.n = n < kGifBubbleFrames ? n : kGifBubbleFrames;
  memcpy(g.delays, delays, sizeof(uint16_t) * (size_t)g.n);
  g.refs = 0;
  g.used = millis();
  for (auto& l : s_gl)
    if (l.img && l.a < 0 && l.id == id) gifLiveAttach(l, slot);
}

// From the UI loop: collects a finished job, then starts the next download.
static void linkImgTick() {
  LiJob& j = s_li_job;
  gifAnimTrim(30000);   // a chat closed or scrolled on: its GIFs' frames go after 30 s
  if (j.state == 1) return;
  if (j.state == 2) {
    const uint32_t id = j.id;
    if (j.raw) {   // a JPEG too big for the full decode: scaled by TJpgDec
      // TJpgDec scales by 1/2, 1/4 or 1/8 at most, so a big photo (over 1536 px)
      // never got down to the thumbnail's size and the bubble stayed text while a
      // tap still opened it: decode to the viewer's size, then shrink from there.
      int w = 0, h = 0;
      uint8_t* rgb = decodeJpegScaledToRgb565(j.raw, j.raw_len, &w, &h, kLiViewSide);
      if (!rgb && !strncmp(s_jpgScaleErr, "Progressive", 11)) j.progressive = true;
      free(j.raw);
      j.raw = nullptr;
      if (rgb && w > 0 && h > 0) {
        if (j.view) { j.px = (uint16_t*)rgb; }
        else {
          int tw = w, th = h;
          if (tw > kLiThumbSide || th > kLiThumbSide) {
            if (tw >= th) { th = (int)((int64_t)th * kLiThumbSide / tw); tw = kLiThumbSide; }
            else          { tw = (int)((int64_t)tw * kLiThumbSide / th); th = kLiThumbSide; }
            if (tw < 1) tw = 1;
            if (th < 1) th = 1;
          }
          j.px = (uint16_t*)malloc((size_t)tw * th * 2);
          if (j.px) {
            const uint16_t* src = (const uint16_t*)rgb;
            for (int y = 0; y < th; y++) {
              const int sy = (int)((int64_t)y * h / th);
              for (int x = 0; x < tw; x++) j.px[y * tw + x] = src[(size_t)sy * w + (int)((int64_t)x * w / tw)];
            }
          }
          lvglPsramFree(rgb);
          w = tw;
          h = th;
        }
        j.w = w;
        j.h = h;
        j.res = j.px ? linkimg::kOk : linkimg::kNoMemory;
      } else if (rgb) {
        lvglPsramFree(rgb);
      }
    }
    if (j.anim && !j.view) {
      if (j.res == linkimg::kOk && j.px && j.frames > 0) gifAnimLanded(id, j.px, j.w, j.h, j.frames, j.delays);
      else {
        if (j.px) lvglPsramFree(j.px);
        s_ga_failed[s_ga_failed_at++ % 16] = id;
      }
    } else if (j.view && j.anim) {
      if (j.res == linkimg::kOk && j.px && j.frames > 0) gifPlayerShow(j.px, j.w, j.h, j.frames, j.delays);
      else {
        s_gp_send[0] = 0;   // a preview that didn't come: the picker carries on
        if (g_lv.task) g_lv.task->showAlert(TR(linkImgWhy(j)), 2200);
      }
      if (j.res != linkimg::kOk && j.px) lvglPsramFree(j.px);
    } else if (j.view) {
      if (j.res == linkimg::kOk && j.px) {
        s_fm_img_path[0] = '\0';   // not a file the viewer can set as a wallpaper
        s_fm_img_on_sd = false;
        fmImageShowDecoded(j.title, (uint8_t*)j.px, j.w, j.h);   // the viewer owns the pixels now
      } else {
        if (j.px) lvglPsramFree(j.px);
        if (g_lv.task) g_lv.task->showAlert(TR(linkImgWhy(j)), 2200);
      }
    } else {
      if (j.res == linkimg::kOk && j.px && VoxPhotos::saveThumb(id, j.px, j.w, j.h)) voxThumbLanded();
      if (j.res != linkimg::kOk) Serial.printf("[LINKIMG] %08lx: %s (http %d)\n", (unsigned long)id, linkImgWhy(j), j.http);
      free(j.px);
      // No Wi-Fi when it ran: try again later rather than never.
      if (j.http < 0 && !linkImgWifiUp()) {
        linkImgWant(id, j.url);
        s_li_retry_ms = millis() + 30000;
      } else {
        s_li_tried[s_li_tried_at++ % (sizeof s_li_tried / sizeof s_li_tried[0])] = id;
        // A picture that won't come says why, rather than the bubble just staying
        // text (each link is tried once a boot, so this is once too).
        if (j.res != linkimg::kOk && g_lv.task) {
          char msg[96];
          snprintf(msg, sizeof msg, "%s: %s", TR("Linked picture"), TR(linkImgWhy(j)));
          g_lv.task->showAlert(msg, 2600);
        }
      }
    }
    j.px = nullptr;
    j.state = 0;
    return;
  }
  if (!linkImgOn() || !linkImgWifiUp() || s_gif_busy) return;
  if (!s_li_want_n) {   // pictures first, then GIFs to play in their bubbles
    if (!s_ga_want_n) return;
    --s_ga_want_n;
    const uint32_t id = s_ga_want[s_ga_want_n];
    bool shown = false;   // still on screen?
    for (const auto& l : s_gl) shown |= (l.img && l.id == id && l.a < 0);
    if (shown && gifAnimFind(id) < 0) linkImgStart(false, id, s_ga_want_url[s_ga_want_n], nullptr, true);
    return;
  }
  if (s_li_retry_ms && (int32_t)(millis() - s_li_retry_ms) < 0) return;
  s_li_retry_ms = 0;
  const LiWant w = s_li_want[--s_li_want_n];   // newest first: the one on screen
  if (VoxPhotos::thumbSize(w.id, nullptr, nullptr)) return;
  if (!linkImgStart(false, w.id, w.url, nullptr)) linkImgWant(w.id, w.url);
}

// The bubble: a linked picture that's here shows above the text; one that
// isn't is asked for.
static void linkImgBubble(const char* text, ChatBubbleDisplay& d) {
  if (!linkImgOn() || !text) return;
  char url[sizeof d.thumb_url];
  char gid[giphy::kIdMax];
  if (giphy::idOf(text, gid, sizeof gid)) {
    if (!giphy::mediaUrl(gid, "200w_s", url, sizeof url)) return;   // a GIF: its still, tapped to play
  } else if (!linkimg::findUrl(text, url, sizeof url)) {
    return;
  }
  const uint32_t id = linkimg::urlId(url);
  int tw = 0, th = 0;
  if (VoxPhotos::thumbSize(id, &tw, &th)) {
    d.thumb_sid = id;
    d.thumb_w = (lv_coord_t)tw;
    d.thumb_h = (lv_coord_t)th;
    d.thumb_link = true;
    snprintf(d.thumb_url, sizeof d.thumb_url, "%s", url);
  } else {
    linkImgWant(id, url);
  }
}

// Its thumbnail in the bubble; a tap opens the picture.
static void linkImgThumbEventCb(lv_event_t* e) {
  char* url = (char*)lv_event_get_user_data(e);
  if (lv_event_get_code(e) == LV_EVENT_DELETE) {
    lv_img_dsc_t* dsc = (lv_img_dsc_t*)lv_img_get_src(lv_event_get_target(e));
    if (!gifLiveGone(lv_event_get_target(e)) && dsc) { free((void*)dsc->data); free(dsc); }
    free(url);
    return;
  }
  if (lv_event_get_code(e) == LV_EVENT_SHORT_CLICKED && url) linkImgView(url);
}
static void linkImgAddThumb(lv_obj_t* parent, const ChatBubbleDisplay& d, lv_coord_t x, lv_coord_t y) {
  int w = 0, h = 0;
  uint16_t* px = nullptr;
  if (!VoxPhotos::loadThumb(d.thumb_sid, &w, &h, &px)) return;   // the row keeps its room
  lv_img_dsc_t* dsc = (lv_img_dsc_t*)calloc(1, sizeof(lv_img_dsc_t));
  char* url = strdup(d.thumb_url);
  if (!dsc || !url) { free(px); free(dsc); free(url); return; }
  dsc->header.always_zero = 0;
  dsc->header.cf = LV_IMG_CF_TRUE_COLOR;
  dsc->header.w = (uint32_t)w;
  dsc->header.h = (uint32_t)h;
  dsc->data_size = (uint32_t)w * (uint32_t)h * 2u;
  dsc->data = (const uint8_t*)px;
  lv_obj_t* img = lv_img_create(parent);
  lv_img_set_src(img, dsc);
  lv_obj_add_flag(img, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(img, LV_OBJ_FLAG_SCROLL_CHAIN);
  lv_obj_set_pos(img, x, y);
  lv_obj_set_style_radius(img, 4, LV_PART_MAIN);
  lv_obj_set_style_clip_corner(img, true, LV_PART_MAIN);
  lv_obj_add_event_cb(img, linkImgThumbEventCb, LV_EVENT_ALL, url);
  gifLiveAdd(img, dsc, url);   // a GIF's still: it plays when its frames come
}

#endif  // CAP_LINK_IMAGES

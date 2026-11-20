// SPDX-License-Identifier: GPL-3.0-or-later
// Fork firmware updates on the device: the network half of ForkUpdateCore.h.
// Included by UITask.cpp (so WiFiClientSecure is the P4's C6 socket there, as
// for the Reader), and only where the About page's OTA code is compiled.
//
// HTTPS needs more stack than the shared tile/version-check worker has (8 KB,
// and a TLS handshake overflowed it before), so each job runs on its own 16 KB
// task like the Reader's fetches, and the worker just waits for it.

#include <esp_heap_caps.h>
#include <mbedtls/md.h>
#if defined(HAS_TDISPLAY_P4) && TDP4_C6_HOSTED
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#endif
#include <mbedtls/platform.h>

#include "ForkUpdateCore.h"

namespace forkupd_dev {

enum class Kind : uint8_t { Latest, Slot, Sd, Fetch, Post };

struct Job {
  Kind kind;
  int n;                    // release to fetch (Slot, Sd)
  const char* path;         // SD destination (Sd)
  volatile int* pct;
  char* msg;
  size_t msg_cap;
  int result;               // Latest: N or -1; Slot/Sd/Fetch: 1 ok, 0 failed
  SemaphoreHandle_t done;
  // Fetch: a file, whole, into buf. No default member initializers: the board
  // builds are C++11, where they'd stop the brace-initialised Jobs above being
  // aggregates (fields left out are zeroed anyway).
  const char* url;
  uint8_t* buf;
  size_t cap;
  size_t got;
  // Post: a JSON body sent with `method` (POST, DELETE) and an
  // X-MeshCore-Signature header; the reply goes into buf, its status into code.
  const char* method;
  const char* sig;
  const uint8_t* body;
  size_t body_len;
  int code;
};

// mbedTLS takes its memory from INTERNAL RAM in these builds
// (CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC: Arduino's prebuilt esp32s3 SDK, and IDF's
// default on the P4): two 16 KB record buffers plus the handshake, ~40 KB per
// connection. With BLE, Wi-Fi and the UI up, that much internal RAM is often not
// left in one piece, and the handshake fails, which HTTPClient reports as -1
// ("can't connect"). That was "Update failed: signature: can't connect to
// github.com (-1)": the update check got through earlier, while there was
// still room, and the download a while later did not. These boards all have
// PSRAM, so send mbedTLS there (internal RAM only if PSRAM is exhausted). It
// covers every TLS client on the device (the Reader too), and is installed
// once, before the first fork update request.
static void* tlsCalloc(size_t n, size_t size) {
  void* p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p ? p : heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}
static void tlsFree(void* p) { heap_caps_free(p); }   // whichever heap it came from

static void tlsUsePsram() {
  static bool done = false;
  if (done) return;
  done = true;
  if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0) mbedtls_platform_set_calloc_free(tlsCalloc, tlsFree);
}

// Internal RAM free and its largest block, in KB, for an error message.
static void ramNote(char* out, size_t cap) {
  snprintf(out, cap, "RAM %uK/%uK", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
           (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
}

static void say(Job& j, const char* m) {
  if (j.msg && j.msg_cap) snprintf(j.msg, j.msg_cap, "%s", m);
}

static void hostOf(const char* url, char* out, size_t cap) {
  const char* a = strstr(url, "://");
  a = a ? a + 3 : url;
  const char* b = strchr(a, '/');
  const size_t n = b ? (size_t)(b - a) : strlen(a);
  snprintf(out, cap, "%.*s", (int)(n < cap ? n : cap - 1), a);
}

// One HTTPS GET, redirects not followed (the callers follow them, reading
// Location). Two backends:
//  - Arduino's HTTPClient over WiFiClientSecure (the S3 boards);
//  - ESP-IDF's esp_http_client on the T-Display P4 with the C6 over esp-hosted.
//    Arduino's TLS client doesn't link on the P4 (an ABI mismatch in
//    arduino-esp32's ssl_client; variants/tdisplay_p4/ssl_client_stub.cpp
//    stands in, failing every connection), which was the P4's "connection
//    refused (-1), TLS: ERROR - Generic error" on every update check. IDF's
//    client does TLS itself, and checks github's certificate against IDF's
//    bundle.
#if defined(HAS_TDISPLAY_P4) && TDP4_C6_HOSTED
#define FORKUPD_IDF_HTTP 1
#else
#define FORKUPD_IDF_HTTP 0
#endif

#if FORKUPD_IDF_HTTP
struct Get {
  esp_http_client_handle_t c = nullptr;
  int code = 0;          // HTTP status, or < 0 when it didn't get one
  long len = -1;         // Content-Length, -1 when not given
  char loc[1024] = "";   // Location of a redirect (release assets' are long)
  esp_err_t err = ESP_OK;

  static esp_err_t onEvent(esp_http_client_event_t* e) {
    Get* g = static_cast<Get*>(e->user_data);
    if (g && e->event_id == HTTP_EVENT_ON_HEADER && e->header_key && e->header_value &&
        !strcasecmp(e->header_key, "Location"))
      snprintf(g->loc, sizeof g->loc, "%s", e->header_value);
    return ESP_OK;
  }
  int open(const char* url) {
    close();
    loc[0] = 0;
    len = -1;
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.method = HTTP_METHOD_GET;
    cfg.timeout_ms = 20000;
    cfg.disable_auto_redirect = true;
    cfg.buffer_size = 4096;        // github's response headers
    cfg.buffer_size_tx = 2048;     // a release asset's signed URL in the request line
    cfg.user_agent = "wadamesh-fork";
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.event_handler = onEvent;
    cfg.user_data = this;
    cfg.keep_alive_enable = false;
    c = esp_http_client_init(&cfg);
    if (!c) { err = ESP_ERR_NO_MEM; return code = -1; }
    err = esp_http_client_open(c, 0);
    if (err != ESP_OK) return code = -1;
    const int64_t n = esp_http_client_fetch_headers(c);
    code = esp_http_client_get_status_code(c);
    if (code <= 0) { err = n < 0 ? (esp_err_t)n : ESP_FAIL; return code = -1; }
    len = n > 0 ? (long)n : -1;
    return code;
  }
  // Body bytes: > 0 read, 0 none yet, -1 at its end or on an error.
  int read(uint8_t* b, size_t n) {
    if (!c) return -1;
    const int r = esp_http_client_read(c, (char*)b, (int)n);
    if (r > 0) return r;
    if (r == -ESP_ERR_HTTP_EAGAIN) return 0;
    return -1;
  }
  bool connected() const { return c != nullptr; }
  // Why it couldn't connect, for the message.
  void note(const char* host, char* out, size_t cap) {
    (void)host;
    const int e = c ? esp_http_client_get_errno(c) : 0;
    if (e) snprintf(out, cap, "%s, errno %d", esp_err_to_name(err), e);
    else snprintf(out, cap, "%s", esp_err_to_name(err));
  }
  void close() {
    if (c) { esp_http_client_close(c); esp_http_client_cleanup(c); c = nullptr; }
  }
  ~Get() { close(); }
};
#else
struct Get {
  WiFiClientSecure* cl = nullptr;
  HTTPClient* h = nullptr;
  decltype(((HTTPClient*)nullptr)->getStreamPtr()) st = nullptr;
  int code = 0;
  long len = -1;
  char loc[1024] = "";

  int open(const char* url) {
    close();
    loc[0] = 0;
    len = -1;
    cl = new (std::nothrow) WiFiClientSecure();
    h = new (std::nothrow) HTTPClient();
    if (!cl || !h) return code = -1;
    cl->setInsecure();   // the image is checked against the release key's signature
    h->setReuse(false);
    h->setConnectTimeout(10000);
    h->setTimeout(20000);
    h->setUserAgent("wadamesh-fork");
    h->setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    const char* keys[] = {"Location"};
    h->collectHeaders(keys, 1);
    if (!h->begin(*cl, url)) return code = -1;
    code = h->GET();
    if (code > 0) {
      snprintf(loc, sizeof loc, "%s", h->header("Location").c_str());
      len = h->getSize();
      st = h->getStreamPtr();
    }
    return code;
  }
  int read(uint8_t* b, size_t n) {
    if (!st) return -1;
    const int a = st->available();
    if (a <= 0) return st->connected() ? 0 : -1;
    const int r = st->read(b, n < (size_t)a ? n : (size_t)a);
    return r > 0 ? r : 0;
  }
  bool connected() const { return st != nullptr; }
  // Whether the name resolved, and what TLS said (mbedTLS's error), so a
  // report says which of DNS, the socket and the handshake it was.
  void note(const char* host, char* out, size_t cap) {
    if (!cap) return;
    out[0] = 0;
#if !(defined(HAS_TDISPLAY_P4) && !TDP4_C6_HOSTED)   // the AT backend has neither
    IPAddress ip;
    if (!WiFi.hostByName(host, ip)) { snprintf(out, cap, "DNS failed"); return; }
    char tls[64] = "";
    if (cl) cl->lastError(tls, sizeof tls);
    if (tls[0]) snprintf(out, cap, "TLS: %s", tls);
    else snprintf(out, cap, "no TCP connection");
#else
    (void)host;
#endif
  }
  void close() {
    st = nullptr;
    if (h) { h->end(); delete h; h = nullptr; }
    if (cl) { cl->stop(); delete cl; cl = nullptr; }
  }
  ~Get() { close(); }
};
#endif

static bool isRedirect(int code) { return code == 301 || code == 302 || code == 303 || code == 307 || code == 308; }

// open() with two retries of a connection that failed outright (on the P4 the
// first TLS connection after Wi-Fi comes up often doesn't make it) or that
// got a 5xx: github.com's release pages take ~10 s and sometimes answer 504.
static int openRetrying(Get& g, const char* url, char* note, size_t note_cap) {
  if (note_cap) note[0] = 0;
  for (int attempt = 0; attempt < 3; attempt++) {
    const int code = g.open(url);
    if (code > 0 && (code < 500 || attempt == 2)) return g.code;
    if (code <= 0) {
      char host[64];
      hostOf(url, host, sizeof host);
      g.note(host, note, note_cap);
    }
    g.close();
    vTaskDelay(pdMS_TO_TICKS(1500 * (attempt + 1)));
  }
  return g.code;
}

// Newest release: github.com answers /releases/latest with a redirect to its tag.
// A renamed repo answers with a redirect to its new name first (and that one
// with the tag), so redirects are followed until one names a tag: a build that
// knows the old name keeps finding updates after a rename.
static int latest() {
  static auto& url = psramStatic<char[1024]>();   // PSRAM: internal RAM is for Wi-Fi and BLE
  snprintf(url, sizeof url, "https://github.com/%s/releases/latest", WADA_FORK_REPO);
  s_verchk_why[0] = 0;
  for (int hop = 0; hop < 4; hop++) {
    char host[64], note[96];
    hostOf(url, host, sizeof host);
    Get g;
    const int code = openRetrying(g, url, note, sizeof note);
    if (!isRedirect(code)) {
      if (code < 0) {
        char ram[32];
        ramNote(ram, sizeof ram);
        snprintf(s_verchk_why, sizeof s_verchk_why, "%s: can't connect%s%s, %s", host, note[0] ? ": " : "", note, ram);
      } else {
        snprintf(s_verchk_why, sizeof s_verchk_why, "%s: HTTP %d", host, code);
      }
      return -1;
    }
    const int rel = forkupd::releaseCode(g.loc);
    if (rel >= 0) return rel;
    if (!g.loc[0]) {   // -1 too when there are no releases yet
      snprintf(s_verchk_why, sizeof s_verchk_why, "%s: HTTP %d, no Location header", host, code);
      return -1;
    }
    if (g.loc[0] == '/') {
      char next[1024];
      snprintf(next, sizeof next, "https://github.com%s", g.loc);
      snprintf(url, sizeof url, "%s", next);
    } else {
      snprintf(url, sizeof url, "%s", g.loc);
    }
  }
  snprintf(s_verchk_why, sizeof s_verchk_why, "too many redirects");
  return -1;
}

static void assetUrl(char* out, size_t cap, const char* tag, const char* file) {
  snprintf(out, cap, "https://github.com/" WADA_FORK_REPO "/releases/download/%s/%s", tag, file);
}

// One GET that may be redirected (a release file on github.com is a 302 to
// release-assets.githubusercontent.com, with a long signed URL). Each hop gets
// a fresh client of its own. Up to 5 hops, and two retries of a hop that
// couldn't connect. True with g.code 200 and the body ready to read; else a
// message in j.
static bool getFollowing(Job& j, const char* first, Get& g, const char* what) {
  static auto& url = psramStatic<char[1024]>();   // PSRAM: internal RAM is for Wi-Fi and BLE
  snprintf(url, sizeof url, "%s", first);
  for (int hop = 0; hop < 5; hop++) {
    char note[96];
    openRetrying(g, url, note, sizeof note);
    if (g.code == 200) return true;
    if (isRedirect(g.code)) {
      if (strlen(g.loc) < 8) break;
      snprintf(url, sizeof url, "%s", g.loc);
      continue;
    }
    char host[64], m[200];
    hostOf(url, host, sizeof host);
    if (g.code == 404) snprintf(m, sizeof m, "%s not in the release", what);
    else if (g.code < 0) {
      char ram[32];
      ramNote(ram, sizeof ram);
      snprintf(m, sizeof m, "%s: can't connect to %s (%s%s%s)", what, host, note, note[0] ? ", " : "", ram);
    }
    else snprintf(m, sizeof m, "%s: HTTP %d from %s", what, g.code, host);
    say(j, m);
    return false;
  }
  char m[64];
  snprintf(m, sizeof m, "%s: too many redirects", what);
  say(j, m);
  return false;
}

// Reads exactly `want` body bytes into `out` (or as they come, to `sink`),
// giving up after `idle_ms` without data. Returns how many it got.
template <typename Sink>
static size_t readBody(Get& g, size_t want, uint32_t idle_ms, Sink sink) {
  uint8_t buf[1024];   // on this task's internal stack: flash and SD want DMA-capable memory
  size_t done = 0;
  uint32_t last = millis();
  while (done < want) {
    const size_t n = want - done < sizeof buf ? want - done : sizeof buf;
    const int r = g.read(buf, n);
    if (r < 0) break;
    if (r == 0) {
      if (millis() - last > idle_ms) break;
      vTaskDelay(pdMS_TO_TICKS(2));
      continue;
    }
    last = millis();
    if (!sink(buf, (size_t)r)) break;
    done += (size_t)r;
  }
  return done;
}

static bool fetchSig(Job& j, const char* tag, const char* file, uint8_t sig[forkupd::kSigBytes]) {
  char name[64], url[192];
  snprintf(name, sizeof name, "%s.sig", file);
  assetUrl(url, sizeof url, tag, name);
  Get g;
  if (!getFollowing(j, url, g, "signature")) return false;
  size_t got = 0;
  readBody(g, forkupd::kSigBytes, 15000, [&](const uint8_t* b, size_t n) -> bool {
    memcpy(sig + got, b, n);
    got += n;
    return true;
  });
  g.close();
  if (got != forkupd::kSigBytes) { say(j, "bad signature file"); return false; }
  return true;
}

// Streams the image into the spare OTA slot or a file on the SD card, hashing as
// it goes; it is kept only if the hash carries the release key's signature.
static bool fetchImage(Job& j) {
  char tag[32], file[48], url[192];
  forkupd::tagFor(j.n, tag, sizeof tag);
  snprintf(file, sizeof file, "wadamesh-%s.bin", WADA_BOARD_ID);
  uint8_t sig[forkupd::kSigBytes];
  if (!fetchSig(j, tag, file, sig)) return false;

  assetUrl(url, sizeof url, tag, file);
  Get g;
  if (!getFollowing(j, url, g, "firmware")) return false;
  const long len = g.len;
  if (len <= 0) { say(j, "bad length"); return false; }

#if CAP_SD
  File f;
#endif
  if (j.kind == Kind::Slot) {
#if CAP_OTA
    if (!Update.begin((size_t)len)) {
      char m[64];
      // The T-Deck's app slots grew (partitions_tdeck_touch_big_app.csv): a radio
      // still on the old table can only take the new image over USB.
      const esp_partition_t* spare = esp_ota_get_next_update_partition(NULL);
      if (spare && (size_t)len > spare->size)
        snprintf(m, sizeof m, "too big for this radio's slots: install once over USB");
      else
        snprintf(m, sizeof m, "won't fit: %s", Update.errorString());
      say(j, m);
      return false;
    }
#else
    say(j, "no update slot");
    return false;
#endif
  } else {
#if CAP_SD
    SD.remove(j.path);
    f = SD.open(j.path, FILE_WRITE);
    if (!f) { say(j, "SD open failed"); return false; }
#else
    say(j, "no SD card");
    return false;
#endif
  }

  mbedtls_md_context_t md;
  mbedtls_md_init(&md);
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  bool ok = info && mbedtls_md_setup(&md, info, 0) == 0 && mbedtls_md_starts(&md) == 0;

  size_t done = 0, seen = 0;
  if (ok) {
    done = readBody(g, (size_t)len, 20000, [&](const uint8_t* b, size_t n) -> bool {
      if (mbedtls_md_update(&md, b, n) != 0) { ok = false; return false; }
      size_t w = 0;
#if CAP_OTA
      if (j.kind == Kind::Slot) w = Update.write(const_cast<uint8_t*>(b), n);
#endif
#if CAP_SD
      if (j.kind == Kind::Sd) w = f.write(b, n);
#endif
      if (w != n) { say(j, j.kind == Kind::Sd ? "SD write failed" : "flash write failed"); ok = false; return false; }
      seen += n;
      if (j.pct) *j.pct = (int)((uint64_t)seen * 100 / (uint64_t)len);
      return true;
    });
  }
  g.close();
  uint8_t digest[32];
  if (ok && mbedtls_md_finish(&md, digest) != 0) ok = false;
  mbedtls_md_free(&md);
  if (ok && done != (size_t)len) {
    char m[40];
    snprintf(m, sizeof m, "short read %u/%ld", (unsigned)done, len);
    say(j, m);
    ok = false;
  }
  if (ok && !forkupd::verify(tag, file, digest, sig)) {
    say(j, "signature check failed");
    ok = false;
  }

  if (j.kind == Kind::Slot) {
#if CAP_OTA
    if (!ok) { Update.abort(); return false; }
    if (!Update.end(true)) {   // finalize, verify the image, mark the slot bootable
      char m[64];
      snprintf(m, sizeof m, "verify: %s", Update.errorString());
      say(j, m);
      return false;
    }
#endif
  } else {
#if CAP_SD
    f.close();
    if (!ok) { SD.remove(j.path); return false; }
#endif
  }
  if (j.pct) *j.pct = 100;
  return true;
}

// A small file (the predictive text dictionary), whole, into j.buf.
static bool fetchToBuf(Job& j) {
  Get g;
  if (!getFollowing(j, j.url, g, "file")) return false;
  const long len = g.len;
  if (len <= 0 || (size_t)len > j.cap) {
    char m[48];
    snprintf(m, sizeof m, "unexpected size %ld", len);
    say(j, m);
    return false;
  }
  j.got = 0;
  readBody(g, (size_t)len, 20000, [&](const uint8_t* b, size_t n) -> bool {
    memcpy(j.buf + j.got, b, n);
    j.got += n;
    return true;
  });
  g.close();
  if (j.got != (size_t)len) {
    char m[48];
    snprintf(m, sizeof m, "short read %u/%ld", (unsigned)j.got, len);
    say(j, m);
    return false;
  }
  return true;
}

// One request with a body (MediaMesh coverage points to an analyzer): the
// reply, whatever its status, into buf (cap - 1 bytes at most, NUL-ended).
// http:// or https://. The points are signed and become public on the
// analyzer anyway, so the certificate isn't checked (as for the update, whose
// image is checked against its own signature).
#if FORKUPD_IDF_HTTP
static esp_err_t postEvent(esp_http_client_event_t* e) {
  Job* j = static_cast<Job*>(e->user_data);
  if (j && e->event_id == HTTP_EVENT_ON_DATA && j->buf && j->cap && e->data && e->data_len > 0) {
    const size_t room = j->cap - 1 - j->got;
    const size_t n = (size_t)e->data_len < room ? (size_t)e->data_len : room;
    memcpy(j->buf + j->got, e->data, n);
    j->got += n;
  }
  return ESP_OK;
}
static bool postBody(Job& j) {
  j.got = 0;
  j.code = -1;
  esp_http_client_config_t cfg = {};
  cfg.url = j.url;
  cfg.method = !strcmp(j.method ? j.method : "POST", "DELETE") ? HTTP_METHOD_DELETE : HTTP_METHOD_POST;
  cfg.timeout_ms = 20000;
  cfg.user_agent = "wadamesh-fork";
  if (!strncmp(j.url, "https://", 8)) cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.event_handler = postEvent;
  cfg.user_data = &j;
  cfg.keep_alive_enable = false;
  esp_http_client_handle_t c = esp_http_client_init(&cfg);
  if (!c) { say(j, "out of memory"); return false; }
  esp_http_client_set_header(c, "Content-Type", "application/json");
  if (j.sig) esp_http_client_set_header(c, "X-MeshCore-Signature", j.sig);
  esp_http_client_set_post_field(c, (const char*)j.body, (int)j.body_len);
  const esp_err_t err = esp_http_client_perform(c);
  if (err == ESP_OK) j.code = esp_http_client_get_status_code(c);
  else say(j, esp_err_to_name(err));
  esp_http_client_cleanup(c);
  if (j.buf && j.cap) j.buf[j.got] = 0;
  return j.code > 0;
}
#else
static bool postBody(Job& j) {
  j.got = 0;
  j.code = -1;
  const bool tls = !strncmp(j.url, "https://", 8);
  WiFiClientSecure* scl = tls ? new (std::nothrow) WiFiClientSecure() : nullptr;
  WiFiClient* pcl = tls ? nullptr : new (std::nothrow) WiFiClient();
  HTTPClient* h = new (std::nothrow) HTTPClient();
  bool ok = false;
  if (h && (scl || pcl)) {
    if (scl) scl->setInsecure();
    h->setReuse(false);
    h->setConnectTimeout(10000);
    h->setTimeout(20000);
    h->setUserAgent("wadamesh-fork");
    if (scl ? h->begin(*scl, j.url) : h->begin(*pcl, j.url)) {
      h->addHeader("Content-Type", "application/json");
      if (j.sig) h->addHeader("X-MeshCore-Signature", j.sig);
      j.code = h->sendRequest(j.method ? j.method : "POST", (uint8_t*)j.body, j.body_len);
      if (j.code > 0) {
        String s = h->getString();
        if (j.buf && j.cap) {
          j.got = s.length() < j.cap - 1 ? s.length() : j.cap - 1;
          memcpy(j.buf, s.c_str(), j.got);
          j.buf[j.got] = 0;
        }
        ok = true;
      } else {
        char host[64], note[96];
        hostOf(j.url, host, sizeof host);
        Get g;
        g.cl = scl;
        g.note(host, note, sizeof note);
        g.cl = nullptr;
        say(j, note[0] ? note : "can't connect");
      }
      h->end();
    } else {
      say(j, "bad address");
    }
  } else {
    say(j, "out of memory");
  }
  delete h;
  if (scl) { scl->stop(); delete scl; }
  if (pcl) { pcl->stop(); delete pcl; }
  return ok;
}
#endif

static void jobBody(Job& j) {
  if (j.kind == Kind::Latest) j.result = latest();
  else if (j.kind == Kind::Fetch) j.result = fetchToBuf(j) ? 1 : 0;
  else if (j.kind == Kind::Post) j.result = postBody(j) ? 1 : 0;
  else j.result = fetchImage(j) ? 1 : 0;
}

static void taskFn(void* arg) {
  Job& j = *static_cast<Job*>(arg);
  jobBody(j);
  xSemaphoreGive(j.done);
  vTaskDelete(nullptr);
}

// On the shared audio worker (VoxAudioTask): its 16 KB internal stack is held
// from boot, so the update doesn't need a second free 16 KB block. At the
// worker's priority (3) a long download would starve the UI, so drop to 1.
static void workerFn(void* arg) {
  Job& j = *static_cast<Job*>(arg);
  const UBaseType_t prio = uxTaskPriorityGet(nullptr);
  vTaskPrioritySet(nullptr, 1);
  jobBody(j);
  vTaskPrioritySet(nullptr, prio);
  xSemaphoreGive(j.done);
}

// Runs `j` on the audio worker when it is idle, else on its own 16 KB task
// (core 0, beside Wi-Fi), and waits for it.
static int run(Job& j) {
  tlsUsePsram();
  // Already on the shared worker (the Pager's net worker runs there): waiting
  // for the worker would wait for ourselves, so run it here, on its stack.
  if (VoxAudioTask::isWorker()) {
    j.done = nullptr;
    const UBaseType_t prio = uxTaskPriorityGet(nullptr);
    vTaskPrioritySet(nullptr, 1);
    jobBody(j);
    vTaskPrioritySet(nullptr, prio);
    return j.result;
  }
  j.done = xSemaphoreCreateBinary();
  if (!j.done) { say(j, "out of memory"); return j.kind == Kind::Latest ? -1 : 0; }
  // The worker may be busy for a moment (a voice clip, the Reader): wait up to
  // 5 s for it rather than need a second free 16 KB block.
  bool on_worker = VoxAudioTask::run(workerFn, &j);
  for (int i = 0; !on_worker && VoxAudioTask::exists() && i < 50; i++) {
    vTaskDelay(pdMS_TO_TICKS(100));
    on_worker = VoxAudioTask::run(workerFn, &j);
  }
  if (on_worker) {
    xSemaphoreTake(j.done, portMAX_DELAY);
    vSemaphoreDelete(j.done);
    return j.result;
  }
  if (xTaskCreatePinnedToCore(taskFn, "fork_upd", 16384, &j, 1, nullptr, 0) != pdPASS) {
    vSemaphoreDelete(j.done);
    // Its 16 KB stack must be one piece of internal RAM (TLS can't run on a PSRAM stack).
    char m[64], ram[32];
    ramNote(ram, sizeof ram);
    snprintf(m, sizeof m, "out of memory (%s, need 16K)", ram);
    say(j, m);
    return j.kind == Kind::Latest ? -1 : 0;
  }
  xSemaphoreTake(j.done, portMAX_DELAY);
  vSemaphoreDelete(j.done);
  return j.result;
}

}  // namespace forkupd_dev

// ---- what the About page's worker calls in this fork ------------------------------
static int forkLatestRelease() {
  // Its failures before the request (no memory for the task) land in the reason too.
  s_verchk_why[0] = 0;
  forkupd_dev::Job j{forkupd_dev::Kind::Latest, 0, nullptr, nullptr, s_verchk_why, sizeof s_verchk_why, -1, nullptr};
  return forkupd_dev::run(j);
}

static bool forkInstallToSlot(int n, volatile int* pct, char* msg, size_t cap) {
  forkupd_dev::Job j{forkupd_dev::Kind::Slot, n, nullptr, pct, msg, cap, 0, nullptr};
  return forkupd_dev::run(j) == 1;
}

static bool forkSaveToSd(int n, const char* path, volatile int* pct, char* msg, size_t cap) {
  forkupd_dev::Job j{forkupd_dev::Kind::Sd, n, path, pct, msg, cap, 0, nullptr};
  return forkupd_dev::run(j) == 1;
}

// A JSON body sent with `method` (POST or DELETE) and a signature header; the
// reply into `resp` (NUL-ended). The HTTP status, or -1 when there was none
// (the reason in msg).
static int forkPostJson(const char* url, const char* method, const char* sig, const uint8_t* body, size_t len,
                        char* resp, size_t resp_cap, char* msg, size_t msg_cap) {
  forkupd_dev::Job j{forkupd_dev::Kind::Post, 0, nullptr, nullptr, msg, msg_cap, 0, nullptr};
  j.url = url;
  j.buf = (uint8_t*)resp;
  j.cap = resp_cap;
  j.method = method;
  j.sig = sig;
  j.body = body;
  j.body_len = len;
  j.code = -1;
  forkupd_dev::run(j);
  return j.code;
}

// A file of at most `cap` bytes, over HTTPS (redirects followed), into `buf`.
static bool forkFetchFile(const char* url, uint8_t* buf, size_t cap, size_t* got, char* msg, size_t msg_cap) {
  forkupd_dev::Job j{forkupd_dev::Kind::Fetch, 0, nullptr, nullptr, msg, msg_cap, 0, nullptr};
  j.url = url;
  j.buf = buf;
  j.cap = cap;
  const bool ok = forkupd_dev::run(j) == 1;
  if (got) *got = j.got;
  return ok;
}

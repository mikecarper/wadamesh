// SPDX-License-Identifier: GPL-3.0-or-later
// LinkImage: finding a picture's link in a message, and decoding each kind
// (JPEG, progressive JPEG, PNG with alpha, GIF, WebP lossy and lossless; BMP is not decoded)
// to RGB565 at the size asked for.
//   g++ -std=c++17 -I src test/test_link_image.cpp src/helpers/LinkImage.cpp
#include <vector>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <initializer_list>

#include "helpers/LinkImage.h"
#include "link_image_fixtures.h"

static int s_fail = 0;
#define CHECK(c)                                                  \
  do {                                                            \
    if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); s_fail++; } \
  } while (0)

static void* outAlloc(size_t n) { return malloc(n); }

static int r5(uint16_t p) { return p >> 11; }
static int g6(uint16_t p) { return (p >> 5) & 63; }
static int b5(uint16_t p) { return p & 31; }

static void url(const char* text, const char* want) {
  char out[200];
  const bool found = linkimg::findUrl(text, out, sizeof out);
  if (!want) {
    if (found) { fprintf(stderr, "found %s in \"%s\"\n", out, text); s_fail++; }
    return;
  }
  if (!found || strcmp(out, want)) {
    fprintf(stderr, "in \"%s\": got %s, want %s\n", text, found ? out : "(none)", want);
    s_fail++;
  }
}

// Decodes a fixture; left is red, right blue, and where the format keeps
// alpha the bottom-right quarter shows the background (white here).
static void picture(const char* name, const uint8_t* d, size_t n, linkimg::Kind kind, bool alpha, bool lossy) {
  CHECK(linkimg::sniff(d, n) == kind);
  int w = 0, h = 0;
  CHECK(linkimg::size(d, n, &w, &h) && w == 40 && h == 30);
  CHECK(linkimg::workBytes(d, n) > 40 * 30 * 4);

  uint16_t* px = nullptr;
  int ow = 0, oh = 0;
  // Full size, then scaled to 20 px wide (15 high).
  for (int side : {64, 20}) {
    const linkimg::Result r = linkimg::decode(d, n, side, 8u << 20, 0xFFFF, outAlloc, &px, &ow, &oh);
    if (r != linkimg::kOk) { fprintf(stderr, "%s: %s\n", name, linkimg::resultText(r)); s_fail++; continue; }
    const int ew = side >= 40 ? 40 : 20, eh = side >= 40 ? 30 : 15;
    CHECK(ow == ew && oh == eh);
    const int tol = lossy ? 4 : 1;
    const uint16_t left = px[(oh / 4) * ow + ow / 8];
    const uint16_t right_top = px[(oh / 4) * ow + ow - 1 - ow / 8];
    const uint16_t right_bottom = px[(oh - 1 - oh / 8) * ow + ow - 1 - ow / 8];
    if (!(r5(left) >= 31 - tol && g6(left) <= 2 * tol && b5(left) <= tol)) {
      fprintf(stderr, "%s @%d: left %04x not red\n", name, side, left); s_fail++;
    }
    if (!(b5(right_top) >= 31 - tol && r5(right_top) <= tol && g6(right_top) <= 2 * tol)) {
      fprintf(stderr, "%s @%d: right %04x not blue\n", name, side, right_top); s_fail++;
    }
    if (alpha && !(r5(right_bottom) >= 31 - tol && g6(right_bottom) >= 63 - 2 * tol && b5(right_bottom) >= 31 - tol)) {
      fprintf(stderr, "%s @%d: see-through %04x not the background\n", name, side, right_bottom); s_fail++;
    }
    free(px);
  }
  // Too little memory to decode: refused before trying.
  CHECK(linkimg::decode(d, n, 64, 1000, 0, outAlloc, &px, &ow, &oh) == linkimg::kTooLarge);
  // Cut short: refused, nothing left allocated (the sanitizer checks).
  if (n > 40) {
    const linkimg::Result r = linkimg::decode(d, n / 2, 64, 8u << 20, 0, outAlloc, &px, &ow, &oh);
    if (r == linkimg::kOk) free(px);
  }
}

int main() {
  // ---- links
  url("look https://i.imgur.com/AbC123.jpg nice", "https://i.imgur.com/AbC123.jpg");
  url("HTTP://Example.COM/a/B.JPEG", "HTTP://Example.COM/a/B.JPEG");
  url("(https://x.org/p.png).", "https://x.org/p.png");
  url("https://cdn.x.org/cat.webp?w=640&h=480 wow", "https://cdn.x.org/cat.webp?w=640&h=480");
  url("gif: https://media.site.net/a/b/c.gif#frag", "https://media.site.net/a/b/c.gif#frag");
  url("old https://x.org/scan.bmp!", nullptr);   // BMP isn't decoded
  url("page https://x.org/index.html then https://x.org/pic.jpg", "https://x.org/pic.jpg");
  url("https://x.org/jpg", nullptr);                 // no extension, just the word
  url("https://x.org/photo.jpg.html", nullptr);
  url("https://localhost/a.png", nullptr);           // host without a dot
  url("ftp://x.org/a.png", nullptr);
  url("www.x.org/a.png", nullptr);                   // a bare host isn't fetched
  url("https://x.org/?file=a.png", nullptr);         // the extension is in the query
  url("", nullptr);
  {
    char small[10];
    CHECK(!linkimg::findUrl("https://x.org/long.png", small, sizeof small));
  }
  CHECK(linkimg::urlId("https://x.org/a.png") == linkimg::urlId("https://x.org/a.png"));
  CHECK(linkimg::urlId("https://x.org/a.png") != linkimg::urlId("https://x.org/b.png"));
  CHECK(linkimg::urlId("") != 0);
  {   // image hosts that make a smaller copy are asked for one
    char f[300];
    CHECK(linkimg::fetchUrl("https://images.pexels.com/photos/30274227/pexels-photo-30274227.jpeg", 1024, f, sizeof f));
    CHECK(!strcmp(f, "https://images.pexels.com/photos/30274227/pexels-photo-30274227.jpeg?auto=compress&cs=tinysrgb&fm=jpg&w=1024"));
    CHECK(linkimg::fetchUrl("https://images.pexels.com/photos/1/a.jpeg?auto=compress&w=640", 1024, f, sizeof f));
    CHECK(!strcmp(f, "https://images.pexels.com/photos/1/a.jpeg?auto=compress&w=640"));   // already sized
    CHECK(linkimg::fetchUrl("https://images.unsplash.com/photo-1?ixlib=rb-4.0.3", 1024, f, sizeof f));
    CHECK(!strcmp(f, "https://images.unsplash.com/photo-1?ixlib=rb-4.0.3&w=1024&q=75&fm=jpg"));
    CHECK(linkimg::fetchUrl("https://x.org/cat.jpg#top", 1024, f, sizeof f));
    CHECK(!strcmp(f, "https://x.org/cat.jpg"));
    CHECK(linkimg::fetchUrl("https://x.org/cat.jpg", 1024, f, sizeof f) && !strcmp(f, "https://x.org/cat.jpg"));
    char tiny[20];
    CHECK(!linkimg::fetchUrl("https://images.pexels.com/photos/1/a.jpeg", 1024, tiny, sizeof tiny));
  }

  // ---- pictures
  picture("jpeg", k_jpeg, sizeof k_jpeg, linkimg::kJpeg, false, true);
  picture("progressive jpeg", k_jpeg_prog, sizeof k_jpeg_prog, linkimg::kJpeg, false, true);
  picture("png", k_png_alpha, sizeof k_png_alpha, linkimg::kPng, true, false);
  picture("gif", k_gif, sizeof k_gif, linkimg::kGif, false, false);
  CHECK(linkimg::sniff(k_bmp, sizeof k_bmp) == linkimg::kUnknown);   // BMP isn't decoded
  picture("webp lossy", k_webp_lossy, sizeof k_webp_lossy, linkimg::kWebp, false, true);
  picture("webp lossless", k_webp_lossless, sizeof k_webp_lossless, linkimg::kWebp, true, false);

  // ---- an animated GIF's frames: red 50 ms, green 10 ms (shown 100), blue 200 ms
  {
    static const uint8_t anim[] = {0x47,0x49,0x46,0x38,0x39,0x61,0x08,0x00,0x06,0x00,0x81,0x00,0x00,0xff,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x21,0xff,0x0b,0x4e,0x45,0x54,0x53,0x43,0x41,0x50,0x45,0x32,0x2e,0x30,0x03,0x01,0x00,0x00,0x00,0x21,0xf9,0x04,0x04,0x05,0x00,0x00,0x00,0x2c,0x00,0x00,0x00,0x00,0x08,0x00,0x06,0x00,0x00,0x08,0x0e,0x00,0x01,0x08,0x1c,0x48,0xb0,0xa0,0xc1,0x83,0x08,0x13,0x0e,0x0c,0x08,0x00,0x21,0xf9,0x04,0x05,0x01,0x00,0x01,0x00,0x2c,0x00,0x00,0x00,0x00,0x08,0x00,0x06,0x00,0x81,0x00,0xff,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x08,0x0e,0x00,0x01,0x08,0x1c,0x48,0xb0,0xa0,0xc1,0x83,0x08,0x13,0x0e,0x0c,0x08,0x00,0x21,0xf9,0x04,0x05,0x14,0x00,0x01,0x00,0x2c,0x00,0x00,0x00,0x00,0x08,0x00,0x06,0x00,0x81,0x00,0x00,0xff,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x08,0x0e,0x00,0x01,0x08,0x1c,0x48,0xb0,0xa0,0xc1,0x83,0x08,0x13,0x0e,0x0c,0x08,0x00,0x3b};
    uint16_t* fr = nullptr;
    uint16_t delays[8] = {};
    int fw = 0, fh = 0, fn = 0;
    CHECK(linkimg::decodeFrames(anim, sizeof anim, 64, 1u << 20, 0, 8, realloc, &fr, &fw, &fh, &fn, delays) == linkimg::kOk);
    CHECK(fw == 8 && fh == 6 && fn == 3);
    CHECK(fr && fr[0] == 0xF800 && fr[48] == 0x07E0 && fr[96] == 0x001F);
    CHECK(delays[0] == 50 && delays[1] == 100 && delays[2] == 200);
    free(fr);
    // Scaled down, and cut short to what's asked for.
    CHECK(linkimg::decodeFrames(anim, sizeof anim, 4, 1u << 20, 0, 2, realloc, &fr, &fw, &fh, &fn, delays) == linkimg::kOk);
    CHECK(fw == 4 && fh == 3 && fn == 2 && fr[0] == 0xF800 && fr[12] == 0x07E0);
    free(fr);
    // Many frames: the buffer grows by half each time, not a frame at a time (which
    // copied every earlier frame on each one), and ends at exactly the frames kept.
    {
      std::vector<uint8_t> many(anim, anim + sizeof anim - 1);   // without the trailer
      size_t last = 0;   // the last frame's block: its graphic control extension on
      for (size_t i = 0; i + 1 < sizeof anim; ++i)
        if (anim[i] == 0x21 && anim[i + 1] == 0xf9) last = i;
      for (int k = 0; k < 37; ++k) many.insert(many.end(), anim + last, anim + sizeof anim - 1);
      many.push_back(0x3b);
      static int calls = 0;
      static size_t last_size = 0;
      auto counting = [](void* p, size_t n) -> void* {
        ++calls;
        last_size = n;
        return realloc(p, n);
      };
      uint16_t many_delays[64] = {};
      CHECK(linkimg::decodeFrames(many.data(), many.size(), 64, 1u << 22, 0, 64, counting, &fr, &fw, &fh, &fn,
                                  many_delays) == linkimg::kOk);
      CHECK(fn == 40);
      CHECK(calls <= 12);   // was 40 (one per frame), each copying all before it
      CHECK(last_size == (size_t)40 * 8 * 6 * 2);
      CHECK(fr && fr[0] == 0xF800 && fr[(size_t)39 * 48] == 0x001F);
      free(fr);
    }
    // No room, not a GIF.
    CHECK(linkimg::decodeFrames(anim, sizeof anim, 64, 1000, 0, 8, realloc, &fr, &fw, &fh, &fn, delays) == linkimg::kTooLarge);
    CHECK(linkimg::decodeFrames(k_png_alpha, sizeof k_png_alpha, 64, 1u << 20, 0, 8, realloc, &fr, &fw, &fh, &fn, delays) ==
          linkimg::kNotPicture);
  }

  // ---- not pictures
  static const uint8_t html[] = "<!doctype html><html></html>";
  uint16_t* px = nullptr;
  int w = 0, h = 0;
  CHECK(linkimg::sniff(html, sizeof html) == linkimg::kUnknown);
  CHECK(linkimg::decode(html, sizeof html, 64, 1u << 20, 0, outAlloc, &px, &w, &h) == linkimg::kNotPicture);
  CHECK(!linkimg::size(html, sizeof html, &w, &h));
  CHECK(linkimg::workBytes(html, sizeof html) == 0);

  if (s_fail) { fprintf(stderr, "link_image: %d failed\n", s_fail); return 1; }
  printf("link_image: all passed\n");
  return 0;
}

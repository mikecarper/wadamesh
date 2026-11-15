// SPDX-License-Identifier: GPL-3.0-or-later
// ---- Repeater admin: MediaMesh (radio2) page ------------------------------------
// Included by UITask.cpp after the repeater admin console's state. Opened from the
// console's antenna button for the repeater we are logged in to. It talks to the
// repeater through the same remote CLI as the console, with the radio-profile
// commands of mikecarper/MeshCore (MEDIAMESH.md has the local equivalents):
//   get/set radio2, get radio, get radio2.cross (set off with a profile), get/set tx.reply,
//   get/set tempradio2, get radio2.status
// One command is in flight at a time, tagged "NN|" so its reply (the repeater
// echoes the tag) is told apart from the console's own; untagged and foreign
// replies still go to the console log. State lives in PSRAM while the page is open.

#include <stdarg.h>

static void prepSettingsPage(lv_obj_t* p);   // with the settings pages, further down

namespace rr2 {

enum Kind : uint8_t {
  K_RADIO2, K_MAIN, K_CROSS, K_REPLY, K_TEMP, K_STATUS,   // reads
  K_SET,                                                  // a write: OK or a refusal
};

struct Cmd { char text[88]; uint8_t kind; };

struct State {
  Cmd q[12];
  uint8_t qn;
  bool busy;
  Cmd now;                  // in flight
  char tag[4];
  char old_tag[4];          // a late reply to the attempt before is swallowed too
  uint8_t tries;
  uint32_t deadline;
  uint8_t next_tag;
  // What the repeater said; -1 / false = not (yet) known.
  int8_t supported;         // -1 asking, 0 no radio2, 1 yes
  bool r2_known, r2_on;
  char r2_mode[6];
  float r2_f, r2_bw;
  int r2_sf, r2_cr, r2_pre;   // r2_pre 0 = automatic
  bool main_known;
  float m_f, m_bw;
  int m_sf, m_cr;
  int8_t cross;             // 0 auto, 1 on, 2 off
  int8_t reply;             // 0 both, 1 radio, 2 radio2, 3 auto, 4 off
  bool reply_force, reply_unavail;
  bool temp_on;
  float temp_f;
  int temp_min;
  bool st_known, st_pending, st_ready;
  unsigned long rx0, rx1, tx0, tx1, sw, err;
  int budget, pre0, pre1;
  char warn[96];
  uint32_t st_at;           // millis() of the last status
  int8_t edit_mode;         // 0 off, 1 rx, 2 rxtx (the tiles; applied with the button)
  char line[64];            // the status line under the bands
};

static State* s = nullptr;
static uint8_t pub[32];
static lv_obj_t* root = nullptr;
static lv_timer_t* timer = nullptr;
// Widgets (all children of root; nulled on close).
static lv_obj_t *w_line, *w_main_f, *w_main_d, *w_r2_f, *w_r2_d, *w_r2_box, *w_link, *w_link_t;
static lv_obj_t *w_mode, *w_freq, *w_bw, *w_sf, *w_cr, *w_pre, *w_apply;
static lv_obj_t *w_reply, *w_force, *w_reply_note;
static lv_obj_t *w_temp, *w_temp_val;
static lv_obj_t *w_rx, *w_tx, *w_share, *w_sw, *w_pre_val, *w_sws, *w_warn, *w_warn_l, *w_updated;

static constexpr uint32_t kMagenta = 0xDE7FDB;   // radio2, as in the app

static const char* modeName(int m) { return m == 2 ? "rxtx" : m == 1 ? "rx" : "off"; }

static void render();
static void renderAgo();
static void pump();
static void fillFields();

static void push(uint8_t kind, const char* fmt, ...) {
  if (!s || s->qn >= sizeof(s->q) / sizeof(s->q[0])) return;
  Cmd& c = s->q[s->qn++];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(c.text, sizeof c.text, fmt, ap);
  va_end(ap);
  c.kind = kind;
  pump();
}

static void readAll() {
  push(K_RADIO2, "get radio2");
  push(K_MAIN, "get radio");
  push(K_CROSS, "get radio2.cross");
  push(K_REPLY, "get tx.reply");
  push(K_TEMP, "get tempradio2");
  push(K_STATUS, "get radio2.status");
}

static void readLive() {
  push(K_TEMP, "get tempradio2");
  push(K_STATUS, "get radio2.status");
}

static bool sendNow() {
  ContactInfo* c = the_mesh.lookupContactByPubKey(pub, PUB_KEY_SIZE);
  if (!c) return false;
  memcpy(s->old_tag, s->tag, sizeof s->tag);
  snprintf(s->tag, sizeof s->tag, "%02X|", (unsigned)(0xA0 + (s->next_tag++ & 0x3F)));
  char buf[96];
  snprintf(buf, sizeof buf, "%s%s", s->tag, s->now.text);
  const int r = the_mesh.uiSendAdminCommand(*c, buf);
  // A flood reply over a few hops at a slow SF takes a while; one retry after.
  s->deadline = millis() + 15000;
  return r == MSG_SEND_SENT_FLOOD || r == MSG_SEND_SENT_DIRECT;
}

static void pump() {
  if (!s || s->busy || s->qn == 0) return;
  s->now = s->q[0];
  memmove(&s->q[0], &s->q[1], (size_t)(s->qn - 1) * sizeof(Cmd));
  s->qn--;
  s->busy = true;
  s->tries = 0;
  snprintf(s->line, sizeof s->line, "%s %s", TR("Asking:"), s->now.text);
  if (!sendNow()) {
    s->busy = false;
    snprintf(s->line, sizeof s->line, "%s", TR("Could not send to the repeater"));
    s->qn = 0;
  }
  render();
}

// ---- reply parsing ------------------------------------------------------------------

static const char* value(const char* t) {   // after "> ", or nullptr for errors
  while (*t == ' ') t++;
  if (*t != '>') return nullptr;
  t++;
  while (*t == ' ') t++;
  return t;
}

static bool word(const char* v, const char* w) {
  const size_t n = strlen(w);
  return strncmp(v, w, n) == 0 && (v[n] == 0 || v[n] == ' ' || v[n] == ';' || v[n] == ',');
}

static unsigned long num(const char* v, const char* key) {
  const char* p = strstr(v, key);
  return p ? strtoul(p + strlen(key), nullptr, 10) : 0;
}

static void onRadio2(const char* t) {
  const char* v = value(t);
  if (!v) { s->supported = 0; s->qn = 0; return; }   // no radio2 here: stop asking
  s->supported = 1;
  s->r2_known = true;
  if (word(v, "off")) { s->r2_on = false; s->edit_mode = 0; return; }
  float f = 0, bw = 0;
  int sf = 0, cr = 0, used = 0;
  char mode[6] = {0};
  if (sscanf(v, "%f,%f,%d,%d,%5[a-z&]%n", &f, &bw, &sf, &cr, mode, &used) < 5) return;
  s->r2_on = strcmp(mode, "off") != 0;
  s->r2_f = f; s->r2_bw = bw; s->r2_sf = sf; s->r2_cr = cr <= 4 ? cr + 4 : cr;
  snprintf(s->r2_mode, sizeof s->r2_mode, "%s", strcmp(mode, "rx&tx") == 0 ? "rxtx" : mode);
  s->r2_pre = 0;
  if (v[used] == ',' && !strstr(v + used, "auto")) s->r2_pre = atoi(v + used + 1);
  s->edit_mode = !s->r2_on ? 0 : strcmp(s->r2_mode, "rx") == 0 ? 1 : 2;
}

static void onMain(const char* t) {
  const char* v = value(t);
  if (!v) return;
  float f = 0, bw = 0;
  int sf = 0, cr = 0;
  if (sscanf(v, "%f,%f,%d,%d", &f, &bw, &sf, &cr) == 4) {
    s->main_known = true;
    s->m_f = f; s->m_bw = bw; s->m_sf = sf; s->m_cr = cr <= 4 ? cr + 4 : cr;
  }
}

static void onCross(const char* t) {
  const char* v = value(t);
  if (!v) return;
  s->cross = word(v, "auto") ? 0 : word(v, "on") ? 1 : word(v, "off") ? 2 : -1;
}

static void onReply(const char* t) {
  const char* v = value(t);
  if (!v) return;
  static const char* const k[5] = { "both", "radio", "radio2", "auto", "off" };
  s->reply = -1;
  for (int i = 0; i < 5; i++) if (word(v, k[i])) s->reply = (int8_t)i;
  const char* semi = strchr(v, ';');
  const char* f = strstr(v, "force");
  s->reply_force = f && (!semi || f < semi);
  s->reply_unavail = strstr(v, "TX unavailable") != nullptr;
}

static void onTemp(const char* t) {
  const char* v = value(t);
  s->temp_on = false;
  if (!v || word(v, "off")) return;
  float f = 0;
  if (sscanf(v, "%f,", &f) != 1) return;
  s->temp_on = true;
  s->temp_f = f;
  const char* m = strstr(v, " min) left");
  if (m) {
    while (m > v && isdigit((unsigned char)m[-1])) m--;
    s->temp_min = atoi(m);
  }
}

static void onStatus(const char* t) {
  const char* v = value(t);
  if (!v) return;
  s->st_known = true;
  s->st_at = millis();
  s->st_pending = strncmp(v, "radio2 change pending", 21) == 0;
  if (s->st_pending) return;
  s->rx0 = num(v, "RX="); s->tx0 = num(v, "TX=");
  const char* p = strstr(v, "RX=");
  s->rx1 = p && strchr(p, ',') ? strtoul(strchr(p, ',') + 1, nullptr, 10) : 0;
  p = strstr(v, "TX=");
  s->tx1 = p && strchr(p, ',') ? strtoul(strchr(p, ',') + 1, nullptr, 10) : 0;
  s->sw = num(v, "switches="); s->err = num(v, "errors=");
  s->budget = (int)num(v, "budget=");
  s->st_ready = strstr(v, " ready") != nullptr;
  p = strstr(v, "preamble=");
  s->pre0 = s->pre1 = 0;
  if (p) sscanf(p + 9, "%d,%d", &s->pre0, &s->pre1);
  s->warn[0] = 0;
  const char* w = strstr(v, "WARN ");
  if (!w) w = strstr(v, "short override");
  if (w) {
    if (strncmp(w, "WARN ", 5) == 0) w += 5;
    snprintf(s->warn, sizeof s->warn, "%s", w);
  }
}

static void onSet(const char* t) {
  while (*t == ' ') t++;
  if (strncmp(t, "OK", 2) == 0) {
    if (g_lv.task) g_lv.task->showAlert(TR("Saved on the repeater"), 1200);
    return;
  }
  s->qn = 0;   // the reads queued after it would show the old state as new
  if (!g_lv.task) return;
  if (strstr(t, "local USB")) {
    g_lv.task->showAlert(TR("This repeater only takes radio2 changes over USB"), 3000);
  } else {
    char m[120];
    snprintf(m, sizeof m, "%s %s", TR("Refused:"), strncmp(t, "Error:", 6) == 0 ? t + 7 : t);
    g_lv.task->showAlert(m, 3000);
  }
  readAll();   // show what it has now
}

// The console hands every reply from the logged-in repeater here first.
static bool onAdminReply(const ContactInfo& from, const char* text) {
  if (!s || !root || !text || memcmp(from.id.pub_key, pub, 32) != 0) return false;
  if (strlen(text) < 3 || text[2] != '|') return false;
  const bool ours = strncmp(text, s->tag, 3) == 0;
  if (!ours) return strncmp(text, s->old_tag, 3) == 0 && s->old_tag[0];   // late: drop
  if (!s->busy) return true;
  const char* body = text + 3;
  switch (s->now.kind) {
    case K_RADIO2: onRadio2(body); fillFields(); break;
    case K_MAIN:   onMain(body); break;
    case K_CROSS:  onCross(body); break;
    case K_REPLY:  onReply(body); break;
    case K_TEMP:   onTemp(body); break;
    case K_STATUS: onStatus(body); break;
    case K_SET:    onSet(body); break;
  }
  s->busy = false;
  s->line[0] = 0;
  if (s->qn == 0) snprintf(s->line, sizeof s->line, "%s", TR("Up to date"));
  pump();
  render();
  return true;
}

static void tick(lv_timer_t*) {
  if (!s) return;
  if (!s->busy) { renderAgo(); return; }
  if ((int32_t)(millis() - s->deadline) < 0) return;
  if (s->tries++ < 1 && sendNow()) {
    snprintf(s->line, sizeof s->line, "%s %s", TR("Asking again:"), s->now.text);
  } else {
    s->busy = false;
    s->qn = 0;
    if (s->supported < 0) s->supported = -2;   // never answered
    snprintf(s->line, sizeof s->line, "%s", TR("No answer from the repeater"));
  }
  render();
}

// ---- rendering ------------------------------------------------------------------------

static void setText(lv_obj_t* l, const char* t) { if (l && lv_obj_is_valid(l)) lv_label_set_text(l, t); }

static void fmtProfile(char* out, size_t n, float bw, int sf, int cr) {
  snprintf(out, n, "BW %g  SF%d  CR%d", bw, sf, cr);
}

static void render() {
  if (!s || !root) return;
  char b[96];
  // The bands.
  if (s->main_known) {
    snprintf(b, sizeof b, "%.3f", s->m_f); setText(w_main_f, b);
    fmtProfile(b, sizeof b, s->m_bw, s->m_sf, s->m_cr); setText(w_main_d, b);
  }
  if (s->r2_known && s->r2_on) {
    snprintf(b, sizeof b, "%.3f", s->r2_f); setText(w_r2_f, b);
    fmtProfile(b, sizeof b, s->r2_bw, s->r2_sf, s->r2_cr);
    size_t o = strlen(b);
    snprintf(b + o, sizeof b - o, "\n%s", strcmp(s->r2_mode, "rx") == 0 ? TR("listening") : TR("listen + send"));
    setText(w_r2_d, b);
  } else if (s->r2_known) {
    setText(w_r2_f, TR("Off")); setText(w_r2_d, "");
  }
  if (w_r2_box) lv_obj_set_style_opa(w_r2_box, s->r2_known && s->r2_on ? LV_OPA_COVER : LV_OPA_50, LV_PART_MAIN);
  {
    const bool on = s->r2_known && s->r2_on;
    const char* t = !on ? "" : s->cross == 1 ? TR("bridged") : s->cross == 2 ? TR("separate") : s->cross == 0 ? TR("auto") : "";
    const uint32_t col = !on ? COLOR_TERTIARY : s->cross == 1 ? COLOR_STATUS_OK : s->cross == 2 ? COLOR_STATUS_WARN : COLOR_STATUS_INFO;
    setText(w_link, !on ? "-" : s->cross == 2 ? LV_SYMBOL_CLOSE : LV_SYMBOL_SHUFFLE);
    setText(w_link_t, t);
    if (w_link) lv_obj_set_style_text_color(w_link, lv_color_hex(col), LV_PART_MAIN);
    if (w_link_t) lv_obj_set_style_text_color(w_link_t, lv_color_hex(col), LV_PART_MAIN);
  }
  {
    const char* head = s->supported == 0 ? TR("This repeater has no radio2: its firmware needs radio profiles (mikecarper/MeshCore).")
                     : s->supported == -2 ? TR("No answer. Still logged in and in range?")
                                      : nullptr;
    if (head) snprintf(b, sizeof b, "%s", head);
    else if (s->temp_on) snprintf(b, sizeof b, "%s %.3f MHz, %d min %s  %s", TR("Temporary"), s->temp_f, s->temp_min, TR("left"), s->line);
    else snprintf(b, sizeof b, "%s", s->line);
    setText(w_line, b);
  }
  // Profile.
  setChoicePick(w_mode, s->edit_mode ? 1 : 0, s->supported == 1);
  if (w_apply) {
    if (s->busy || s->supported != 1) lv_obj_add_state(w_apply, LV_STATE_DISABLED);
    else lv_obj_clear_state(w_apply, LV_STATE_DISABLED);
  }
  // Replies.
  setChoicePick(w_reply, s->reply >= 0 && s->reply <= 3 ? s->reply : -1, s->supported == 1);
  if (w_force) {
    if (s->reply_force) lv_obj_add_state(w_force, LV_STATE_CHECKED);
    else lv_obj_clear_state(w_force, LV_STATE_CHECKED);
    setSwitchEnable(w_force, s->supported == 1 && (s->reply == 0 || s->reply == 2));
  }
  if (w_reply_note) {
    if (s->reply_unavail) lv_obj_clear_flag(w_reply_note, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(w_reply_note, LV_OBJ_FLAG_HIDDEN);
  }
  // Temporary.
  if (s->temp_on) snprintf(b, sizeof b, "%.3f MHz, %d min %s", s->temp_f, s->temp_min, TR("left"));
  else snprintf(b, sizeof b, "%s", TR("None"));
  setText(w_temp_val, b);
  // Live.
  if (s->st_known && s->st_pending) {
    setText(w_rx, TR("change pending")); setText(w_tx, "-");
  } else if (s->st_known) {
    snprintf(b, sizeof b, "%lu / %lu", s->rx0, s->rx1); setText(w_rx, b);
    snprintf(b, sizeof b, "%lu / %lu", s->tx0, s->tx1); setText(w_tx, b);
    if (s->st_ready && s->budget) snprintf(b, sizeof b, "%d us", s->budget);
    else snprintf(b, sizeof b, "%s", TR("measuring"));
    setText(w_sw, b);
    snprintf(b, sizeof b, "%d / %d", s->pre0, s->pre1); setText(w_pre_val, b);
    if (s->err) snprintf(b, sizeof b, "%lu (%lu %s)", s->sw, s->err, TR("failed"));
    else snprintf(b, sizeof b, "%lu", s->sw);
    setText(w_sws, b);
    const unsigned long tot = s->rx0 + s->rx1;
    if (w_share) lv_bar_set_value(w_share, tot ? (int32_t)(s->rx1 * 100 / tot) : 0, LV_ANIM_OFF);
  }
  if (w_warn) {
    if (s->warn[0]) { lv_obj_clear_flag(w_warn, LV_OBJ_FLAG_HIDDEN); setText(w_warn_l, s->warn); }
    else lv_obj_add_flag(w_warn, LV_OBJ_FLAG_HIDDEN);
  }
  renderAgo();
}

static void renderAgo() {
  if (!s || !root || !s->st_known) return;
  char b[40];
  const uint32_t ago = (millis() - s->st_at) / 1000;
  if (ago < 60) snprintf(b, sizeof b, "%s %lus %s", TR("Updated"), (unsigned long)ago, TR("ago"));
  else snprintf(b, sizeof b, "%s %lu min %s", TR("Updated"), (unsigned long)(ago / 60), TR("ago"));
  setActionValue(w_updated, b);
}

// ---- actions --------------------------------------------------------------------------

static void modeCb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED || !s) return;
  // Off or On (listen and send). A listen-only radio2 set from the Terminal
  // shows as On; tapping On makes it send too.
  s->edit_mode = setChoiceIndex(e) == 1 ? 2 : 0;
  render();
}

static void applyNow() {
  if (!s) return;
  if (s->edit_mode == 0) { push(K_SET, "set radio2 off"); readAll(); return; }
  float f = 0, bw = 0;
  int sf = 0, cr = 0, pre = 0;
  kbMirrorSyncToReal();
  if (!parseFloatField(w_freq, f) || !parseFloatField(w_bw, bw) || !parseIntField(w_sf, sf) || !parseIntField(w_cr, cr)) {
    if (g_lv.task) g_lv.task->showAlert(TR("Fill in frequency, bandwidth, SF and CR"), 1800);
    return;
  }
  if (parseIntField(w_pre, pre) && (pre < 8 || pre > 512)) pre = 0;
  if (s->main_known && fabsf(f - s->m_f) < 0.0005f && fabsf(bw - s->m_bw) < 0.01f && sf == s->m_sf) {
    if (g_lv.task) g_lv.task->showAlert(TR("radio2 must differ from the main radio"), 1800);
    return;
  }
  char pb[12] = "";
  if (pre) snprintf(pb, sizeof pb, ",%d", pre);
  push(K_SET, "set radio2 %.3f,%g,%d,%d,%s%s", f, bw, sf, cr, modeName(s->edit_mode), pb);
  // radio2 traffic never crosses onto the main mesh, nor main-mesh traffic onto
  // radio2: there's no bridging choice, and "auto" (the firmware's default)
  // bridges a saved radio2.
  if (s->cross != 2) push(K_SET, "set radio2.cross off");
  readAll();
}

static void applyCb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED || !s || s->busy) return;
  hideKb();
  showConfirm(s->edit_mode == 0
                  ? TR("Turn radio2 off on the repeater?")
                  : TR("Change radio2 on the repeater? It answers first, then switches. If you reach it over radio2 you may lose contact."),
              s->edit_mode == 0 ? TR("Turn off") : TR("Apply"), applyNow);
}

static void replyCb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED || !s) return;
  static const char* const v[4] = { "both", "radio", "radio2", "auto" };
  const int i = setChoiceIndex(e);
  if (i < 0 || i > 3) return;
  const bool force = s->reply_force && (i == 0 || i == 2);
  push(K_SET, "set tx.reply %s%s", v[i], force ? " force" : "");
  push(K_REPLY, "get tx.reply");
}

static void forceCb(lv_event_t* e) {
  if (!s) return;
  lv_obj_t* sw = lv_event_get_target(e);
  const bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
  if (s->reply != 0 && s->reply != 2) { render(); return; }
  push(K_SET, "set tx.reply %s%s", s->reply == 0 ? "both" : "radio2", on ? " force" : "");
  push(K_REPLY, "get tx.reply");
}

static void tempCb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED || !s) return;
  static const uint16_t kMin[4] = { 15, 60, 120, 360 };
  const int i = setChoiceIndex(e);
  if (i == 4) {
    push(K_SET, "set tempradio2 off");
  } else if (i >= 0 && i < 4) {
    float f = 0, bw = 0;
    int sf = 0, cr = 0;
    kbMirrorSyncToReal();
    if (!parseFloatField(w_freq, f) || !parseFloatField(w_bw, bw) || !parseIntField(w_sf, sf) || !parseIntField(w_cr, cr)) {
      if (g_lv.task) g_lv.task->showAlert(TR("Fill in the profile above first"), 1600);
      return;
    }
    push(K_SET, "set tempradio2 %.3f,%g,%d,%d,%s,%u", f, bw, sf, cr,
         s->edit_mode == 1 ? "rx" : "rxtx", (unsigned)kMin[i]);
  }
  readLive();
}

static void refreshCb(lv_event_t* e) {
  if (lv_event_get_code(e) == LV_EVENT_CLICKED && s && !s->busy) readLive();
}

void closePage() {
  if (timer) { lv_timer_del(timer); timer = nullptr; }
  if (root) { hideKb(); popupClose(&root); }
  root = nullptr;
  w_line = w_main_f = w_main_d = w_r2_f = w_r2_d = w_r2_box = w_link = w_link_t = nullptr;
  w_mode = w_freq = w_bw = w_sf = w_cr = w_pre = w_apply = nullptr;
  w_reply = w_force = w_reply_note = nullptr;
  w_temp = w_temp_val = nullptr;
  w_rx = w_tx = w_share = w_sw = w_pre_val = w_sws = w_warn = w_warn_l = w_updated = nullptr;
  if (s) { s->qn = 0; s->busy = false; }
}

// One band: its name, the frequency large, the rest small.
static lv_obj_t* band(lv_obj_t* parent, const char* name, uint32_t hue, lv_coord_t w, lv_obj_t** f, lv_obj_t** d) {
  lv_obj_t* b = setBox(parent);
  lv_obj_set_size(b, w, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(b, lv_color_hex(hue), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(b, s_theme_high_contrast ? LV_OPA_TRANSP : 28, LV_PART_MAIN);
  lv_obj_set_style_border_color(b, lv_color_hex(hue), LV_PART_MAIN);
  lv_obj_set_style_border_width(b, 1, LV_PART_MAIN);
  lv_obj_set_style_radius(b, SC(8), LV_PART_MAIN);
  lv_obj_set_style_pad_all(b, SC(6), LV_PART_MAIN);
  lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(b, 2, LV_PART_MAIN);
  lv_obj_t* n = lv_label_create(b);
  lv_label_set_text(n, name);
  lv_obj_set_style_text_font(n, &g_font_semi_12, LV_PART_MAIN);
  lv_obj_set_style_text_color(n, lv_color_hex(hue), LV_PART_MAIN);
  *f = lv_label_create(b);
  lv_label_set_text(*f, "...");
  lv_obj_set_style_text_font(*f, &g_font_semi_16, LV_PART_MAIN);
  lv_obj_set_style_text_color(*f, lv_color_hex(COLOR_TEXT), LV_PART_MAIN);
  *d = lv_label_create(b);
  lv_label_set_text(*d, "");
  lv_obj_set_width(*d, w - 2 * SC(6) - 2);
  lv_label_set_long_mode(*d, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_font(*d, &g_font_12, LV_PART_MAIN);
  lv_obj_set_style_text_color(*d, lv_color_hex(COLOR_SUB), LV_PART_MAIN);
  return b;
}

static void build(lv_obj_t* page) {
  s_settings_inline_parent = page;

  // Both radios side by side, joined by the bridging state, and the status line.
  {
    lv_obj_t* c = setCard("");
    lv_obj_t* row = setBox(c);
    const lv_coord_t w = s_settings_content_w, mid = SC(44), bw = (w - mid) / 2;
    lv_obj_set_size(row, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    band(row, TR("MAIN"), COLOR_STATUS_INFO, bw, &w_main_f, &w_main_d);
    lv_obj_t* m = setBox(row);
    lv_obj_set_size(m, mid, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(m, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(m, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    w_link = lv_label_create(m);
    lv_label_set_text(w_link, "-");
    lv_obj_set_style_text_font(w_link, &g_font_16, LV_PART_MAIN);
    w_link_t = lv_label_create(m);
    lv_label_set_text(w_link_t, "");
    lv_obj_set_style_text_font(w_link_t, &g_font_12, LV_PART_MAIN);
    w_r2_box = band(row, "RADIO2", kMagenta, bw, &w_r2_f, &w_r2_d);
    w_line = setNote(c, "");
  }

  // Live figures.
  {
    lv_obj_t* c = setCard("Live");
    w_rx = setInfoRow(c, "Heard main / radio2", "-");
    w_tx = setInfoRow(c, "Sent main / radio2", "-");
    {
      lv_obj_t* hd = setBox(c);
      lv_obj_set_size(hd, s_settings_content_w, LV_SIZE_CONTENT);
      setRowText(hd, nullptr, "Share of packets heard on radio2", s_settings_content_w - 4, 0);
      w_share = lv_bar_create(c);
      lv_obj_set_size(w_share, s_settings_content_w, SC(8));
      lv_bar_set_range(w_share, 0, 100);
      lv_obj_set_style_bg_color(w_share, lv_color_hex(COLOR_STATUS_INFO), LV_PART_MAIN);
      lv_obj_set_style_bg_opa(w_share, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_bg_color(w_share, lv_color_hex(kMagenta), LV_PART_INDICATOR);
    }
    w_sw = setInfoRow(c, "Switch time", "-");
    w_pre_val = setInfoRow(c, "Preamble main / radio2", "-");
    w_sws = setInfoRow(c, "Profile switches", "-");
    w_warn = setNote(c, " ", SET_TONE_WARN);
    w_warn_l = lv_obj_get_child(w_warn, 1);
    lv_obj_add_flag(w_warn, LV_OBJ_FLAG_HIDDEN);
    w_updated = setActionRow(c, UI_ICON_REFRESH_CW, "Refresh", "", refreshCb, nullptr, SET_NORMAL, false);
  }

  // The second profile.
  {
    lv_obj_t* c = setCard("radio2 profile");
    const char* opts[2] = { TR("Off"), TR("On") };
    w_mode = setChoiceRow(c, nullptr, "On listens and sends on radio2. Applies with the button below.",
                          opts, 2, s->edit_mode ? 1 : 0, modeCb);
    lv_obj_t* r1 = setFieldRow(c);
    w_freq = setField(r1, "Frequency (MHz)", "MHz", 10, setFieldW(2));
    w_bw = setField(r1, "Bandwidth (kHz)", "kHz", 7, setFieldW(2));
    lv_obj_t* r2 = setFieldRow(c);
    w_sf = setField(r2, "SF", "SF", 2, setFieldW(3));
    w_cr = setField(r2, "CR", "CR", 1, setFieldW(3));
    w_pre = setField(r2, "Preamble", "auto", 3, setFieldW(3));
    setNote(c, "Leave the preamble empty and the repeater picks it. Everyone on radio2 must use the same settings.");
    w_apply = setButton(c, UI_ICON_SEND_HORIZONTAL, "Apply to repeater", applyCb, nullptr, SET_PRIMARY);
  }

  // Where the repeater's own replies go. (No bridging choice: radio2 is
  // always kept separate from the main mesh, applyNow.)
  {
    lv_obj_t* c = setCard("Replies to admins");
    const char* opts[4] = { TR("Both"), TR("Main"), TR("radio2"), TR("Auto") };
    w_reply = setChoiceRow(c, nullptr, "Where logins, CLI answers and ACKs go out. Both is the default.", opts, 4, -1, replyCb);
    w_force = setSwitchRow(c, "Also on a listen-only radio2", "For Both or radio2 while radio2 is set to Listen.",
                           false, forceCb);
    w_reply_note = setNote(c, "radio2 can't send right now, so replies only go out on the main radio.", SET_TONE_WARN);
    lv_obj_add_flag(w_reply_note, LV_OBJ_FLAG_HIDDEN);
  }

  // A temporary radio2.
  {
    lv_obj_t* c = setCard("Temporary radio2");
    setNote(c, "Runs the profile above without saving it, for example to meet another group. The saved radio2 comes back after, or on a reboot.");
    const char* opts[5] = { TR("15 min"), TR("1 h"), TR("2 h"), TR("6 h"), TR("Stop") };
    w_temp = setChoiceRow(c, nullptr, nullptr, opts, 5, -1, tempCb);
    w_temp_val = setInfoRow(c, "Running now", "-");
  }
  s_settings_inline_parent = nullptr;
}

static void fillFields() {
  if (!s || !root || !s->r2_known || !w_freq) return;
  char b[16];
  const bool have = s->r2_on;
  snprintf(b, sizeof b, "%.3f", have ? s->r2_f : mediamesh::kDefaultFreq); lv_textarea_set_text(w_freq, b);
  snprintf(b, sizeof b, "%g", have ? s->r2_bw : mediamesh::kDefaultBw); lv_textarea_set_text(w_bw, b);
  snprintf(b, sizeof b, "%d", have ? s->r2_sf : 7); lv_textarea_set_text(w_sf, b);
  snprintf(b, sizeof b, "%d", have ? s->r2_cr : 5); lv_textarea_set_text(w_cr, b);
  if (s->r2_pre) { snprintf(b, sizeof b, "%d", s->r2_pre); lv_textarea_set_text(w_pre, b); }
  else lv_textarea_set_text(w_pre, "");
}

void openPage(const uint8_t* pub32, const char* name) {
  closePage();
  if (!s) s = (State*)psAlloc(sizeof(State));
  if (!s) { if (g_lv.task) g_lv.task->showAlert(TR("Out of memory"), 1500); return; }
  memset(s, 0, sizeof *s);
  s->supported = -1;
  s->cross = s->reply = -1;
  memcpy(pub, pub32, 32);

  const lv_coord_t sw = lv_disp_get_hor_res(nullptr), sh = lv_disp_get_ver_res(nullptr);
  root = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(root);
  lv_obj_set_size(root, sw, sh - STATUSBAR_H);
  lv_obj_set_pos(root, 0, STATUSBAR_H);
  styleSurface(root, COLOR_BG, 0);
  lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* hdr = lv_obj_create(root);
  lv_obj_remove_style_all(hdr);
  lv_obj_set_size(hdr, sw, 36);
  styleSurface(hdr, COLOR_PANEL, 0);
  lv_obj_set_style_border_side(hdr, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
  lv_obj_set_style_border_width(hdr, 1, LV_PART_MAIN);
  lv_obj_set_style_border_color(hdr, lv_color_hex(COLOR_BORDER), LV_PART_MAIN);
  lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t* title = lv_label_create(hdr);
  char tb[48];
  snprintf(tb, sizeof tb, "MediaMesh: %s", name);
  lv_label_set_text(title, tb);
  lv_obj_set_style_text_font(title, &g_font_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT), LV_PART_MAIN);
  lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
  lv_obj_set_width(title, sw - 8 - 36);
  lv_obj_align(title, LV_ALIGN_LEFT_MID, 8, 0);
  addCloseXBadge(hdr, +[](lv_event_t* e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) closePage();
  });

  lv_obj_t* page = lv_obj_create(root);
  lv_obj_set_pos(page, 0, 36);
  lv_obj_set_size(page, sw, sh - STATUSBAR_H - 36);
  prepSettingsPage(page);
  build(page);
  snprintf(s->line, sizeof s->line, "%s", TR("Asking the repeater..."));
  render();
  timer = lv_timer_create(tick, 1000, nullptr);
  readAll();
}

}  // namespace rr2

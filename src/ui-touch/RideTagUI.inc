// SPDX-License-Identifier: GPL-3.0-or-later
// A Keymind RAK collar addresses its authenticated check to one owner radio.
#include "RideTag.h"
namespace bikeui {
static bike::RideTag tag;
static bool tagLoaded;
static void tagPage();

static void loadTag() {
  if (tagLoaded) return;
  SdNvsPrefs p;
  if (!p.begin("bike", true)) return;
  bike::RideTag stored;
  if (p.getBytes("tag1", &stored, sizeof(stored)) == sizeof(stored) &&
      stored.enabled <= 1 && stored.answer <= mesh::tracker::Lost) tag = stored;
  p.end();
  tagLoaded = true;
}
static bool saveTag(const bike::RideTag& v) {
  SdNvsPrefs p;
  if (!p.begin("bike", false)) return false;
  bool ok = p.putBytes("tag1", &v, sizeof(v)) == sizeof(v);
  p.end();
  if (ok) { tag = v; tagLoaded = true; }
  return ok;
}
static uint8_t receiveTag(const ContactInfo& from, uint32_t ts, const uint8_t* data, uint8_t len, uint8_t* reply) {
  if (!reply || (from.type != ADV_TYPE_CHAT && from.type != ADV_TYPE_SENSOR)) return 0;
  loadTag();
  bike::RideTag next = tag;
  if (!next.accept(from.id.pub_key, ts, the_mesh.getRTCClock()->getCurrentTime(), data, len) || !saveTag(next)) return 0;
  return mesh::tracker::makeStatusResponse(ts, next.answer, next.answer == mesh::tracker::Lost ? 120 : 0,
                                           reply, mesh::tracker::ResponseLength);
}
static void pickTag(int first) {
  auto* card = sheetOpen("Choose a RAK tag", "Select the saved contact for the tracker. Set the tag's owner to this radio's full public key.");
  int i = first, shown = 0;
  for (; i < (int)the_mesh.getNumContacts() && shown < 6; ++i) {
    ContactInfo c;
    if (!the_mesh.getContactByIdx(i, c) || (c.type != ADV_TYPE_SENSOR && c.type != ADV_TYPE_CHAT)) continue;
    ++shown;
    char label[60];
    snprintf(label, sizeof(label), "%.32s [%02x%02x%02x]", c.name, c.id.pub_key[0], c.id.pub_key[1], c.id.pub_key[2]);
    // The contact list can change while this sheet is open; retain the displayed identity.
    auto* key = (uint8_t*)lv_mem_alloc(32);
    if (!key) continue;
    memcpy(key, c.id.pub_key, 32);
    auto* button = sheetBtn(card, label, [](lv_event_t* e) {
      const auto* key = (const uint8_t*)lv_event_get_user_data(e);
      if (tag.enabled && !memcmp(key, tag.key, 32)) { tagPage(); return; }
      bike::RideTag next;
      memcpy(next.key, key, 32); next.enabled = 1;
      if (saveTag(next)) tagPage(); else toast("Could not save tracker");
    }, key);
    lv_obj_add_event_cb(button, [](lv_event_t* e) { lv_mem_free(lv_event_get_user_data(e)); }, LV_EVENT_DELETE, key);
  }
  if (i < (int)the_mesh.getNumContacts()) sheetBtn(card, "More", [](lv_event_t* e) {
    pickTag((intptr_t)lv_event_get_user_data(e));
  }, (void*)(intptr_t)i);
  if (first) sheetBtn(card, "First page", [](lv_event_t*) { pickTag(0); });
  sheetBtn(card, "Back", [](lv_event_t*) { tagPage(); });
}
static void tagPage() {
  loadTag();
  char body[220] = "Choose a tracker contact to receive private check-ins.";
  if (tag.enabled) {
    char last[100] = "Waiting for the first check.";
    if (tag.heard) {
      uint32_t now = the_mesh.getRTCClock()->getCurrentTime();
      snprintf(last, sizeof(last), "Heard %lu s ago, %u mV. %s", (unsigned long)(now > tag.heard ? now - tag.heard : 0),
               tag.battery_mv, tag.position == tag.heard ? "Fresh GPS at that check." : tag.position ? "Map shows the previous fix." : "Waiting for GPS.");
    }
    ContactInfo* c = the_mesh.lookupContactByPubKey(tag.key, sizeof(tag.key));
    snprintf(body, sizeof(body), "%.32s [%02x%02x%02x]. Answer: %s.\n%s", c ? c->name : "RAK tag", tag.key[0], tag.key[1], tag.key[2],
             tag.answer == mesh::tracker::Lost ? "Lost" : "Safe", last);
  }
  auto* card = sheetOpen("RAK tag monitor", body);
  if (tag.enabled) {
    sheetBtn(card, tag.answer == mesh::tracker::Lost ? "Mark safe" : "Mark lost", [](lv_event_t*) {
      bike::RideTag next = tag;
      next.answer = tag.answer == mesh::tracker::Lost ? mesh::tracker::Safe : mesh::tracker::Lost;
      if (saveTag(next)) tagPage(); else toast("Could not save status");
    });
    if (tag.position) sheetBtn(card, "Last fix on map", [](lv_event_t*) { sheetClose(); teamui::showOnMap(tag.latitude / 1e7, tag.longitude / 1e7); });
    sheetBtn(card, "Forget tag", [](lv_event_t*) { if (saveTag({})) tagPage(); else toast("Could not forget tracker"); });
    sheetBtn(card, "Refresh", [](lv_event_t*) { tagPage(); });
  }
  sheetBtn(card, "Choose tag", [](lv_event_t*) { pickTag(0); });
  sheetBtn(card, "Back", [](lv_event_t*) { open(); });
}
}

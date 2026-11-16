# Team presence

How teammates stay on the Team app's roster and map, in step with MeshCore Open TAK (the app's `documentation/team-presence.md`).

## Each beacon is new

Team traffic is channel data (GRP_DATA `0x5654`), which carries no timestamp, and channel encryption gives the same bytes for the same message. Every MeshCore radio drops a packet it has seen recently, and it remembers its last 160. A standalone with a hand-set position, or one that isn't moving, sent the same beacon every 10 minutes. Repeaters and teammates dropped every one after the first, so the standalone went stale on everyone else's map and then disappeared, while still sending.

Two changes make every send new bytes. Older readers ignore both.

- **Beacon number:** each beacon carries one that goes up every time, as position flag `0x80` with a `u16` at the end. It starts from a random value at boot.
- **Alert repeats:** each send of an alert (three) and of its cancel (two) carries its own number, as alert flag `0x04` with a byte after the note.

## No position

A standalone with no GPS fix and no location set used to send no beacon at all. It never showed on anyone's Team list, though its SOS still came through, because alerts don't need a position. It now sends a "here, no position" beacon: the usual position message with latitude and longitude both `0x7FFFFFFF` (`TeamWire::kNoPosition`), out of range on purpose so older builds reject it as they always did.

- It goes out when the standalone joins, or a teammate is first heard, then every 10 minutes, or every minute while its own alert is up.
- Receivers list the member with **no position** instead of a range, after everyone who has one. The sheet drops Hound and Map.
- They aren't drawn on the map, and don't count for the nearest-teammate widget or a message's reach map.
- A position heard from them before stays (their last known), while battery, status and SOS update.

## Live, stale, lost

| | Last beacon |
| --- | --- |
| Live | under 25 minutes ago (two missed still beacons) |
| Stale | under an hour ago |
| Lost | older (not drawn on the map) |

These were 12 and 30 minutes, which was under two still beacons, so a single lost packet made a teammate stale.

Code: `src/ui-touch/TeamWire.h` (the formats), `src/ui-touch/TeamCore.cpp` (beacons, alerts, freshness). Tests: `test/test_team_wire.cpp`, with golden bytes the app's `test/team/team_wire_golden_test.dart` checks too.

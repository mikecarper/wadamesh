# MediaMesh and radio2: a second LoRa profile

MediaMesh puts a second LoRa profile (`radio2`) on the same radio. Its main use
is moving the photos and voice clips that
[MeshCore Open TAK](https://github.com/jade-on-mesh/meshcore-open-tak) sends onto
their own, faster channel while text, adverts and ACKs stay on the main mesh.
The radio listens to both, so a node with MediaMesh on still hears the main mesh
and works with stock repeaters.

It is the two-profile design of
[mikecarper/MeshCore](https://github.com/mikecarper/MeshCore/blob/keymindCascade/docs/radio_profiles.md),
with the same commands and replies, so an app can drive either firmware the same
way. Everything there is here except Meshtastic chat (`mt.*`, `#mt-` channels)
and the infrastructure-only `tx.reply`.

## Turning it on

**Settings → Radio → MediaMesh Settings**, under the main Radio card.

The **radio2 (MediaMesh)** card:

- **radio2**: Off / On. On listens and sends there (`rxtx`). Listen only (`rx`)
  is still available from the Terminal (`set radio2 ...,rx`); it shows as On,
  and tapping On makes it send too. Turning it on from untouched settings also
  picks Photos & voice below.
- **Sent on radio2**: *Photos & voice* (`tx.media radio2`, `radio2.cross off`).
  Any other mix shows *Custom*; set `tx.media` in the Terminal. Turning radio2
  on or applying a preset always sets `radio2.cross off`: the two meshes are
  never bridged from the settings pages.
- **Preset**: *250 kHz fast* (plus *2.4 GHz* on LR1121 / LR2021 radios).
  Picking it fills in bandwidth, SF7 and CR 4/5 and leaves the frequency alone;
  it shows as picked whenever the fields match. The old *125 kHz range* preset
  is gone (it confused people and nobody used it); a radio2 still on 125 kHz
  shows as custom and keeps working.
- **Find MediaMesh**: finds the radio2 profile the MediaMesh here uses (below).
- **Frequency, Bandwidth, SF, CR**: the radio2 profile. A field applies when you
  leave it. It must differ from the main radio and stay clear of every listed
  community mesh (see *Presets and community meshes*).
- Live figures, refreshed every second: listen time per profile, radio2
  preamble, time on air, switch time (measured on the first four hops), packets
  received and sent per profile, and any timing warnings in red.

The **Temporary radio2** card: its own Frequency/Bandwidth/SF/CR, then
*15 min / 1 hour / 2 hours* (`set tempradio2 ...,rxtx,mins`) or *Stop*
(`set tempradio2 off`). *Running now* shows the frequency and time left.

### Find MediaMesh

**Find MediaMesh** (under *Preset*) works like Discover Repeaters: it asks the
repeaters in direct range which radio2 profile they run.

- One zero-hop discovery request goes out on the main mesh with the radio2 flag
  (byte 0 `0x82`). Repeaters that support it answer within about 12 seconds
  with their radio2 profile after their key.
- The answer lists them, strongest first: each repeater's frequency, bandwidth
  and SF, *listen only* or *temporary* when so, *radio2 off*, or *didn't say*
  (stock firmware). The profile most of them run is offered (the stronger
  signal breaks a tie), and **Use it** sets it, turning radio2 on for photos and
  voice if it was off.

Answering with the profile is a proposed addition to keymindCascade; stock
repeaters ignore the flag. **When no repeater says**, it falls back to
listening, in about 20 more seconds:

1. radio2 listens on the 250 kHz preset, at the frequency in the form, with a
   2-minute temporary radio2 (`set tempradio2 <MHz>,250,7,5,rx,2`, listen only).
2. A repeater discovery request goes out on the main mesh. MediaMesh repeaters
   (keymindCascade, `tx.reply both`) answer on both of their radios, so their
   radio2 answer is only heard if this standalone listens on the same preset.
3. After about 9 seconds, `set tempradio2 off` puts the saved radio2 back. If
   packets were heard on radio2, the 250 kHz preset is offered.

It needs a MediaMesh repeater in direct range. Repeaters answer at most four
discovery requests every two minutes (asking uses one, listening one more), so
running it again right away may find nothing. It refuses while Client Repeat is
on, and the listening fallback won't run while a temporary radio2 runs.
MeshCore Open TAK has the same button in its MediaMesh settings.

#### Discover reply: radio2 profile

Requested by bit 1 of DISCOVER_REQ byte 0 (`0x80 | 0x02`, combinable with the
prefix-only bit 0). The repeater appends 12 bytes after its key (after the
8-byte prefix in prefix-only mode):

```
[ver=1][flags][freq u32 LE, MHz x1000][bw u32 LE, kHz x1000][sf][cr]
flags: bit 0 radio2 configured, bits 1-2 mode (0 off, 1 rx, 2 rxtx),
       bit 3 temporary profile (tempradio2)
radio2 off: just [1][0]
example: 910.525 MHz, 250 kHz, SF7, CR5, rxtx = 0105bde40d0090d003000705
```

Stock clients check only `len >= 6+32` and ignore what follows. The code is
`mediamesh::DiscoverRadio2` in `src/helpers/MediaMeshCore.h`, with the same
golden bytes as MeshCore Open TAK's tests.

Per contact: the contact menu's **⇄ Radio** button cycles Auto → Main → radio2 →
Both → Off (`tx.user`). Per channel: **Send on** in the channel's settings
(`tx.channel`); an `#r2-` channel shows it is bound to radio2.

Everything else (schedules, `tempradio`, diagnostics) works from
**Terminal**: type the commands below, or `help`.

A radio that has never saved MediaMesh settings starts with radio2 **on**
(send and listen) when its main radio is in the US 902-928 MHz band, and off
elsewhere, since the default frequency below is a US one. Turning it off is
remembered. **Sent on radio2** starts as *Photos & voice*. A radio whose
`tx.media` was never chosen (still `auto`, shown as *Custom*) moves to
*Photos & voice* once (NVS key `mm_pv_def`); choosing `auto` again from the
Terminal afterwards is kept.

Bridging (`radio2.cross`) is **off** by default. A radio updated from firmware
that saved its MediaMesh settings in an older format (blob v7 or earlier)
turns bridging off once, saved and held alike, whatever it was; `set
radio2.cross on|auto` from the Terminal afterwards is kept.

Default settings: 911.150 MHz, 250 kHz, SF7, CR 4/5 (the *250 kHz fast* preset).
That is about 4× faster than SF7/62.5 for roughly 6 dB less range. 911.150 is a quiet spot found on site, inside a BayMesh 3 cavity
filter tuned for 910.525 (flat about 908.6–912.8 MHz) and 625 kHz above the main
channel (the channels' edges about 530 kHz apart). Everyone exchanging
media has to use the same settings. Radios already set up keep their own.

### Coverage map

*Settings › radio2 (MediaMesh) › Map coverage* maps where the MediaMesh
repeaters can be reached as you walk or drive, the same as MeshCore Open TAK's
coverage map. Every 150 m, and never more often than every 40 s, the radio asks
the repeaters in direct range as Find MediaMesh does (the radio2 flag on
DISCOVER_REQ) and keeps only the ones that answer with radio2 on. Repeaters
without MediaMesh, and every other node, are left out.

Each sample is a dot on the map, coloured by the best repeater's margin:

| Colour | Margin | Meaning |
| --- | --- | --- |
| Green | 5 dB or more | radio2 reaches it with room to spare |
| Amber | 0 to 5 dB | just reaches |
| Red | below 0 | heard on the main mesh, but radio2 likely won't reach it |
| Grey | no answer | no MediaMesh repeater answered here |

The ask goes out on the main mesh, so radio2's reach is an estimate: the weaker
of the two directions' SNR (ours of the reply, the repeater's of the ask), less
the extra noise of radio2's wider bandwidth (10·log10 of radio2's bandwidth over
the main mesh's), against the lowest SNR radio2's spreading factor decodes
(SF7 −7.5 dB, each SF step 2.5 dB lower). The sums are in
`src/helpers/MediaMeshCoverage.h`, with the same golden values as the app's
test. A margin under a few dB is worth checking by sending a photo there.

Tap a dot on the map to see when it was measured and each MediaMesh repeater
heard there, strongest first: its margin on radio2 and the SNR both ways (as
in the app). Dots read back from the card keep their repeaters too.

It needs a GPS fix, and the GPS stays on while it records, with the screen off
too. A Discover scan or Find MediaMesh in progress pauses it. Radios that can't
run radio2 themselves can still map the repeaters'. The map keeps the last 500
samples (*Clear coverage map* empties it); every sample is also a line per
repeater in `/meshcomod/mediamesh_coverage.csv` on the card, read back at
start-up:

```
epoch,lat,lon,reach,best_margin,repeater,snr,remote_snr,margin,key
1760000000,47.600000,-122.300000,marginal,4.5,First Hill,4.5,3.0,4.5,1ea5e195
1760000040,47.601400,-122.300000,none,,,,,
```

`key` is the repeater's first 4 key bytes in hex; lines written before it was
added have no such column.

#### Sharing coverage points

*Share coverage points* (under Map coverage, off by default) sends the card's
points to a CoreScope analyzer over Wi-Fi, where the MediaMesh coverage
estimate is checked against them, the same as MeshCore Open TAK's switch.
**Analyzer** is the address the points are sent to,
`https://analyzer.cascadiamesh.org/api/mediamesh/coverage-points` unless you
enter your own area's (its upload address, or just the analyzer's address). Every 5 minutes on Wi-Fi (and on *Send now*) it sends what it hasn't
yet, up to 200 points a batch, signed by the standalone's own key over
`corescope-coverage:` and the SHA-256 of the batch; how far into the file it
got is kept, and a new analyzer gets everything. Lines without a `key` can't
be sent and are skipped. The analyzer only takes radios it has heard, shows
the points as 200 m squares without the first and last 300 m of each trip, and
never says whose they are. *Remove my points from the analyzer* deletes them
there and switches sharing off; the file stays. The batches and the signed text
are in `src/helpers/MediaMeshCoverageShare.h` (host-tested); the request runs
on the net worker (`forkPostJson` in `ForkUpdate.inc`), so only builds with the
fork's updates can share.

### MediaMesh repeaters on the map

Repeaters known to run MediaMesh get a fuchsia rim and a fuchsia badge on the
map, as in MeshCore Open TAK. Their contact sheet shows the profile under the
name, for example *MediaMesh 250 kHz SF7, listens and sends*. *Map options ›
MediaMesh only* hides every other node.

A repeater is known from what it answered when asked for its radio2 profile
(the radio2 flag on DISCOVER_REQ). These ask:

- Find MediaMesh;
- the coverage map;
- the Discover app;
- the Regions scan;
- a quiet ask 90 s after start-up and then every 30 minutes. It is skipped when
  another scan asked in the last 3 minutes, while Discover is open, or with
  little airtime left.

An answer with radio2 off clears the mark. Answers are kept 30 days, in
`/mm_reps` on internal storage (`src/helpers/MediaMeshReps.h`; test:
`test/test_mediamesh_reps.cpp`).

The ask is zero-hop, so only repeaters in direct range can answer. A repeater
further out is marked once it has been in direct range.

## Spectrum app

The Spectrum app can show radio2 as well as the mesh channel. Tap the gear at
the bottom right of the waterfall and turn on **Show MediaMesh** (saved, off by
default). It uses the standalone's own channels: the sweep is centred on the
main channel and MediaMesh is radio2's frequency while radio2 is on
(`tempradio2` included). When one isn't there, it falls back to the
CascadiaMesh pair: **910.525 MHz** for the main channel and **911.150 MHz**
(250 kHz) for MediaMesh, so a standalone with radio2 off still sees where
MediaMesh traffic is.

- **Band** draws a fuchsia marker labelled *MediaMesh* at that frequency, when
  it falls in the 24 MHz sweep. That is the case for any US main channel.
- The view button goes **Band → Channel → MediaMesh**. *MediaMesh* zooms on
  radio2 the way *Channel* zooms on the mesh channel: 16 channel widths around
  it, at most 4 MHz. The Channel and Busy tiles then measure that channel.

The sweep runs on the main radio chip, which tunes 150–960 MHz, so the
MediaMesh view works whatever the main channel is. The T-Deck Pro's e-paper
Spectrum layout doesn't have it.

## Repeaters

A repeater running MeshCore with radio profiles
([mikecarper/MeshCore](https://github.com/mikecarper/MeshCore/blob/keymindCascade/docs/radio_profiles.md))
can carry radio2 too. Log in as admin (contact menu → **Admin**), then tap the
antenna button in the console's header for its **MediaMesh** page:

- Both radios side by side, joined by the bridging state, and a status line.
- **Live**: packets heard and sent per radio, the share heard on radio2, switch
  time, preambles, switch errors and the repeater's warnings. **Refresh** asks
  again; nothing is polled, to save airtime.
- **radio2 profile**: Off / On (listen and send; a listen-only repeater shows as
  On until On is tapped and applied), frequency, bandwidth, SF, CR
  and an optional preamble (empty = the repeater picks it), applied with **Apply
  to repeater** after a confirm. The repeater answers first and switches after,
  so if you reach it over radio2 you may lose contact until your radio follows.
- Bridging isn't offered: **Apply to repeater** sets `radio2.cross off`
  whenever radio2 is on.
- **Replies to admins** (`tx.reply`): Both (the default), Main, radio2 or Auto;
  *Also on a listen-only radio2* adds `force`.
- **Temporary radio2** (`set tempradio2 ...,minutes`) for 15 min to 6 h, or Stop.

The page tags its commands (`A0|get radio2`) so their replies stay out of the
console log. A repeater without radio2 says so on the page; one whose role only
takes radio2 changes over USB shows that as an alert. Stock repeaters still pass
MediaMesh traffic heard on the main mesh. MeshCore Open TAK has the same page
under repeater management → **MediaMesh (radio2)**.

## Commands

From an app, the radio answers the local CLI frame `CMD_RUN_CLI_COMMAND` (`0x42`)
with `RESP_CODE_CLI_REPLY` (`0x1D`). A two-character correlation tag is echoed:
`ab|get radio2` → `ab|> off`. Unknown text answers `Unknown command`.

The tuple is **frequency in MHz, bandwidth in kHz, spreading factor, coding-rate
denominator, mode, optional preamble in symbols** (`auto` or omitted = worked
out from the scan). Modes: `rx` (listen only) or `rxtx` (also `rx&tx`).

### The second profile

```text
get radio2                         > 911.150,125.000,7,5,rxtx,32 (auto)
set radio2 911.15,125,7,5,rxtx     OK - radio2 rxtx; preamble=32
set radio2 911.15,125,7,5,rx,72    listen only, explicit preamble
set radio2 off                     OK - single radio (also ends temporary sessions and schedules)
```

### Temporary and scheduled profiles

```text
set tempradio2 910.5,500,8,5,rxtx,120     radio2 replaced for 120 minutes (1-10080)
get tempradio2                            > 910.500,500.000,8,5,rxtx,40 (auto); 0d2h0m (120 min) left
set tempradio2 off                        OK - saved radio2 restored

set radioat2 910.5,500,8,5,rxtx,START[,PREAMBLE]          becomes the saved radio2 at START
set tempradioat2 910.5,500,8,5,rxtx,START,END[,PREAMBLE]  a temporary radio2 from START to END
get radioat2 | get radioat2 1 | del radioat2 1 | del tempradioat2 all

tempradio 869.525,250,11,5,30[,PREAMBLE]  the main radio replaced for 30 minutes
tempradio | get tempradio                 TempRadio active: 869.525,250.00,11,5 1800s left
normalradio                               the saved main radio back now
```

`START` and `END` are UTC Unix seconds, or `+N` for N whole minutes from now
(both measured from the moment the command is accepted). They must be in the
future and within 24 days, which needs the clock set. Four slots per family;
temporary schedules may not overlap each other or a running session.

Temporary sessions and schedules live in RAM: they end on a reboot, and run on
the millisecond clock, so setting the clock cannot stretch them.

### Where traffic goes

```text
get radio2.cross | set radio2.cross auto|on|off
get tx.media | set tx.media auto|radio|radio2|both|off
get radio2.repeat | set radio2.repeat on|off
get tx.user | get tx.user "Alice" | set tx.user key:0123456789AB radio2
get tx.channel | get tx.channel 0 | set tx.channel #wardriving both
```

| Choice | Sends on |
| --- | --- |
| `auto` | The `radio2.cross` rules below (default) |
| `radio` | The main profile only |
| `radio2` | radio2 only; the send fails while radio2 is off or `rx` |
| `both` | Both, when radio2 can send; otherwise the main profile |
| `off` | Neither (reception is unaffected) |

`radio2.cross` decides what `auto` traffic does, and what a forwarded packet does
(it keeps to the profile it arrived on unless crossing is allowed):

| Main | Second | `auto` | `on` | `off` (default) |
| --- | --- | --- | --- | --- |
| saved | saved | both | both | isolated |
| `tempradio` | `tempradio2` | both | both | isolated |
| saved | `tempradio2` | isolated | both | isolated |
| `tempradio` | saved | isolated | both | isolated |

Crossing never sends on an `rx`-only radio2.

- **tx.media** is for MeshCore Open TAK photos and voice: raw data from the app
  (`CMD_SEND_RAW_DATA`) and channel data type `0x5643`. An explicit choice wins
  over the contact's or channel's own; `auto` leaves it to them. The fetch
  requests asking for a photo or clip (binary `0x72`) follow it too.
- **mediamesh.share** was removed: photo and voice requests and answers always
  stay on radio2, so nothing of a transfer touches the main mesh.
  `get mediamesh.share` answers `off`, `set mediamesh.share off` is accepted,
  and `on` is refused. The hidden developer switch `mediamesh.main` was removed
  the same way.
- **Client Repeat** moves the main radio to the off-grid channel, the allowed
  client-repeat frequency nearest the current one (918.000 MHz in the US,
  869.495 in EU868, 433.000 at 433) at 62.5 kHz, SF7, CR 4/5, as MeshCore Open
  TAK's Off-Grid Repeat does, and remembers the settings it replaced; turning it
  off puts them back.
  - **US band:** radio2 runs MediaMesh beside it, 800 kHz above (918.800 MHz)
    at 250 kHz, SF7, CR 4/5, listening and sending, with `radio2.cross off`.
    The two channels don't overlap (keymindCascade's spacing rule wants more
    than (62.5 + 250) / 2 + 6 = 162.25 kHz between centres), so the radio scans
    them as it scans the main mesh and MediaMesh normally. Photos and voice go
    on radio2; everything else stays on the off-grid channel.
  - **Elsewhere** (EU868's off-grid channel has no room beside it in its
    sub-band): radio2 is **Off**, a 2.4 GHz radio2 included, and everything
    goes out on the off-grid main radio.
  - Either way radio2's mode, settings and `radio2.cross` are remembered and
    put back when Client Repeat is turned off, and a `tempradio2` ends.
    radio2 changed meanwhile (from an app or the Terminal) is set again, and
    that newer choice is the one put back. `get radio2` adds
    `; client repeat is on (radio2 back to rxtx when it is off)`; the
    remembered settings are saved, so they survive a restart.
  - A repeating node relays each packet on the radio it arrived on, following
    `radio2.cross` from that radio, so others' MediaMesh photos and voice are
    relayed on radio2 and the off-grid channel's traffic on the main radio. A
    packet its sender put on both radios is relayed on each radio it is heard
    on, once per radio, and handled (shown, passed to the app) only once.
- **tx.user** covers everything addressed to a contact: messages, logins,
  requests, path-return retries, every flood to them, and the ACKs and replies
  we send them. Direct packets the core sends by itself (room keep-alives) are
  matched to the one contact whose key and stored route they carry. A contact is
  named exactly (quote names with spaces) or by `key:` + 12-64 hex digits.
- **tx.channel** covers a channel's messages and data. A channel is named by its
  slot number or its exact name; `"1"` in quotes is a channel named 1.
- Choices are saved (NVS key `mmroutes`, up to 48) by key, not by slot or name:
  renaming keeps them, a new key loses them. Getters show `active=`, where the
  next packet would go now.
- A packet's profiles are fixed when it is queued: its retries keep them. Anything
  made while handling a received packet (forwards, ACKs, replies) starts from the
  profile that packet arrived on, as mikecarper/MeshCore does.
- A packet for `both` becomes two packets, one per profile, each sent on its own
  (its own turn in the queue and its own busy-channel check). If no packet is
  free to copy, the one packet goes out twice back to back instead.
- If a profile changes after a packet was queued for it (new settings, a
  temporary session starting or ending, radio2 off), the packet is not sent
  there; with nothing left it is dropped. Retries of it are dropped the same way.

### `#r2-` channels

A channel named `#r2-name` uses the same key as `#name` (when the app derived the
key from the name; a key you typed is kept). While radio2 is configured (`rx` or
`rxtx`), it is bound to radio2: it sends there only (`rx` can't send), only
packets received on radio2 reach it, and its packets are forwarded on radio2.
`set tx.channel` can't change that; rename the channel instead. With radio2 off
the prefix does nothing. If you have both `#name` and `#r2-name`, a packet heard
on radio2 goes to `#r2-name` and one heard on the main profile to `#name`.
A packet heard on the main profile that only an `#r2-` channel could open is
dropped before it is marked as seen, so the same packet arriving on radio2 is
still taken. `#r2-` alone, or nested prefixes, are refused.

### MediaMesh nearby

The status bar shows an antenna while radio2 is listening: dim, and lit for two
hours after a valid packet (it passes the radio's CRC and parses as MeshCore)
is heard on radio2, meaning a MediaMesh is on the air here. A reboot starts it
dark.

### Presets and community meshes

MediaMesh Settings offers one sub-GHz preset: **250 kHz fast**, SF7, CR 4/5 at
911.150 MHz (the default for a new radio2). A radio2 channel that would overlap any listed
community mesh preset is refused on the device with "Conflicts with <preset>".

### Photos and voice stay off the main mesh

Open TAK's photos and voice clips go out on radio2 alone (upstream MeshCore
Open's channel image messages are left as they are), whatever `tx.media` says, and there is no setting that changes it: a photo burst
on the main mesh takes minutes of everyone's airtime. With radio2 off, or
receive-only, they are not sent, and the T-Deck says so when you hold the mic.
Text, adverts and the rest follow `tx.media`/`radio2.cross` as before. Client
Repeat, which has no radio2, still sends them on its own frequency.

### Not on Public and #public

Photos, voice messages and push-to-talk are off on Public and `#public`. Those
are the channels everyone on the mesh shares, and photos and voice there show
up as clutter for everyone (other apps show them as `IMAGE` / `VOICE` lines).
Holding the mic, sending or pasting a photo, or talking on the walkie-talkie in
those chats says so instead. They come back with **Settings > MediaMesh >
Public and #public > Photos and voice on Public**, which is off by default and
says in red to switch it on only in an emergency. It's saved on the device.

The channels are the ones OTP refuses (`OtpCore::isPublicChannel`): a name of
`public` or `#public`, in any case. Every channel post is checked, not only the
buttons, so a resend, a Lua app or the web remote can't send a photo or voice
message there either; text and everything else on Public is untouched. The
rule is in `src/ui-touch/PublicMedia.h` (host test
`test/test_public_media.cpp`). MeshCore Open TAK has the same switch, in
Settings > App Settings > Messaging, also off by default.

### Diagnostics

```text
get radio2.status    > rxtx; RX=12,40 TX=3,9 switches=5120 errors=0 max=1203us preamble=32,64; test=4/4 ready budget=1324us
get radio2.scan      > slow=radio; listen_us=9421,21667; preamble=32,64
get radio2.timing    > chirps=4.60,43.33; need=32,64; switch=1324us; loop=0us; test=4/4 ready; preamble=32,64
get mediamesh        > radio2=...; tx.media=radio2; cross=off; share=off; media on radio2
```

`max=` is the longest retune ever seen and `typ=` the typical (median) one
since the last profile change; `stalls=` counts retunes over 8 ms, held up
waiting for the SPI bus and left out of the timing. `budget=` is built on the
95th percentile of the recent retunes (the longest, until 32 are measured),
plus 10%: a T-Deck, whose screen shares the bus with the radio, measures
~1 ms like a repeater instead of the ~8 ms a rare slow retune used to set.

On the T-Deck the retune itself is also trimmed, following keymindCascade's
measurements on the same board (mikecarper/MeshCore a46158708, d4c826322):
the radio's SPI runs buffered at 8.89 MHz instead of byte by byte at 2 MHz
(`variants/lilygo_tdeck/TDeckRadioHal.h`; 8.89 MHz is the fastest the board's
wiring passed), a scan retune skips the commands that can't have changed (the
wake-up NOP, a second standby, the packet-type query and the IQ register
fix-up: eight SPI commands instead of about fifteen), and the screen is sent in
pieces of at most 960 pixels so the radio never waits long for the bus.

`get radio.timing` is the same as `get radio2.timing`. Warnings follow the
figures: `WARN recommended preamble: radio2=64` when a profile's preamble is
shorter than a scanning receiver needs, `short override: radio2` when that is an
explicit value you chose, `WARN need >128` when an automatic preamble had to stop
at 128, `WARN main preamble too short for scanning` when the main mesh's stock
preamble can't cover the radio2 visit.

### Where a photo or voice clip came from

While radio2 is configured, a downloaded photo or voice clip says how it got
here, under its bubble: **Received via MediaMesh**, **Received via main mesh**,
or **Received via MediaMesh and main mesh** when its packets came both ways.
It is counted per packet while downloading, so it is gone after a reboot.

The app gets the same from the frames it is sent, in a byte stock firmware
leaves reserved: `0xA0` main mesh, `0xA1` radio2, anything else unknown
(radio2 off, or other firmware).

| Frame | Byte |
| --- | --- |
| `PUSH_CODE_RAW_DATA` (`0x84`): `[code][snr][rssi][via][payload]` | 3 (stock: `0xFF`) |
| `RESP_CODE_CHANNEL_DATA_RECV` (27): `[code][snr][via][0][idx][path][type 2][len][data]` | 2 (stock: `0`) |

## How the scan works

One SX1262 alternates between the two profiles. The timing is
mikecarper/MeshCore's, recomputed for the actual settings:

- The **slower** profile (longer LoRa symbol, normally the main mesh) gets a
  4.6-symbol visit. The faster one gets the rest of half the slow preamble,
  minus two retunes. With SF7/62.5 main and SF7/500 radio2: 9.4 ms main,
  21.7 ms radio2.
- The main mesh's preamble is fixed by stock nodes (MeshCore: 32 symbols at
  SF≤8, 16 above), so radio2 only fits main presets where half that preamble
  covers both visits. `get radio2.timing` warns otherwise.
- radio2 packets are sent with a longer preamble that covers a receiver's time
  on the main profile plus 16 symbols to lock on: 64 symbols for SF7/500, 40 for
  SF8/500, 32 for SF9/500 (rounded up to 8, capped at 128 when automatic).
- Retunes are fast: while scanning the oscillator stays on between visits (XOSC
  standby), SF/BW/CR go to the chip in one command, and the preamble rides on the
  packet setup the receive start sends anyway. mikecarper/MeshCore measured the
  same changes taking a hop from about 3.2 ms to 1.2 ms. Keeping the oscillator
  on draws a little more current while radio2 is on.
- Retune time is measured on the first four hops after any profile change and
  padded by 10%; until then 1.5 ms is assumed. `get radio2.status` shows `test=`.
- The receiver stays on a profile while a packet is arriving there, until the
  packet is read out, and while it transmits. A packet on one profile means the
  other is deaf until it ends. Heavy media traffic therefore costs main-mesh
  reception: a 255-byte SF7/250 packet is ~0.2 s.
- Every received packet is read out of the radio before the scan moves on (the
  scan waits for the chip's RX-done flag to clear), so it is credited to the
  profile it arrived on. The same packet heard on both profiles is credited
  copy by copy.
- Buffered receive is switched on while radio2 is active (its drain task lifts
  each packet out within ~1 ms so the scan can move on), and goes back to your
  setting when radio2 is off.

## Fast MediaMesh

**Settings → MediaMesh → Fast MediaMesh** (on by default) makes photo and voice transfers on radio2 quicker, through repeaters especially. It works the same way as in MeshCore Open TAK:

- **Repair packets** (`VoxFountain.h`). A repair packet is a mix of all of a clip's packets, so a standalone missing *m* packets rebuilds them from any *m* repair packets (practically always *m* + 1). Its channel requests say it takes them. When this standalone answers for a clip it sent, it sends that many fresh repair packets, plus a margin, instead of the specific packets, and that one answer mends every listener's gaps. A whole clip sent at once ends with about a tenth repair packets instead of the block parity.
- **Repair packets in a mixed group.** Data packets going out anyway, to a listener that can't use repair packets, count towards the repair packets a listener that can is owed (`VoxServe::repairsDue`, `VoxFountain::owed`): it gets repair packets only for what no data packet carries, plus about a tenth of the rest, since a radio scanning radio2 misses some while it's on the main mesh.
- **Thumbnail asks get the packets themselves** (`VoxServe::thumbAsk`). An ask for only a photo's first two packets, which hold the thumbnail, is answered with those two packets, even when it says it takes repair packets (as MeshCore Open TAK's thumbnail asks did): the listener holds almost none of the photo, so repair packets couldn't give it the thumbnail. A standalone's own thumbnail asks never say they take repair packets.
- **Listeners missing most of a clip.** A request names at most 124 packets. A listener naming that many may lack more after the last one, so it gets those as data too, not repair packets, which only mend once there are as many as it lacks.
- **Repair packets sized for the losses** (`FloodPace.h` `loss_pct`, `VoxFountain::passShare`). The sender learns from listeners' requests how much of its last answers the worst-placed listener lost (up to 40%) and sizes repair packets for it, so listeners several hops away finish without asking again. With no loss seen, a pass ends with about a tenth, as before; a fifth lost, about 29%.
- **No asking again while the transfer is on the air** (`VoxFetch::busyAirHoldMs`, `VoxFountain::sameTransfer`). A standalone whose request times out while it still hears packets of the same transfer (another part of the clip going to another listener, or this clip's repair or parity packets) waits for them to stop, up to 3 minutes after asking, before asking again. The sender answers one request after another, so asking again would only queue; and every request floods the main mesh, where every repeater scanning radio2 misses radio2 packets while relaying it. Other clips on the air don't hold a request. The serial log says "sender still on this transfer".
- **Every answer through the repeaters** (`VoxServe::zeroHop`). Channel answers are flooded through the MediaMesh repeaters, never sent zero-hop, even to a listener heard directly. In a field test with every radio scanning radio2, zero-hop answers arrived with 41-46% of their packets against 91% flooded at the fast pace: a scanning radio misses packets while it's on the main mesh, and a repeater's copy is a second chance.
- **Closer spacing through repeaters** (`FloodPace.h`). Flooded answers start about 2.4 airtimes apart (about 0.8 s at 250 kHz) instead of 4 (1.5 s), and still slow down when an answer loses a third of its packets.

Standalones and apps without it still get every packet itself: they don't ask for repair packets, and they drop any they hear as an unknown clip.

**Adaptive spacing** (**Settings → MediaMesh → Fast MediaMesh → Adaptive spacing**, on by default, needs Fast MediaMesh; `FloodPace.h` `Pace::adaptive`). The gap between an answer's packets learns from what listeners' requests say they lost, from the fast gap (517 ms at 153 ms a packet). Hop counts play no part: in a dense mesh a listener next door can be 3 hops away, and what limits the spacing is how many repeaters relay each packet. A listener lost under 3%: down a tenth, to at most 1.6 airtimes and 150 ms (395 ms). A listener lost 7 points more than at its widest recent gap: the spacing's doing, so up a quarter (by half at a third), held 10% above the gap that lost for 30 minutes. As lossy at a wider gap: its own link, so the gap stays and repair packets cover it. Not heard from before and 10% lost: up a quarter, without the hold. Answers wait their turn, but a small one (40 packets or fewer: repair packets, a few missing packets) goes before a bigger one queued ahead of it. A standalone learns afresh after a reboot (the app keeps its learnt gap). The serial log says "pace N ms apart (adaptive)" and "adaptive spacing now …". Switch it off for the spacing of the 2026-11-18 field test, kept as the branch `mediamesh-gold-2026-11-18` here and in MeshCore Open TAK.

**Rolling back.** Switch Fast MediaMesh off, on one standalone or on all of them, and transfers work exactly as they did before. That covers the requests, the answers and the spacing. Standalones and phones with it on and off still work together. To take it out of the firmware altogether, revert the pull request that added it (and the app's).

## Listen before talk

A time-shared radio can't sense radio2 while it listens on the main channel,
so "busy" means a radio2 packet was heard lately. A packet bound for radio2
alone (a photo or voice packet, or a request for one) waits in the queue
while radio2 is busy:
- **Until:** radio2 has been quiet for one full packet's airtime plus a
  random part of another, so two radios that both waited don't then collide.
- **Meanwhile:** main-mesh packets go out as usual.
- **At most:** 20 s in all; after that it goes anyway.

`radio2.status` counts the waits as `lbt=`. MeshCore Open TAK waits the same
way before each photo or voice packet it sends (`vox_timing.dart`).

## Supported radios

SX1262/SX1268 boards: T-Deck, T-Deck Pro/Max, Heltec V4, T-LoRa Pager (SX1262),
Wio Tracker L2, CrowPanel 3.5 and 5.0, Attaky, RAK WisMesh Tap V2, and the
T-Display P4 with the SX1262 module.

The P4's DIO1 goes to the XL9535 expander, not a GPIO, so there is no receive
interrupt. The scan reads the chip's IRQ register over SPI on every visit anyway,
so it stands in for one: a finished packet it finds is handed to the drain task
straight away, and TX-done is read from the chip as well. (Without radio2 the P4
also gains the TX-done read: sends used to end on the dispatcher's timeout.)

LR1110/LR1121/LR2021 boards: ThinkNode M9 (LR1110), T-LoRa Pager LR1121, and
the T-Display P4 LR2021 build. The scan is the same; the retune goes through
RadioLib's own setters (frequency, then SF/BW/CR only when they differ) and the
receiver is armed the stock way, without the SX1262's lean arm. The scan times
its retunes and sizes its visits to them, so a slower retune costs a little
radio2 dwell, not packets; `radio2.status` shows the measured switch time. The
LR wrappers reset the preamble to the stock one after every send, so MediaMesh
puts radio2's longer preamble back before the next radio2 send. These boards
are new to MediaMesh: check `radio2.status` (rx/tx counts on both profiles)
after turning it on.

Tanmatsu shows "This radio can't run MediaMesh" and answers radio2 commands
with an error. `tempradio` works on every board.

### radio2 on 2.4 GHz (LR1121, LR2021)

The LR1121 (T-LoRa Pager LR1121) and the LR2021 (T-Display P4 LR2021 build) also
do LoRa at 2.4 GHz, so their radio2 can leave the 900 MHz band altogether.
Settings → MediaMesh → **Preset** has a third choice there, **2.4 GHz**,
balanced for speed and distance: 2424.5 MHz (between Wi-Fi channels 1 and 6),
406.25 kHz, SF8, CR 4/5. Or type any 2400-2500 MHz frequency with a 203.125,
406.25 or 812.5 kHz width (`set radio2 2424.5,406.25,8,5,rxtx`). Picking 250 kHz again puts radio2
back on 911.150 MHz. The main radio stays sub-GHz (`tempradio` refuses 2.4 GHz).

- Only LR1121 and LR2021 radios can join a 2.4 GHz radio2. The SX1262 boards,
  the ThinkNode M9's LR1110, Keymind Cascade companions and repeaters can't
  hear it, so a phone with MeshCore Open TAK can't use it.
- The scan still time-shares one radio: it hops between 900 MHz and 2.4 GHz,
  calibrating each band once rather than on every hop. It is not full duplex.
- Each band has its own power amplifier. The TX power goes back on at each band
  change, clamped to what the 2.4 GHz amplifier can do (+13 dBm on the LR1121,
  +12 on the LR2021).
- Why 406 kHz SF8: it moves about 10 kb/s, the same as the 900 MHz 250 kHz
  preset, and hears about 5.5 dB deeper than the fastest 2.4 GHz setting
  (812.5 kHz SF7), nearly twice the distance in the open. Over the same
  distance 2.4 GHz loses about 8.5 dB more than 915 MHz, and its amplifier
  gives about 9 dB less than the sub-GHz one at +22 dBm, so it still reaches
  less far than the 900 MHz presets. Other trades, typed in: 812.5 kHz SF7
  (about 1.8x faster, shorter) or 203.125 kHz SF9 (about 3.5x slower, about
  1.6x farther again). The board needs an antenna for 2.4 GHz on the radio's
  2.4 GHz port. Check `radio2.status` after turning it on: rx/tx counts on both
  profiles.

## Repeat MediaMesh

Settings → MediaMesh → **Repeat MediaMesh** (`set radio2.repeat on|off`) makes
a standalone relay what it hears on radio2, back out on radio2, like a
MediaMesh repeater. Main-mesh traffic isn't relayed; that is still Client
repeat. It needs radio2 On (listen and send). On a 2.4 GHz radio2, a few
standalones with it on make a 2.4 GHz MediaMesh with no repeaters on 900 MHz
involved.

**Client repeat** sets radio2 itself: beside the off-grid channel in the US
band (918.800 MHz, 250 kHz), off elsewhere, a 2.4 GHz radio2 included. It
relays radio2 traffic on radio2 as it is, so Repeat MediaMesh adds nothing
while it is on.

## Limits

- One transceiver: this is a scan, not two receivers. Expect some loss on both
  profiles, as with mikecarper's dual-profile bench results (197/200 packets).
- Listen-before-talk checks the profile the radio is on when the packet's turn
  comes, which may not be the one it is about to send on.
- Settings are stored separately from the node preferences (NVS keys
  `mediamesh`, `mmroutes`) and are not in the settings backup yet.

## Code

| Piece | Where |
| --- | --- |
| Settings, CLI, routing rules, schedules, scan timing, markers, `#r2-` (host-tested) | `src/helpers/MediaMeshCore.h`, `test/test_mediamesh_core.cpp` |
| Scanning radio wrapper: fast retune, per-packet profiles, receive origin, tempradio | `src/helpers/MediaMeshRadio.h` (variants declare `RADIO_DRIVER_CLASS radio_driver`) |
| Storage, timers, apply, packet marking, CLI glue | `src/helpers/MediaMesh.{h,cpp}` |
| Contact/channel contexts, `tx.user` / `tx.channel`, `#r2-` lookups, `CMD_RUN_CLI_COMMAND` | `src/MyMesh.cpp`, `src/MyMesh.h` |
| MediaMesh Settings cards, contact ⇄ Radio, channel Send on | `src/ui-touch/UITask.cpp` (`buildMediaMeshCard`, `actionSheetRadioCb`, `chanScopeRadioCb`) |
| Terminal commands | `MyMesh::handleMeshcomodCommand` |
| Coverage map: margins, schedule, CSV (host-tested); recorder, map layer, settings rows | `src/helpers/MediaMeshCoverage.h`, `test/test_mediamesh_coverage.cpp`; `src/ui-touch/MediaMeshCoverageUI.inc` |
| Sharing coverage points: batches, signed text (host-tested); the request | `src/helpers/MediaMeshCoverageShare.h`, `test/test_mediamesh_coverage_share.cpp`; `MediaMeshCoverageUI.inc`, `ForkUpdate.inc` |

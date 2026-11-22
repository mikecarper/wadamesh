# Kid on a bike and games

Open **Apps > Kid on a bike** on a T-Deck, T-Deck Plus or T-LoRa Pager. A GPS-equipped rider radio can report to a family team. A guardian radio can also receive private check-ins directly from a Keymind RAK pet tracker.

## Follow a rider

1. On both radios, open **Team > Setup** and join the same private, keyed family channel. Enable Team on both. Enable GPS and Team position sharing on the rider. The original T-Deck needs a working GPS receiver to report a position. See [Team setup](TEAM_SETUP.md) for joining by QR or sound.
2. On the rider radio, tap **Kid on a bike > Start ride**. Give the GPS a clear view of the sky. It sends a team check-in every 60 seconds by default. While waiting for a fresh fix, it sends presence only.
3. On the guardian radio, open **Team**, select the rider, and tap **Monitor bike rider**. In **Kid on a bike**, see when the rider was last heard and when their last position arrived. Tap **Rider on map** to center the map there. A missed check-in for three minutes raises a local overdue alert. The next check-in clears it and alerts the guardian. Time without radio contact can also mean the rider is outside mesh coverage.
4. A phone running MeshCore Open TAK can join the same team and view the standard team reports. The overdue alarm described here runs on the guardian radio. Everyone holding the family channel key can read the team reports.
5. Choose **End ride** when finished. The radio then follows its ordinary Team sharing settings. To stop all team position reports, switch off Team position sharing too.

**Position interval** accepts 10 to 3,600 seconds. **Missing timeout** accepts 1 to 3,600 seconds, independently on each radio. Set the guardian timeout longer than the rider interval to allow for missed packets.

### Boundary and return-home sound

Open **Boundary** on the rider. Pan the map, reopen **Kid on a bike > Boundary**, then choose **Add map center**. **Back to map** keeps your draft while you place the next point.

| Points saved | Shape |
| --- | --- |
| One | Circle around that point. Set **Radius** in meters. |
| Two | Box defined by two opposite corners. |
| Three to nine | Polygon. Add points in perimeter order. |

Choose **Save boundary**. The rider sends a family-channel message when two fresh fixes confirm an exit or a return. The first confirmed side establishes the starting position. While outside, the rider plays the return-home sound every 30 seconds by default. Change **Return-home sound** to set the interval, or set it to zero to mute it. The sound uses the radio's mention chime. On radios with custom audio, select your WAV file for the mention sound in the sound settings. Silent settings still apply.

### Two owner phones

On a Bluetooth-equipped rider, pair and connect one phone with the MeshCore companion app. Open **Owner phones** on the radio and save it in slot 1. Disconnect it, connect the second phone and save it in slot 2. Either registered phone counts while its encrypted Bluetooth connection is active. With no slots enrolled, bike monitoring works over the mesh by itself.

During a ride, if both phones are disconnected for the rider's **Missing timeout** (180 seconds by default), the rider sends a separation notice to the team and checks in at least once per minute while separated. Reconnecting either phone sends a recovery notice. Closing the phone app or a suspended Bluetooth connection can also cause separation.

## Monitor a Keymind RAK tag

1. Set both clocks and match both radios' LoRa settings. Get the tag's advertisement and save its contact on the guardian. Compare the public keys when pairing.
2. In **Kid on a bike > RAK tag monitor > Choose tag**, select the saved RAK contact. The radio remembers one tag by its full public key. This mode works independently of Team.
3. On the native RAK tracker console, enter `pet owner <guardian-radio-public-key>` and enable pet mode with `pet on`. Use `pet advert` to announce the tag. For the Keymind Sensor text console, use `set tracker.owner <guardian-radio-public-key>` and `set tracker.mode pet`. See the [native tracker setup](https://github.com/mikecarper/MeshCore/blob/497348490d31cbfb05e387d5163ceca6f6ba324c/examples/rak_findmy/README.md) for the exact commands for your build.
4. Open **RAK tag monitor** and tap **Refresh** after a check-in. It shows the tag's last check, battery voltage and GPS status. **Last fix on map** shows the most recent valid position. A check without GPS preserves the previous fix.
5. Choose **Mark lost** or **Mark safe**. The next authenticated check receives that answer. Lost requests a 120-second awake lease and the tag follows its configured lost reporting interval. Tag boundary alerts arrive in the guardian's ordinary private chat. Timing depends on the tag's configuration, GPS and mesh coverage.

## Games

Open **Apps > Games**.

- **Artillery** is a Scorched Earth-style game. Choose **Local** for a CPU opponent. For a mesh game, choose **Private**, select up to three player contacts and open the lobby. The invitees open **Games > Artillery** and accept. Start once the players have joined. For a room game, log in to the same room server first and select **Room**. The host chooses teams or free-for-all. Each radio needs compatible Artillery firmware. Use the on-screen angle and power controls, then fire on your turn.
- **Four in a row** is for two people sharing a radio. Select a column to drop a piece. Four in a line wins.
- **Tic-tac-toe** is for two people sharing a radio. Take turns choosing an empty square. Three in a line wins.

## Firmware and checks

The feature flag is `CAP_BIKE_GAMES`. It defaults on for the standard T-Deck and both T-LoRa Pager radio variants. For a Pager, match the firmware to the radio chip printed on your board. The builds use the current WadaMesh TAK partition layouts. A local app image is for an existing compatible OTA layout. A merged USB image includes the bootloader and partition table and starts at address 0.

For a field check, walk the rider past a small saved boundary with the guardian in range. Confirm an exit and a return in family chat. Power down just the rider and check that the guardian signals overdue, then recovery after the next check-in. For multiplayer, invite a second radio, fire one shot on each, and compare terrain and turn order.

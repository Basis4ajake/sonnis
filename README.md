# Sonnis: a Sonos remote for Nintendo 2DS / 3DS

Sonnis is a homebrew app that controls Sonos speakers over your local Wi-Fi. It
uses the speakers' own UPnP/SOAP interface on port 1400, so it needs no account,
no cloud and no phone. The top screen shows what's playing and the touch screen
has the controls.

## Features

| Area | What works |
|---|---|
| Discovery | SSDP auto-discovery, zone-group topology, manual IP fallback, auto-rediscovery |
| Playback | Play/pause, next/prev, volume (room or group), mute, shuffle, repeat, progress |
| Now Playing | Title, artist, album, album art (JPEG/PNG), radio stream titles, elapsed/duration |
| Browse | Sonos Favorites, Sonos Playlists, Queue (play/remove/clear), Artists, Albums, Tracks, Genres, Radio, Recently Played |
| Rooms | Room list with availability, select room, join/leave groups (leaders hand off cleanly), pin favorite rooms |
| Personalize | 8 theme presets (incl. animated Deadhead tie-dye), 8 accent colors, per-color hex overrides, clock format, volume step, square or spinning-vinyl album art |
| Song effects | Dual-screen animated scenes that follow the artist: Grateful Dead family → tie-dye, Disco Biscuits → space disco lasers, Ozzy/Sabbath → Halloween storm |
| Fun | Ozzy "Prince of Darkness" vocabulary (explicit), sound bites, hidden Party mode, more easter eggs |
| Tools | Network diagnostics, network log, crash dumps and Wi-Fi install via a built-in web server |
| Persistence | Last room, pinned rooms, manual IPs, look & behaviour, play history |

### What needs the cloud (and why it's marked in the app)

Sonos only exposes **streaming-service browsing** (Spotify, Apple Music,
Amazon, …) and **account-wide "recently played"** through its cloud API. That
API needs OAuth and modern TLS, which isn't practical on 2DS hardware.
Sonnis handles this like so:

- **Music Services** shows an explanation rather than a fake list.
- **Workaround:** add any album, playlist or station to *Sonos Favorites* in
  the official app. Favorites store their playback URI on the speaker, so Sonnis
  can play them locally, streaming-service content included.
- **Recently Played** is kept locally: it lists what you started from this 2DS.

## Controls

| Button | Now Playing | Lists (Browse/Settings) | Rooms |
|---|---|---|---|
| A | Play / Pause | Open / play | Control this room |
| B | — | Back | Back |
| X | Next track | Play whole album/playlist · *Queue:* remove track | Join this room to my group |
| Y | Previous track | Previous track · *Queue:* clear queue | Leave group |
| D-pad | ↑ Rooms · ↓ Browse · ← Shuffle · → Repeat | Move (←/→ page; change value in Settings) | Move |
| L / R | Volume down / up (hold to repeat) | same | same |
| START | Settings | Save & close settings | Settings |
| SELECT | — | Return to Now Playing | Return to Now Playing |

Touch works everywhere too: tap the big buttons, drag the volume slider,
drag lists to scroll, and tap the ▶ on an album row to play it without opening it.

## Installing

1. Copy `sonnis.3dsx` to `sdmc:/3ds/sonnis/sonnis.3dsx` on your SD card
   (or install over Wi-Fi, see below).
2. Launch the **Homebrew Launcher** and pick **Sonnis**.
3. Make sure the 2DS is connected to the **same Wi-Fi network** as your Sonos
   system (System Settings → Internet Settings).

Settings are saved to `sdmc:/3ds/sonnis/settings.ini` (plain text, safe to edit
on a PC) and history to `sdmc:/3ds/sonnis/history.txt`.

### Testing and installing over Wi-Fi

- **Try a build without installing it:** open the Homebrew Launcher, press **Y**
  for the netloader, then run `3dslink -a <2ds-ip> sonnis.3dsx`. The build
  only runs until you exit.
- **Keep a build (dev builds only):** a dev build (`make DEV=1`, see
  [Building](#building)) accepts uploads while **START → Network log** is open:

  ```sh
  curl --limit-rate 60k -T sonnis-dev.3dsx http://<2ds-ip>:8080/install
  curl --limit-rate 60k -T eleven.wav      http://<2ds-ip>:8080/sounds/eleven.wav
  ```

  Keep the rate limit: at full Wi-Fi speed the 2DS's SD card writes fall
  behind and the upload stalls.

## Building

You need devkitPro with devkitARM and libctru (citro2d/citro3d come with libctru).

```sh
# Native devkitPro install
make

# Or with Docker, which also works when the host's glibc is too old for the
# current devkitARM release (e.g. Ubuntu 22.04):
docker run --rm -u $(id -u):$(id -g) -v "$PWD":/src -w /src devkitpro/devkitarm make
```

The output is `sonnis.3dsx` (with an embedded SMDH icon/title). `make clean` removes build files.

**Dev builds.** `make DEV=1` builds `sonnis-dev.3dsx` (version `1.0.0-dev`, in
`build-dev/`). It adds Wi-Fi uploads (`PUT /install`, `PUT /sounds/<name>.wav`)
and Luma crash-dump downloads (`/dumps`) to the Network log screen's web server.
These have no authentication, so anyone on the same Wi-Fi could replace the app
while that screen is open. Release builds leave them out and only serve the
read-only log. Use dev builds on your own device only. If you install a release
build, uploads stop working until you load a dev build again with `3dslink` or
the SD card.

## Personalizing

From **Settings** (START):

- **Theme:** Midnight (default), Mono, Ocean, Forest, Plum, Daylight (light mode), Handheld (retro green),
  **Deadhead**: an animated tie-dye spiral that swirls faster while music plays,
  a colour-cycling accent, and rainbow progress and volume bars (original artwork, no band logos)
- **Accent color:** theme default, Amber, Coral, Sky, Mint, Violet, Gold, White.
  Text on accent-colored buttons switches between black and white
  automatically, so it stays readable.

Changes preview live. For full control, edit `settings.ini` on a PC and
override any color slot with a hex value:

```ini
[look]
theme = Ocean
accent = Coral
color.bg = #101418
color.accent = #FF8800
# slots: bg surface raised line text muted accent ok err
```

## Easter eggs

- **Song effects:** the background follows the artist on **both** screens:
  - **Grateful Dead family** (Dead & Co, Jerry Garcia, Jefferson Airplane, Hot
    Tuna, the archive releases …): tie-dye, a rose-crowned skull, dancing
    bears, tambourine terrapins and crows.
  - **The Disco Biscuits:** a space disco laser show over a light-up dance
    floor.
  - **Ozzy / Black Sabbath:** a Halloween storm with lightning on both
    screens, bats, a graveyard and jack-o'-lanterns.
- **Party mode:** in Settings, press **"Sonnis 1.0.0" seven times** to reveal a
  hidden row that forces any effect on every song, or turns song effects off.
- **Vocabulary → Prince of Darkness:** explicit, Ozzy-flavoured wording
  throughout. Every room becomes "Fucking …" or "Bloody …".
- **Ozzy sound bites:** WAVs in `sdmc:/3ds/sonnis/sounds/` play on events, in
  Ozzy mode only, and need `sdmc:/3ds/dspfirm.cdc` (run DSP1 once). The files
  are `start`, `eleven`, `bears`, `sharon` (errors), `lastsong`, `quit`, and
  `random1`, `random2`, … (song changes). Use 8/16-bit PCM;
  `ffmpeg -i in.mp3 -ac 1 -ar 22050 -sample_fmt s16 eleven.wav` converts
  anything else.
- **Vinyl:** Settings → Album artwork → Vinyl makes the album art spin as a
  33⅓ record. Hold the circle pad to scratch through the song.
- Press **R at volume 100**: it goes to eleven.
- Press **↑↑↓↓←→←→ B A** for the bears. Do it in Settings on the About row,
  where the arrows do nothing else.

## Fonts

Text uses the 2DS system font, drawn at whole-pixel positions for sharpness.
`source/ui_font.c` can optionally use pre-rendered fonts instead: put `.bcfnt`
files in `romfs/fonts/` (see `tools/make_fonts.sh`) and add `ROMFS := romfs` to
the Makefile. This was tried with Inter and looked worse on real hardware, so
it's off by default.

## Architecture

See **[ARCHITECTURE.md](ARCHITECTURE.md)** for the full write-up: threads, the
shared snapshot, discovery, the HTTP/SOAP quirks of the 2DS socket layer,
playback routing, grouping, drawing, effects, vinyl rotation, sound and the
log server.

In short, the UI thread draws at 60 fps and never touches the network. A
worker thread owns all speaker I/O, polls the active room every second and
publishes a snapshot behind a lock. Button presses update the screen straight
away and the next poll corrects it if the speaker disagrees.

---

## Network setup & troubleshooting

### Requirements
- The 2DS and the Sonos speakers must be on the **same subnet** (for example
  both on 192.168.1.x).
- TCP port **1400** to the speakers and UDP **1900** multicast (SSDP) must be allowed.
- The 2DS supports **2.4 GHz Wi-Fi only**, with WPA2 at most (not WPA3-only
  networks). The speakers can be on 5 GHz, but the two bands must be bridged
  on the same LAN, which is normal for home routers.

### "No speakers found"
1. **Check the subnet.** In Settings, the *About* row shows the 2DS's IP. In
   the Sonos app, *Settings → System → About My System* lists each speaker's IP.
   The first three numbers should match.
2. **Guest networks / client isolation** block device-to-device traffic. Connect
   the 2DS to your main network.
3. **Multicast filtering** (common on mesh systems, "IGMP snooping" or
   "multicast enhancement" settings) can block SSDP discovery. Sonnis handles
   this itself. When SSDP finds nothing, it scans your subnet for port 1400
   (the top screen shows *"Scanning network…"*, about 5–7 s). It saves the
   first speaker it finds, so later launches connect straight away. If even
   that fails, use **Settings → Add speaker IP…** and enter any one speaker's
   IP. One speaker is enough, because it reports every room in the house.

   Discovery order: last known speaker → SSDP → saved IPs → subnet scan.
4. Give your speakers **DHCP reservations** on your router so their IPs
   don't change.

### Diagnosing problems
- **START → Network diagnostics** tests the connection to one speaker step by step.
- **START → Network log** lists recent failures and retries ("retry → OK" means
  a hiccup Sonnis recovered from). While it's open, `http://<2ds-ip>:8080/`
  serves the full log (dev builds also list crash dumps at `/dumps`).

### "Living Room unreachable"
- The speaker may be powered off, rebooting, or have a new IP. Sonnis retries
  and runs discovery again after three failed polls. You can also force a
  refresh from **Rooms → Refresh**.
- After the 2DS wakes from sleep, Sonnis checks the network and rediscovers
  speakers automatically.

### "Shuffle/repeat not available for this source"
Radio stations and line-in/TV sources have no queue, so Sonos rejects play-mode changes for them.

### No album art
- Art from the speaker's own `/getaa` proxy (local library, most services) works.
- Art hosted on **https** URLs can't be fetched (no TLS). You'll see the placeholder note icon.
- To save bandwidth, turn art off in Settings.

### Can't play a Spotify / Apple Music item
Only items saved as **Sonos Favorites** or in the queue can be played locally. Add them to Favorites in the Sonos app first.

## License

MIT, see [LICENSE](LICENSE). Bundles [stb_image](https://github.com/nothings/stb)
(public domain / MIT).

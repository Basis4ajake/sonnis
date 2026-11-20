# Sonnis architecture

How Sonnis is put together: what each thread does, how a button press turns
into a speaker command, how the screens are drawn, and why the code is shaped
the way it is. For installing and using the app, see [README.md](README.md).

## The big picture

A 2DS has a 268 MHz ARM11 CPU, 64 MB of RAM for apps, 2.4 GHz Wi-Fi and a
socket service that's slow and quirky. Sonos speakers answer SOAP requests on
port 1400, but sometimes take seconds to do it. So the design follows one
rule:

> **The UI thread never touches the network.** Every speaker call runs on a
> worker thread. The UI draws from a shared snapshot at 60 fps, whatever the
> speakers are doing.

```
                ┌───────────────────────────── 2DS ─────────────────────────────┐
                │                                                               │
  buttons ─────►│  MAIN THREAD (prio 0x30)            WORKER THREAD (prio+1)    │
  touch         │  ─────────────────────              ──────────────────────    │
                │  input.c   read keys/touch          controller.c              │
                │  ui.c      screens, effects         ├ command queue (32)      │
                │     │                               ├ poll status   1 s       │
                │     │ ctrl_send(cmd) ──────────────►├ poll volume   3 s       │──► SOAP / HTTP
                │     │                               ├ topology     20 s       │    port 1400
                │     │ ◄─── ctrl_state() snapshot ───┤ album art               │◄── Sonos speakers
                │     │       (LightLock)             └ discovery / rescan      │
                │     ▼                                                         │
  top screen ◄──│  citro2d ── GPU                     LOG SERVER THREAD         │◄─► PC: curl
  bottom screen │                                     logserver.c :8080         │    (log, dumps,
                │  sound.c ── DSP (ndsp)              (Network log screen only) │     uploads)
                └───────────────────────────────────────────────────────────────┘
```

## Source map

| File | Role |
|---|---|
| `main.c` | Startup and shutdown order, sleep/wake hook, the 60 fps loop |
| **Networking** | |
| `network.[ch]` | Socket service (`socInit`, 1 MB buffer), Wi-Fi status, local IP |
| `sonos_http.[ch]` | HTTP/1.1 client plus SOAP envelope, timeouts, retries, abort flag |
| `sonos_discovery.[ch]` | SSDP M-SEARCH, subnet scan, ZoneGroupTopology parsing |
| `sonos_player.[ch]` | AVTransport, RenderingControl, GroupRenderingControl actions |
| `sonos_library.[ch]` | ContentDirectory browsing, plus "play this item" routing |
| `albumart.[ch]` | Art download, then JPEG/PNG decode (stb_image), then a 256×256 box-filter resize |
| `xml_util.[ch]` | Tiny, low-allocation XML helpers: find, attribute, (un)escape |
| `controller.[ch]` | **The worker:** command queue, polling, reconnects, shared state |
| `diag.[ch]` | On-device network diagnostics (Settings → Network diagnostics) |
| `netlog.[ch]` | Ring buffer of network events, mirrored to `debug.log` |
| `logserver.[ch]` | Small HTTP server: log; dev builds add crash dumps and Wi-Fi install |
| **UI** | |
| `ui.[ch]` | All screens, drawing primitives, effects, easter eggs |
| `ui_theme.[ch]` | Theme presets, accents, HSV helper, accent cycling |
| `ui_font.[ch]` | Text measure and draw (system font, pixel-snapped; optional bcfnt) |
| `input.[ch]` | Key snapshot, auto-repeat, tap vs. drag detection |
| `ozzy.[ch]` | "Prince of Darkness" vocabulary filter applied to all UI text |
| `sound.[ch]` | WAV loader and DSP playback for Ozzy sound bites (off by default, `make SOUND=1`) |
| `settings.[ch]` | `settings.ini` and `history.txt` persistence |

## Threads and shared state

### Main thread (`main.c` → `ui_frame()`)

Each frame:

1. `input_poll()` captures keys and touch once. Taps, drags and auto-repeat are
   worked out here so every screen sees the same snapshot.
2. Input filters run before any screen sees the input:
   - **Carried-over touch block:** a touch that *opened* a screen is ignored on
     the new screen until the stylus lifts. Without this, tapping "Settings"
     released onto whatever list row was underneath; once that was "Quit".
   - **Konami tracker:** swallows the final B/A so they don't also act as
     Back or Play.
3. `global_input()` handles START, SELECT and L/R volume. Then the current
   screen's `*_input()` runs.
4. **Lock** the shared state, then:
   - sync album art
   - pick the effect mode
   - hand room names to the Ozzy filter
   - draw both screens

   Then **unlock**.
5. Things that touch files, like a sound bite, run **after** unlocking, so a
   slow SD card can never block the worker.

### Worker thread (`controller.c`)

The worker has a 64 KB stack and runs one priority level below the UI. Its
loop:

```
wait for a command (or the next timer)
├─ command?  → run it (play, next, volume, browse, join, …)
│              then poll again soon so the screen catches up
├─ every 1 s → GetTransportInfo + GetPositionInfo        (what's playing)
├─ every 3 s → play mode + volume + mute                 (slow poll)
├─ every 20 s→ ZoneGroupTopology                         (rooms/groups)
├─ Wi-Fi down/up or woke from sleep → re-check, rediscover
└─ 3 failed polls in a row → rediscover (speaker may have a new IP)
```

### The shared snapshot (`ctrl_state_t`)

There is one struct, protected by one `LightLock`. It holds:
- speakers and groups
- the active room
- transport state, track and elapsed time
- volume, mute, shuffle and repeat
- album-art pixels and a generation counter
- the current browse list
- diagnostics output
- the current toast

The worker holds the lock only to **copy results in**. All network I/O
happens with the lock released, so the UI never waits on a speaker.

**Optimistic UI.** Pressing play, mute or shuffle, or dragging volume, changes
the snapshot right away and *then* queues the command. The next poll, about a
second later, corrects the screen if the speaker disagreed. Volume is
*coalesced*: dragging the slider stores only the latest value, and the worker
sends that one.

**Elapsed time** is estimated between polls: `elapsed + (now - sample time)`
while playing. The progress bar moves smoothly although the speaker is only
asked once a second.

## Networking

### Discovery (`sonos_discovery.c`, `controller.c: discover()`)

Discovery tries these in order and stops at the first that works:

1. **Last known speaker:** the saved room's IP. This usually connects
   instantly.
2. **SSDP:** three M-SEARCH packets to `239.255.255.250:1900`, collecting
   replies.
3. **Saved IPs:** from `settings.ini`, either typed in or learned.
4. **Subnet scan:** TCP connects to port 1400 across the /24, 16 at a time,
   1.2 s per batch. Finding one speaker is enough, because any speaker reports
   every room through `ZoneGroupTopology`. The IP it finds is saved for next
   time.

Topology parsing builds the room list:
- group membership
- which room coordinates its group
- group size
- hidden satellites (surrounds, subs, bonded pairs), marked "invisible"

### HTTP and SOAP (`sonos_http.c`)

Every request is HTTP/1.1 with `Connection: close`. It reads until
`Content-Length` and decodes chunked bodies. Several hard-won details from
testing on real hardware:

| Problem seen on the 2DS | Fix |
|---|---|
| `SO_ERROR` returns a bogus −26 even after a good connect | Check the result with a **second `connect()`**: `0` or `EISCONN` means connected |
| The first SYN to a host is dropped (ARP resolution), so connects took about 3 s | **ARP warm-up:** send a 1-byte UDP packet to port 9, wait 60 ms, then connect. This is cached per host for 60 s |
| Commands failed on the first try, then worked | 7 s connect timeout, 8 s reply timeout measured *between packets*, and one retry for safe actions |
| Speakers reset the connection right after a complete body | Treat as success once `Content-Length` bytes have arrived |
| `bind()` to port 0 fails for UDP | SSDP sends without binding |

**Retries.** `soap_call()` retries once, after 200–500 ms, on a timeout,
I/O error or UPnP 701 ("busy"). Actions that **aren't safe to repeat** never
retry: `Next`, `Previous`, `AddURIToQueue`, `RemoveTrackFromQueue`,
`DelegateGroupCoordinationTo`, `BecomeCoordinatorOfStandaloneGroup`. If the
first one secretly worked, a repeat would skip twice or fail.

**Abort.** `net_abort_all()` sets a flag that every wait loop checks, every
250 ms. On quit, the worker is told to stop and joined (5 s limit). If it
somehow doesn't stop, `main()` **skips `socExit()`**. Removing the socket
service under a live worker caused an early pre-release crash: a NULL
dereference inside `close()`.

**Friendly errors.** `report()` turns UPnP fault codes into plain messages:

| Code | Message |
|---|---|
| 701 | "*Room* is busy" |
| 711 / 714 | "No next track" or "Already at the first track" |
| 800 / 712 | "Not available for this source" |
| 402 | "Rejected" |

### Playback routing (`sonos_library.c: sonos_play_item`)

| Item | What Sonnis does |
|---|---|
| Queue entry | Switch to the queue (`x-rincon-queue:`), seek to that track, play |
| Radio or stream | `SetAVTransportURI` with its metadata, play |
| Container (album, playlist, `x-rincon-cpcontainer:`) | Clear queue, add the container, switch to the queue, seek 1, play |
| Single track | Append to the queue, seek to it, play |
| Favorite with no URI (artist shortcut) | Shown as info only: open it in the Sonos app |

Favorites store their own playback URI on the speaker. That's why streaming
content saved as a Favorite plays locally, even though browsing a streaming
service needs Sonos's cloud.

### Grouping

- **Join:** `SetAVTransportURI(x-rincon:<coordinator uuid>)` on the joining
  room.
- **Leave (normal member):** `BecomeCoordinatorOfStandaloneGroup`.
- **Leave (group leader):** `DelegateGroupCoordinationTo(<other member>,
  RejoinGroup=0)`. Sonnis prefers a member that answered recently. This is
  what the Sonos app does. The rest of the group keeps playing, and the
  handover is quick. Asking a leader to "go standalone" instead made it
  rebuild the group itself, which took over 8 s.

### Album art (`albumart.c`)

The worker downloads `/getaa?...` (relative to the speaker) or a plain-HTTP
URL. stb_image decodes it (JPEG/PNG only), then a box filter resizes it to
256×256 RGBA and bumps `art_gen`. The UI notices the new generation and
copies the pixels into a GPU texture. GPU textures use 8×8 tiles with Morton
(Z-order) inside each tile, so the copy swizzles the pixels into that layout.
HTTPS art can't be fetched (no TLS) and shows a placeholder.

## Drawing

### Screens

`ui_frame()` draws the **top** screen (400×240) and then the **bottom** screen
(320×240) with citro2d. The top screen shows one of three things:
- Now Playing
- diagnostics output
- the network log

The bottom screen shows the current screen:

| Screen | Contents |
|---|---|
| `SCR_NOW` | Transport buttons, volume slider, shuffle/repeat pills, four nav buttons |
| `SCR_BROWSE` | A stack of up to 12 list levels (root → favorites → album → …) |
| `SCR_ROOMS` | Rooms with availability, pins and group actions |
| `SCR_SETTINGS` | Value rows (←/→) and action rows (A); Party mode is hidden |
| `SCR_DIAG`, `SCR_NETLOG` | Tools |

Lists share one widget (`list_input` and `draw_list`). It handles cursor, page
jumps, drag-to-scroll and tap. A callback fills in each row's title, subtitle,
right-hand value and marks.

### Text (`ui_font.c`, `ozzy.c`)

All text goes through `text()` and `measure()`, which:
1. Pass the string through **`vox()`**, the Ozzy filter.
2. Shorten it with "…" to fit the available width.
3. Snap it to whole pixels, using the system font.

Because measuring and drawing both go through `vox()`, layout always fits the
words actually shown.

The Ozzy filter does two passes:
1. **Table pass:** exact, prefix (`"Connecting to *"`) and suffix
   (`"*is busy - try again in a moment"`) matches. These rewrite the UI and
   the worker's toasts without touching the code that produces them.
2. **Room pass:** every whole-word room name becomes "Fucking *Room*" or
   "Bloody *Room*". The word is fixed per room by hashing the name, and longer
   names are matched first.

Diagnostics and the network log skip the filter so error details stay exact.

### Themes and effects (`ui_theme.c`, `ui.c`)

A theme is nine color slots: bg, surface, raised, line, text, muted, accent,
ok, err. Applying one resolves them into `C`, the palette all drawing code
reads. Text on accent-colored buttons picks black or white by luminance.

The animated background is chosen fresh **every frame**:

```
Party mode (hidden setting)
├─ Theme only  → the theme's own effect (tie-dye if Deadhead, else none)
├─ Deadhead / Disco lasers / Sabbath → that effect, always
└─ Auto (default)
     track_fx(artist, album, title):
       Ozzy / Sabbath family     → FX_SABBATH
       Disco Biscuits family     → FX_LASER
       Grateful Dead family      → FX_TIEDYE   (incl. Jefferson Airplane, archives…)
       otherwise                 → theme's own effect
```

| Effect | Top screen | Bottom screen |
|---|---|---|
| Tie-dye | Swirling HSV spiral (10 rings × 40 segments, per-vertex color), crows | Second spiral, rose-crowned skull, dancing bears, tambourine terrapins, crows |
| Lasers | Space gradient, twinkling stars, perspective grid floor, mirror ball, 7 sweeping beams | Pulsing disco floor; the **same** beams continue down; sparkles |
| Sabbath | Storm sky, moon with glow, clouds, graveyard hill, dead tree, mist | Misty graveyard, flickering jack-o'-lanterns |
| (all) | Effect-colored progress and volume bars, play-button halo | |

**One tall screen.** Beams, bats, crows and rain are drawn in *world*
coordinates, with the top screen at the origin. On the bottom screen they're
shifted by (40, 260), which is roughly where it sits physically, and the GPU
clips everything else. That's how a beam or a bat crosses the hinge. Shared
state, such as the lightning bolt's timer and shape, updates once per frame,
so both screens flash together.

**Cost control on a 268 MHz CPU:**
- The tie-dye's hue grid (about 450 HSV conversions) is cached per frame and
  shared by both screens.
- The spiral's trig is computed once.
- Star positions come from an integer hash, so no table is stored.

### Spinning vinyl

citro2d can rotate an image, but only as a whole square, and a square's
corners would stick out past a round record. So the **CPU** rotates the art:

- `rotate_art()` writes a rotated, circle-cropped copy into one of two
  textures, alternating each frame. The GPU never reads a texture the CPU is
  writing.
- It uses 16.16 fixed-point stepping per row, and only the pixels inside the
  circle are touched.
- The corners are cleared once at startup and never written again.
- When the record isn't moving, rotation is skipped entirely.

Spin and spin-up/down are **time-based**: angle += speed × elapsed time. A
frame-based spin visibly slowed whenever a frame was dropped, such as when
the worker's 1 s poll landed on the same frame. Holding the circle pad
scrubs: the record spins 8× faster, and letting go sends `Seek`.

### Easter eggs

| Egg | Where it lives |
|---|---|
| Volume to eleven | `global_input`: R pressed at 100 → toast plus `eleven.wav` |
| Konami bears | `konami()`: a 10-step matcher; the third ↑ is still a valid two-step prefix |
| Party mode unlock | Settings → About pressed 7 times within 1.5 s gaps; the row is spliced in with `set_row_id()` |
| Quit confirmation | Press twice within 3 s |

## Sound (`sound.c`)

**Off by default.** Only `make SOUND=1` builds it (defines `SONNIS_SOUND`).
Otherwise `sound.h` replaces every function with an empty inline stub, so call
sites need no `#ifdef`s, the Settings row disappears, `PUT /sounds` is
rejected, and the CIA doesn't request the DSP service. Run `make clean` when
toggling.

`ndspInit()` needs the DSP firmware at `sdmc:/3ds/dspfirm.cdc`, made once by
the DSP1 homebrew. Without it, sound is off and every call does nothing.

Clips are uncompressed WAVs (8/16-bit, mono/stereo), parsed chunk by chunk.
Each one is loaded into linear memory only when it plays (up to about 3 MB),
and 8-bit samples are flipped from unsigned to signed.

Clips play **only in Ozzy mode**. Events are picked by watching state, not
wired into the worker:
- new error toast → `sharon.wav`
- "No next track" → `lastsong.wav`
- song changed → a 1-in-3 chance of `random*.wav`
- direct triggers: `start`, `eleven`, `bears`, `quit`

On quit, Sonnis waits up to 5 s for the goodbye clip to finish.

## Log server (`logserver.c`)

The log server is a small HTTP server on its own thread, **running only while
START → Network log is open**:

| Request | Build | Does |
|---|---|---|
| `GET /` | all | Last 128 KB of `debug.log` |
| `GET /dumps` | dev | Lists Luma3DS ARM11 crash dumps |
| `GET /dump/<name>` | dev | Downloads one dump (name checked: no paths, no `..`) |
| `PUT /install` | dev | Saves the body as `sdmc:/3ds/sonnis/sonnis.3dsx` (keeps a netloaded build) |
| `PUT /sounds/<name>.wav` | dev + `SOUND=1` | Adds a sound bite |

"dev" endpoints exist only in `make DEV=1` builds (`SONNIS_DEV` defined). They
have no authentication, so release builds compile them out entirely and answer
`not available in this build`; a settings toggle could be flipped back on.

Uploads write to `<file>.part` and rename only when every byte has arrived,
so a dropped transfer never leaves a broken app. A partial upload is deleted
and logged with the socket error. In practice, send with
`curl --limit-rate 60k`. At full Wi-Fi speed the 2DS's SD writes fall behind
and the transfer stalls.

## Startup and shutdown order (`main.c`)

```
gfxInitDefault → settings_load → apply theme → netlog_init → network_init
→ ui_init (citro2d, textures, fonts) → input_init → ctrl_start (worker)
→ aptHook (sleep/wake) → sound_init → start.wav
   … 60 fps loop …
quit.wav → ctrl_stop (abort flag, join 5 s) → wait for quit.wav → sound_exit
→ settings_save → ui_exit (also stops the log server)
→ network_exit (only if the worker really stopped) → gfxExit
```

On wake from sleep, `ctrl_on_wake()` re-checks Wi-Fi and rediscovers, because
sockets don't survive sleep reliably.

## Files on the SD card

```
sdmc:/3ds/sonnis/
  sonnis.3dsx     the app (Homebrew Launcher lists it)
  settings.ini    rooms, IPs, behaviour, look, vinyl, ozzy, party mode
  history.txt     "Recently Played" (started from this 2DS, max 15)
  debug.log       network log (also served over Wi-Fi)
  sounds/*.wav    Ozzy sound bites (SOUND=1 builds only)
sdmc:/3ds/dspfirm.cdc        DSP firmware (needed for sound)
sdmc:/luma/dumps/arm11/      crash dumps (readable via the log server, dev builds)
```

## Debugging a crash

1. With a dev build, open START → Network log on the 2DS, then
   `curl http://<2ds-ip>:8080/dumps` and
   `curl -O http://<2ds-ip>:8080/dump/<name>`.
2. Keep each build's `sonnis.elf` (or `sonnis-dev.elf`) somewhere outside the
   repo; a dump can only be decoded with the exact ELF that crashed. Decode
   the dump's PC and LR with `arm-none-eabi-addr2line -e sonnis.elf -f -i <addr>`.

# Sonos kitchen controller — ESP32-2432S024C

Touch controller for the Sonos room **Køkken**, running on a Sunton ESP32-2432S024C
(Cheap Yellow Display, 2.4" capacitive). Talks to the speakers directly over the local
UPnP/SOAP interface on port 1400; no cloud, no account.

## Features

- Now-playing screen: album art, title, artist, album (or station name for radio), clock (NTP, Danish time)
- Play / pause, previous, next; for radio streams the prev/next slots become mute and favourites
- Volume +/- buttons (2 steps per tap, hold to repeat) and mute act on the whole group
  (GroupRenderingControl); the status bar shows "+N" when grouped
- Track progress bar with elapsed/total time for normal tracks (hidden for radio), extrapolated locally
  between Sonos updates
- Queue view: tap the album art while a track plays to see the queue around the current track and jump
  to another track (tap the art during radio to open favourites instead)
- Wi-Fi setup without recompiling: if no network can be joined the board opens the hotspot
  "Sonos-Remote" with a captive portal at http://192.168.4.1 where you pick the network and enter the
  password (stored in NVS). Also reachable from the room list ("Wi-Fi-opsætning"). `include/secrets.h`
  remains an optional compile-time fallback
- Buttons react instantly on tap
- **Push updates**: the board subscribes to UPnP events (AVTransport, GroupRenderingControl, ZoneGroupTopology)
  and runs a tiny NOTIFY listener on port 3400, so changes made in the Sonos app appear within a fraction of a
  second. SOAP polling remains only as a 20 s safety net; subscriptions renew every 5 min
- **Room picker**: tap the room name to list all zone groups. Join the kitchen to a group, leave a group,
  or switch which room the remote controls (persisted in NVS)
- Favourites list from the Sonos app (radio stations and playable items)
- Auto-discovers the group coordinator for the room via SSDP + zone topology, caches it in NVS,
  and follows the room into/out of groups via topology events (plus a 60 s safety refresh)
- Ambient-light backlight dimming (LDR), idle dim after 2 min, screen off after 5 min when nothing plays
  (first touch wakes the screen without triggering a button)
- **Multi-room pages**: swipe left/right on the now-playing screen to move between zone groups (page dots
  under the status bar). Swiping changes the controlled room for the session; the saved home room is restored
  at boot. Use the room picker's "Styr ... i stedet" or the web page to change the home room
- **Web configuration** at http://sonos-koekken.local/ (or the IP): home room, backlight levels, dim/sleep
  timeouts, screen rotation (USB up or down), volume step, OTA password, plus buttons for Wi-Fi setup and
  reboot. Settings live in NVS and apply immediately (rotation too); OTA password applies after reboot
- Over-the-air updates (ArduinoOTA, `sonos-koekken.local`)

## Remote logging, watchdog and crash reports

- Every log line (ours and the Arduino/IDF libraries') is mirrored as UDP syslog to the receiver
  `python3 tools/logserver.py` on the Mac; lines land in `logs/<hostname>.log`. Boards find the receiver via
  its UDP broadcast beacon (port 5515); the `Logserver` field on the web page (name or IP, default empty) is only a
  fallback for networks without the beacon and is not looked up once a beacon has set the target (each mDNS miss cost
  a 2 s block and an ESPmDNS "Host was not found!" warning every 10 min).
  Lines are buffered in RAM (6 KB) until the receiver has answered a RLOG-HELLO handshake, because the LAN path
  is often unusable for the first ~30 s after boot; then the backlog is flushed. On the Mac the receiver runs as
  the launchd agent `dk.mikkel.esp32-logserver` (starts at login, restarted if it dies): install or refresh it with
  `tools/install-logserver-agent.sh`. The agent runs a copy of the script from `~/Library/Application Support/`
  and writes to `~/Library/Logs/esp32-boards/`, because launchd-spawned processes are denied access to the external
  volume the repo lives on; `logs/` in the repo is a symlink to that directory. Rerun the install script after
  editing `tools/logserver.py`.
- Always-on alternative (so logs are captured while the Mac is off): the same receiver in a Debian LXC on the
  Proxmox host `pve` (192.168.68.78). `tools/proxmox/setup-logserver-lxc.sh` creates CT 111 `esp32-logserver`
  (DHCP on vmbr0, onboot), installs the script as the systemd unit `esp32-logserver` (udp/5514) plus a read-only
  HTTP viewer on :8080 and weekly logrotate, and then stops the Mac launchd agent: two receivers must never run at
  once, because both broadcast the beacon and the boards would flap between them. Needs key-based SSH as root on pve
  (`ssh-copy-id root@192.168.68.78` once). Rerun the script after editing `tools/logserver.py`. Read logs with
  `tools/boardlog.sh sonos-koekken` (tail over SSH) or `http://<ct-ip>:8080/`; the IP is saved in
  `tools/proxmox/logserver-ip.txt`.
- Grafana + Loki + Alloy in the same container (`tools/proxmox/setup-grafana-lxc.sh`, packages from apt.grafana.com):
  Alloy tails `/var/log/esp32-boards/*.log`, sets labels `board` (file name), `level` (I/W/E/D/V -> info/warn/error/
  debug/trace, BOOT lines -> `boot`) and `tag` (the `[sonos]`/`[data]`/... prefix), parses the receiver's timestamp
  (Europe/Copenhagen) and pushes to Loki (localhost:3100, filesystem store, 90-day retention). Grafana on
  `http://<ct-ip>:3000` (first login admin/admin, then set a password) has two provisioned dashboards in folder ESP32:
  "ESP32 board" (heap/largest block/RSSI/LDR graphs from the minute status line via LogQL `unwrap`, boot reports,
  warnings+errors, all lines filtered by tag) and "ESP32 overview" (all boards). Dashboards are generated by
  `tools/proxmox/grafana/make_dashboards.py`; rerun it and the setup script after edits (Grafana reloads provisioned
  files every 30 s; UI edits are lost on the next push). Gotchas learned: run Alloy only after Loki reports ready,
  because once newer lines of a stream are stored Loki rejects anything older than ~1 h for that stream ("too far
  behind") and Alloy drops a batch after its retries (now 200 retries, 5 min max backoff); imported history older than
  3 h is only queryable after a chunk flush (`POST /flush?wait=true`, the script does this); a LogQL `regexp` with more
  than one named group turns the extra groups into labels and splits the series, so the metric patterns have exactly one
  group and are wrapped in `max by (board)`. `tools/proxmox/grafana/verify.sh` (run inside the CT) prints a health summary.
- Prices (EDS DayAheadPrices) are parsed while streaming (`PriceSink` in `src/info/data.cpp`, fed by `pumpBody()`,
  a 512-byte stack-buffer reader with its own chunked decoding; `HTTPClient::writeToStream` was tried first and failed
  with "too less ram! need 4096" on every fetch, because during TLS no 4 KB heap block exists), so the body is never allocated: during a TLS session mbedTLS holds ~40 KB and
  the whole-body reserve failed regularly ("prices: hukommelse"). The old scanner also skipped the first record of the
  day because `"sort":"TimeDK"` in the header matched the key search. LVGL charts always get >= 2 points
  (`LV_CHART_POINT_NONE` as filler): `lv_chart` divides by `point_cnt - 1` and a 1-point bar chart panicked the board
  (IntegerDivideByZero) whenever the price data ended within the current hour.
- Home Assistant polling stops at the first host-level failure (connection refused, timeout) instead of trying every
  entity and group member, and backs off 30 s, 60 s, 2 min, 4 min, then 5 min until HA answers again (`HaData::hostDown`).
  Last known values stay on screen with the error "Home Assistant svarer ikke".
- Kitchen board firmware updates (ArduinoOTA and HTTP `/update`) go through `otaPrepare()`: the album-art sprite
  (~39 KB) is freed on top of the Sonos buffers and servers, so the updater sees ~95 KB like the info board. Before this,
  six OTA attempts died at 0 % because lwIP could not buffer the incoming TCP data at ~57 KB free. ArduinoOTA's receive
  timeout is raised from 1 s to 10 s (`setTimeout`), the source of "OTA fejl 3" on the jittery kitchen link. A failed
  update now restarts the board instead of trying to restore the torn-down servers. The first update onto a board still
  running the old firmware has to go over USB: `tools/deploy.sh cyd`. Verified 2026-09-20: with the buffers released the
  board has ~82 KB free during the update and a 1.6 MB upload completes in 47 s. Prefer `tools/ota-via-ct.sh <env> <ip>`,
  which runs espota from the log-server container: espota makes the board connect back to the uploader, and the Mac's
  own Wi-Fi path to the boards is often bad ("OTA fejl 2" = connect-back failed, each failure now restarts the board).
- Info board fetches read the HTTP body in their own loop (30 s deadline, 12 s idle timeout, watchdog reset per
  chunk, chunked encoding undone in place). `http.getString()` on a trickling link at -88 dBm starved the 60 s task
  watchdog and rebooted the board (2026-09-12). A failed first fetch after boot is retried after 20 s instead of
  2-3 min, because DNS/TLS often fail in the first seconds after Wi-Fi comes up.
- Sonos remote: album art streaming has a 30 s whole-body deadline, SOAP responses a 15 s one, both reset the task
  watchdog per chunk, and every SOAP/GENA call resets it before and after the request. A failed OTA on a stalled
  link used to trigger four ~10 s failing (un)subscribe calls in a row and a task-wdt reboot (2026-09-12 17:01).
- `tools/deploy.sh <env> [host-or-ip]`: pass the IP when mDNS does not resolve. espota needs a link without
  multi-second stalls (it gives up after 10 s without an ack); `tools/httpupdate.sh` aborts after a 5 s stall.
- The build timestamp in the boot line only changes when `main.cpp` recompiles; the `elf=<sha8>` is the reliable
  identity and matches the archived ELF file name.
- At boot each board sends `BOOT build ... elf=<sha8>, reset: <reason>`; if a crash dump is found in the
  coredump partition it appends `CRASH task=... cause=... pc=... elf=<sha8> bt=<addresses>` and erases the dump.
- Deploy with `tools/deploy.sh <env>`: it builds, uploads and archives the ELF as
  `builds/<date>-<env>-<sha8>.elf`. To decode a crash, pick the ELF whose sha matches the report and run
  `~/.platformio/packages/toolchain-xtensa-esp-elf/bin/xtensa-esp32-elf-addr2line -pfiaC -e builds/<file>.elf <bt addresses>`.
- Wi-Fi resilience: with stored credentials a board retries forever (no setup hotspot); a signal below -80 dBm
  for 2 minutes forces a reconnect because the ESP32 does not roam between mesh nodes on its own; 2 minutes
  without a successful Sonos call reconnects Wi-Fi and 5 minutes reboots.
- A 60 s hardware task watchdog covers the UI loop and the Sonos task; a freeze of either reboots the board.
  The heap and network watchdogs from before remain.

## Memory notes

The classic ESP32 has ~110 KB of heap after Wi-Fi; this firmware runs at ~65-70 KB free after loading art.
Things that keep it there: a single 20-line LVGL draw buffer, art decoded straight from the socket, persistent
SOAP receive buffers, favourites kept without their metadata blobs, queue widgets freed when leaving the screen,
and the web page streamed in 600-byte chunks. A heap watchdog reboots the board if free memory stays below
18 KB for 30 s, and a network watchdog reboots after two minutes of failed Sonos calls.

## Stand (3D print)

`hardware/stand.stl` is a 65° desk stand for the board in portrait with the USB-C at the bottom
(set rotation "USB-stik nedad" on the web page). `hardware/stand.scad` is the editable OpenSCAD source and
`hardware/make_stand.py` regenerates the STL without OpenSCAD:

```bash
uv run --with trimesh --with manifold3d --with numpy --with shapely --with scipy python hardware/make_stand.py
```

Board data from the factory drawing: PCB 74.4 x 42.9 mm, four Ø3.2 mm holes on a 68.0 x 36.5 mm grid, 3.2 mm
from the edges. The stand has Ø6.5 mm standoffs 5 mm tall with Ø2.6 mm holes for M3 self-tapping screws
(drill to 3.2 mm for through-bolts), a 13 mm cable slot at the bottom edge, and an open back. Print with the
inclined face on the bed; no supports needed. About 48 cm³ of material.

## First-time setup

1. `pio run -e cyd -t upload` over USB.
2. The board starts the hotspot "Sonos-Remote"; connect a phone to it and enter your 2.4 GHz Wi-Fi
   details in the page that pops up (or open http://192.168.4.1). Alternatively copy
   `include/secrets.h.example` to `include/secrets.h` and fill in the credentials before building.

## Code layout

| File | Role |
|---|---|
| `src/main.cpp` | Setup, LVGL glue, Sonos task (core 0), album-art download, backlight |
| `src/sonos.h/.cpp` | SOAP client: discovery, transport, volume, favourites, XML helpers |
| `src/ui.h/.cpp` | LVGL screens: status, now playing, favourites |
| `src/app.h` | Command enum shared between UI and Sonos task |
| `include/lgfx_cyd_2432s024c.hpp` | LovyanGFX board class (display + touch pins) |
| `include/lv_conf.h` | LVGL configuration |

## Hardware
Sunton ESP32-2432S024C: ESP32-D0WD-V3, 4 MB flash, CH340 USB-serial on `/dev/cu.usbserial-120`.

| Peripheral | Pins |
|---|---|
| ILI9341 240x320 SPI display | MOSI 13, MISO 12, SCLK 14, CS 15, DC 2, backlight 27 |
| CST820 capacitive touch (I2C) | SDA 33, SCL 32, RST 25, INT 22 |
| RGB LED (active low) | R 4, G 16, B 17 |
| Light sensor (LDR) | 34 (ADC) |
| Speaker amp (FM8002A) | 26 (DAC) |
| microSD (SPI) | CS 5, MOSI 23, MISO 19, SCLK 18 |
| Free GPIO on connectors | 21, 22 (shared with touch INT), 35 (input only) |

## Graphics stack

- **LovyanGFX** drives the ILI9341 and the CST820 touch. The board class with all pins is
  `include/lgfx_cyd_2432s024c.hpp`. If the image is mirrored or rotated, change
  `offset_rotation` there (0-3 rotate, 4-7 rotate + mirror).
- **LVGL 9.2.2** for the UI. Config in `include/lv_conf.h`; glue (flush + touch callbacks) in `src/main.cpp`.
- **Touch orientation**: the CST820 reports both axes mirrored (rotated 180°) relative to the panel. `TOUCH_FLIP_X=1` / `TOUCH_FLIP_Y=1`
  in `platformio.ini` correct this in the LVGL read callback.
- **Fonts**: `src/fonts/font_dk_{12,14,16,20}.c` are Montserrat + FontAwesome symbols generated with
  `lv_font_conv` including Latin-1 (æ ø å). LVGL's built-in Montserrat fonts are ASCII only. Regenerate with
  `npx lv_font_conv@1.5.3 --bpp 4 --size 14 --no-compress --font Montserrat-Medium.ttf -r 0x20-0x7F,0xA0-0xFF,0x2022 --font FontAwesome5-Solid+Brands+Regular.woff -r <symbols> --format lvgl --lv-include lvgl.h -o font_dk_14.c`
  (fonts and the symbol list live in `.pio/libdeps/cyd/lvgl/scripts/built_in_font/`).
- **Album art**: external https art URIs are fetched through the speaker's own proxy
  `http://<speaker>:1400/getaa?s=1&u=<urlencoded track URI>`, which serves plain HTTP with chunked encoding.
  The image is decoded by LovyanGFX straight from the socket through a small de-chunking `HttpBodyStream`
  (zoom 0 = fit-to-box), so no download buffer is needed; the ESP32 heap is too tight for one.

## Commands

```bash
pio run                        # build
pio run -e cyd -t upload       # flash over USB (first install)
pio run -e cyd_ota -t upload   # update over Wi-Fi (board announces itself as sonos-koekken.local)
pio device monitor             # serial monitor at 115200
```

OTA: the board runs ArduinoOTA on port 3232 after joining Wi-Fi. The screen shows update progress and
Sonos polling pauses during the transfer. If mDNS is flaky, pass the IP instead:
`pio run -e cyd_ota -t upload --upload-port 192.168.68.102`. An optional OTA password goes in
`include/secrets.h` (`OTA_PASSWORD`) plus `--auth=<password>` in the `[env:cyd_ota]` upload_flags.

## Info board (board 2)

`src/info/` is a second application for the same hardware: three swipeable pages (tap or swipe to switch).

1. **Elpris** - DK1 day-ahead prices from Energi Data Service (`DayAheadPrices`, 15-min resolution) shown as
   the current price, a 24-hour bar chart from the current hour (green/yellow/red by level) and cheapest /
   most expensive hour. Surcharge (nettarif + elafgift) and VAT are settings on the web page.
2. **Vejr** - Open-Meteo: current temperature, description, wind, 24-hour temperature curve, 4-day cards.
   Coordinates and place name on the web page (default Tilst).
3. **Bus** - Rejseplanen departure board for one line and direction from one stop (default 3A -> Aarhus Ø
   from Skjoldhøjskolen), with real-time delays and minute countdowns. Works without a key: the board uses
   Rejseplanen's legacy HAFAS station board (`stboard.exe ... L=vs_java3`, plain HTTP, ~1 KB of XML per
   fetch, undocumented so it may vanish; `e_delay` is the live prognosis the Midttrafik app shows, `delay` only the
   confirmed delay, so the board uses `e_delay` when present). If a Rejseplanen API key is entered on the web page the official
   ReST API is used instead. Stop lookup goes through `ajax-getstop.exe` (legacy) or `location.name`
   (official). Refreshes every 90 s. `http://infotavle.local/deps` dumps the parsed list (planned, realtime, flags)
   for checking against Rejseplanen.
4. **Hjem** - up to 8 Home Assistant entities (label + state + unit) polled every 60 s through the REST API
   (`GET /api/states/<entity_id>`, plain HTTP on the LAN, https also works). Configure on the web page: HA address
   (found via mDNS: `http://192.168.68.96:8123`, HA 2026.9.1), a long-lived access token (create it in HA under
   Profile -> Security; the user enters it on the board, it never leaves the LAN) and the entity list, one per
   line as `entity_id|Label` (label optional -> HA friendly_name). A line `a+b+c|Åbne vinduer` counts how many of
   the listed entities are on/open/unlocked ("2" or "Ingen"). States are shown in Danish and read according to the
   entity's device_class (a window: Åben/Lukket, a lock: Låst/Ulåst, a plug: Tilsluttet/Frakoblet, alarm panel:
   Frakoblet/Tilkoblet, vacuum: I dock/Støvsuger, mower: Slår græs, ...), numbers with at most one decimal and the
   unit.

Orientation: the info board defaults to landscape (rotation 1, USB-C to the right); all four rotations are
selectable on the web page and applied after the automatic restart. Layouts adapt to 320x240 or 240x320.
Stand: `hardware/stand-landscape.stl` (82 x 55 x 50 mm, generator `make_stand_landscape.py`); the USB plug
sits in free air to the right of the stand.

Screen: LDR-based brightness, idle dim after N min, and two ways to switch the screen off: a night plan
(hours, default 23-06) and "sluk i mørke" (room dark for N min). A touch wakes it for `wakeSec` seconds
(the waking touch is swallowed). The minute log line reports rssi, ldr, backlight level and inactivity.

Build/flash: `tools/deploy.sh info2` (USB), `tools/deploy.sh info2_ota` (espota, hostname `infotavle.local`)
or `tools/httpupdate.sh info2_ota infotavle.local` (upload through the web server; robust on weak links).
The same platform code is used (Wi-Fi portal "Infotavle", OTA, web config, remote log, watchdogs).
All data is fetched over HTTPS (`WiFiClientSecure::setInsecure`, ~45 KB heap per connection); the info board
has ~125 KB free heap since it carries no album-art buffer or Sonos client.

## Second board

A second ESP32-2432S024C (MAC 24:dc:c3:9a:6e:1c) runs the same firmware with its own hostname:

```bash
pio run -e cyd2 -t upload        # USB
pio run -e cyd2_ota -t upload    # OTA -> sonos-remote-2.local
```

Its original firmware (an ESPHome 2024.12 LVGL config that boot-looped for lack of PSRAM) is backed up as
`backup/board2-original-4MB.bin`. Both boards default to the room in `secrets.h`; change the home room on each
board's web page. The boot log prints `touch probe:` to tell the capacitive (C) and resistive (R) variants apart.

## Backup

`backup/original-firmware-4MB.bin` is a full flash dump of the board as received (LVGL button demo).
Restore with:

```bash
uvx esptool --port /dev/cu.usbserial-120 --baud 115200 write-flash 0 backup/original-firmware-4MB.bin
```

## Wall mount (3D print)

`hardware/wallmount-shell.stl` + `hardware/wallmount-bezel.stl` form a slim wall enclosure (49 x 94 x 16 mm).
Source: `hardware/wallmount.scad`, generator `hardware/make_wallmount.py` (same uv command as the stand).

- Shell: 3 mm back plate with two keyhole slots on the centre line (18 and 58 mm from the top) for wall
  screws, four Ø6.5 standoffs on the board's hole grid (5 mm tall, Ø2.6 holes), a 10 mm notch in the bottom
  wall for the USB-C plug/cable and a Ø10 rear hole under the plug area for in-wall cabling. 14 mm of space
  below the board takes a straight USB-C plug; a right-angle plug also fits.
- Bezel: 2 mm frame with a 41 x 54 mm window (the 2.4" active area is 36.7 x 49 mm, the rest shows the
  black glass border), countersunk holes and spacers that bear on the PCB corners.
- Assembly: hang the shell on the wall, drop the board in with the USB-C at the bottom, put the bezel on,
  fasten with 4 x M3 x 12 mm screws through bezel and PCB into the standoffs. Set rotation
  "USB-stik nedad" on the web page.
- v3: every boss (shell standoffs, bezel spacers, stand bosses) stands on a concave fillet so the joint between
  tube and plate has material to bond to, and the bezel holes have a real 90° countersink. Shared primitives in
  `hardware/hw_common.py`; the `.scad` files carry the same `filleted_boss()` / `countersunk_hole()` modules.
- Print: shell open side up, bezel face down. No supports.
- v2 (after the first print): USB notch and rear hole moved 2.5 mm left (`plug_dx = -2.5`, the connector is
  off-centre) and widened to 13 mm; bezel window moved 3 mm up (`window_dy = 2.0`) so the top status bar and
  the back button sit clear of the bezel edge.

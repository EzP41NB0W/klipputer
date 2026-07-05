# Klipputer — pocket KlipperScreen for the M5Stack Cardputer ADV

Live status + remote control for a Klipper 3D printer via Moonraker,
on a ~$50 M5Stack Cardputer ADV (ESP32-S3, 240x135). Built for a
Flashforge Adventurer 5M running community Klipper, but it speaks
plain Moonraker JSON-RPC — any Klipper printer should work.

Dashboard with live temps/progress/ETA, temperature control with a live
graph, jog/home controls, gcode file browsing + print start, macro
runner with did-it-actually-finish feedback, and an interactive gcode
console with command history. WebSocket push updates (no polling), HTTP
streaming parse for the bulky stuff — runs entirely in internal heap,
no PSRAM needed.

MIT licensed. Built collaboratively with Claude (Anthropic), tested on
real hardware against a real printer.

## Build & flash

1. Copy `src/secrets_example.h` to `src/secrets.h` and fill in your WiFi
   credentials + Moonraker host/port. Never commit real credentials; the
   file is gitignored.
2. `pio run -t upload` (same toolchain as range-toolkit/cardmeleon).
3. `pio device monitor` at 115200 if you want serial output.

## Screens & keys

Number keys **1–6** jump between screens from anywhere.
**h** or **ESC** backs out (ESC = this keyboard's real 0x35 code).

| Screen | Keys |
|---|---|
| **1 Dashboard** | `p` pause/resume · `c` cancel (confirm) · `x` E-STOP (confirm) · `;`/`.` select SPD/FLW/FAN while printing · `,`/`/` adjust (hold to ramp, debounced send) |
| **2 Temps** | `;`/`.` nozzle/bed · `,`/`/` adjust target · `e` type a number (`c` clears) · `ENT` send · `0` heater off · `p` cycle presets PLA/PETG/ABS/OFF |
| **3 Move** | `w`/`s` Y± · `a`/`d` X∓ · `;`/`.` Z± · `t` step 0.1/1/10/25 · `g` home menu · `m` motors off · `e`/`r` extrude/retract 5mm |
| **4 Files** | `;`/`.` navigate · `ENT` details → `ENT` print (confirm) · `r` refresh |
| **5 Macros** | `;`/`.` navigate · `ENT` run (toast: "running..." with dots → green OK / red FAILED when Klipper actually finishes it) · `r` reload |
| **6 Console** | `ENT` open command line (type any gcode/macro, `ENT` sends with OK/FAIL feedback, `ESC` exits, `fn+↑` recalls last command, `fn+↓` clears line) · `;`/`.` scroll log · `r` restart menu · `x` E-STOP · `b` speaker test |
| **7 Objects** | exclude-object plate map: `;`/`.` select · `ENT` exclude (confirm) · `r` refresh — current object green, excluded red X |
| **8 Webcam** | live MJPEG view from the printer cam · `r` reconnect |

**Safety:** every print-wrecking action asks for confirmation — pause,
resume, cancel, E-STOP, mid-print temp changes (presets are locked
outright while printing), and exclude-object. **Z babystep:** while
printing, the dashboard tune row has a `Z` item — `,`/`/` microsteps the
gcode offset ±0.005mm (hold to ramp) for dialing in the first layer.

**`0` opens a keymap help overlay from any screen** (except Temps, where
`0` is heater-off). Top bar tabs show their jump key (1–6). Connection
dot: green = klippy ready, yellow = WS only, red = offline; plus WiFi
bars and battery %.

Beeps: ascending chirp on print complete, low buzz on error/shutdown,
short warn on local refusals (cold extrude, unhomed jog). If it's ever
silent, check the 3.5mm jack-detect switch before blaming code
(range-toolkit lesson, 2026-06-28).

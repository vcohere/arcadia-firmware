# WLtoys 6401 Direct-Drive Firmware (XIAO ESP32-S3 Sense)

## 1. What it does

This is prototype firmware for a **Seeed Studio XIAO ESP32-S3 Sense** that
replaces the WiFi/camera module in a WLtoys 6401 1:64 FPV car and drives it
directly over the car's internal signal wire. Control is now **network-only**:
the board joins your 2.4 GHz WiFi, streams its onboard camera over the LAN, and
takes discrete steer/throttle commands over a WebSocket. The four physical
push buttons of the earlier revision have been removed.

The network stream + control contract (for a separate webapp) is documented in
[`NETWORK_API.md`](NETWORK_API.md). There is **no authentication** — the device
is meant to live on a trusted LAN.

## 2. Supported model

WLtoys 6401 Mini RC Car, 1:64 scale, FPV variant. Firmware target board is the
Seeed Studio XIAO ESP32-S3 Sense (ESP32-S3R8, 8 MB octal PSRAM, 8 MB flash,
onboard OV2640/OV3660 camera).

## 3. Reverse-engineering background

The protocol was reverse-engineered from raw sigrok/PulseView `.sr` logic
captures (12 MHz, 8 channels, only one active) of the car's internal yellow
signal wire — no datasheet, no prior decoder. Run-length analysis of the raw
waveform (not a decoder overlay) found every run to be an exact multiple of
2500 samples, which uniquely identifies 4800 baud. A from-scratch UART 8N1
decoder then confirmed 0 framing errors across 136 decoded bytes. Full
methodology, evidence tags ([CAP]/[REF]/[INF]/[ASM]), and reproduction
instructions are in [`PROTOCOL_ANALYSIS.md`](PROTOCOL_ANALYSIS.md); rerun the
analysis yourself with:

```
python3 tools/analyze_captures.py
```

## 4. Protocol summary

- Transport: **UART 4800 8N1, non-inverted, idle HIGH, LSB-first.**
- Frame: 8 bytes, repeated every **~50 ms** (not the reference app's 100 ms
  UDP interval — the yellow wire runs faster than the WiFi hop).

```
byte 0 : 0x66   start marker      (constant)
byte 1 : STEER  steering          (0x80 centre, 0x59 left, 0xA6 right)
byte 2 : THR    throttle          (0x80 neutral, 0xFF forward, 0x00 reverse)
byte 3 : 0x80   constant
byte 4 : 0x00   constant
byte 5 : 0x00   constant
byte 6 : CHK    XOR checksum = byte1 ^ byte2 ^ byte3
byte 7 : 0x99   end marker        (constant)
```

Reference packets (captured, byte-for-byte verified):

| Command  | Packet (hex)                      |
|----------|------------------------------------|
| Idle     | `66 80 80 80 00 00 80 99`          |
| Forward  | `66 80 FF 80 00 00 FF 99`          |
| Backward | `66 80 00 80 00 00 00 99`          |
| Left     | `66 59 80 80 00 00 59 99`          |
| Right    | `66 A6 80 80 00 00 A6 99`          |

This 8-byte frame is exactly the last 8 bytes of the 16-byte UDP packet used
by the reference control app (see §19); the outer 8-byte prefix is stripped
somewhere between the app and the yellow wire (see
[`PROTOCOL_ANALYSIS.md`](PROTOCOL_ANALYSIS.md) §8).

## 5. Pin assignment

Only two connections to the car are needed — the signal wire and a shared
ground. Everything else (WiFi, camera) is internal to the XIAO board.

| Function               | GPIO  | XIAO pad |
|-------------------------|-------|----------|
| Signal out (UART1 TX)   | GPIO4 | D3 |
| Ground                  | GND   | GND (shared) |

The onboard camera uses internal-only GPIOs that are **not** broken out to
pads (XCLK=10, PCLK=13, D0–D7=15/14/16/17/18/11/12, D8=48, VSYNC=38, HREF=47,
SIOD=40, SIOC=39), and none collide with GPIO4. GPIO5–8 (the old button pins)
are no longer used.

## 6. Wiring table

| From              | To                          | Notes |
|-------------------|-----------------------------|-------|
| XIAO GPIO4 (D3)   | Car yellow signal wire      | Through a level check / shifter if required — see §9 |
| XIAO GND          | Car controller GND          | Common ground, required |

## 7. Simple wiring diagram

```
                 +-------------------------+
                 |   XIAO ESP32-S3 Sense   |
                 |                         |
                 | GPIO4 (D3) |--- (level check!) --- car yellow wire
                 |                         |
                 |        GND |--- shared GND --- car controller GND
                 +-------------------------+
       (onboard camera + WiFi; no external buttons)
```

## 8. Control (network)

Control is over WiFi — there is no button wiring. The board joins the network
set in [`main/config.h`](main/config.h) (`WIFI_SSID` / `WIFI_PASSWORD`) and
serves a WebSocket control endpoint, a pull-based camera snapshot endpoint, and a built-in
test page. Open `http://<ip>/` in a browser for the built-in UI, or drive it
from a separate app per [`NETWORK_API.md`](NETWORK_API.md). Commands are
discrete (`steer` = left/center/right, `throttle` = forward/neutral/reverse),
mapping to the verified capture bytes. A **400 ms failsafe** returns the car to
neutral if command frames stop arriving.

## 9. Car signal wiring

- GPIO4 (UART1 TX) connects to the car's signal input, in place of the
  removed WiFi/camera module's output.
- **~10 kΩ external pull-up on GPIO4**, tied to whatever rail represents
  logic HIGH for the car's input (3.3 V for direct connection, or the
  car-side rail *after* a level shifter if one is used). GPIO4 is
  floating from chip reset until firmware configures the UART — spanning
  power-on, the ROM bootloader, and every flash/reset cycle — and this
  resistor holds the line at idle/mark during that whole window.
- Shared GND between ESP32-S3 and car controller is required.
- **Electrical warning:** a logic analyzer only records digital timing, never
  voltage. Before connecting GPIO4 to the car, measure the yellow wire's HIGH
  voltage, physically isolate the original WiFi/camera module so it can't
  drive the line at the same time as the ESP32, and add a level shifter if
  the car runs 5 V logic. Full checklist in §14 and
  [`IMPLEMENTATION_PLAN.md`](IMPLEMENTATION_PLAN.md) §8.
- **Power the XIAO from its own supply** (USB/battery) with GND shared to the
  car. This matters more than before: WiFi **and** the camera now draw current
  transients that can brown out the board if powered from an undersized rail
  (e.g. the car's own regulator). Do not power the board from the car until
  that rail's current capacity has been verified.

## 10. Build prerequisites

- ESP-IDF v5.x installed and exported (`. $HOME/esp/esp-idf/export.sh` or
  equivalent).
- USB cable to the XIAO's USB-C port (console runs over USB-Serial-JTAG).
- `idf.py` on `PATH`.
- Internet access on first build — the `espressif/esp32-camera` managed
  component is fetched from the ESP Component Registry (see
  `main/idf_component.yml`).

The board-specific config (8 MB flash, octal PSRAM for camera framebuffers,
the custom `partitions.csv` giving the app 3 MB, and HTTP WebSocket support) is
in `sdkconfig.defaults` — no manual `menuconfig` needed. Set your WiFi SSID and
password in [`main/config.h`](main/config.h) before building.

## 11. How to build

```
idf.py set-target esp32s3
idf.py build
```

## 12. How to flash

```
idf.py -p <port> flash
```

## 13. How to open serial logs

```
idf.py -p <port> monitor
```

Logging runs over USB-Serial-JTAG, independent of GPIO4/UART1, so opening the
monitor never disturbs the car signal.

## 14. First safe test

Follow the staged bring-up in
[`IMPLEMENTATION_PLAN.md`](IMPLEMENTATION_PLAN.md) §9 in order — do not skip
stages:

1. **Stage A:** ESP32 alone, logic analyzer on GPIO4, car disconnected.
   Confirm idle HIGH, 4800 baud, 8N1, 0 framing errors, Idle packet decodes to
   `66 80 80 80 00 00 80 99`, ~50 ms cadence.
2. **Stage B:** diff the Stage-A decode against this repo's reference
   captures with `tools/analyze_captures.py` — byte-identical expected.
3. **Stage C:** connect to the car with wheels lifted off the ground,
   Idle only. Motors must not move.
4. **Stage D:** single-direction commands over `ws://<ip>/control`, wheels
   still lifted (one axis at a time).
5. **Stage E:** combined-axis commands (e.g. `steer:left throttle:forward`)
   and the failsafe (stop sending → car returns to neutral within ~400 ms),
   wheels still lifted.

Only run the car on the ground after Stage E passes.

## 15. Expected behavior

Steer and throttle are set independently by the WebSocket command (see
[`NETWORK_API.md`](NETWORK_API.md)):

| Command                                   | STEER  | THR    |
|-------------------------------------------|--------|--------|
| `steer:center throttle:neutral` (Idle)    | 0x80   | 0x80   |
| `steer:center throttle:forward`           | 0x80   | 0xFF   |
| `steer:center throttle:reverse`           | 0x80   | 0x00   |
| `steer:left   throttle:neutral`           | 0x59   | 0x80   |
| `steer:right  throttle:neutral`           | 0xA6   | 0x80   |
| `steer:left   throttle:forward`           | 0x59   | 0xFF   |
| `steer:right  throttle:forward`           | 0xA6   | 0xFF   |

The first ~1 s after boot always transmits Idle regardless of network input
(startup neutral window), and the ~5 s HIGH hold precedes even that. After
that, a command changes the payload of the *next* periodic frame — the wire
keeps emitting at a steady 50 ms. If no command arrives within **400 ms** the
firmware reverts to Idle (failsafe).

## 16. Troubleshooting

| Symptom | Likely cause |
|---------|--------------|
| No motion at all | Check level compatibility/ground/original-module isolation (§9); confirm WS commands arrive (`/status` `ws_active:true`) |
| Reversed direction | Check STEER/THR polarity and the command→byte mapping (NETWORK_API.md §3) |
| Jitter / erratic motion | Check loose wiring, WiFi dropouts (failsafe re-triggering), or bus contention with the original module |
| Not on the network | Wrong `WIFI_SSID`/`WIFI_PASSWORD` in `main/config.h`, or not a 2.4 GHz network |
| No video | Camera init failed (`/status` `camera:false`); needs octal PSRAM enabled (§10) |
| Wrong idle level on scope | GPIO4 not latched HIGH before UART init, or external pull-up missing/wrong rail |
| Framing errors on logic analyzer | Wrong baud/inversion config, or bus contention |
| Car cuts out / failsafe trips | Frame cadence stalled — verify the 50 ms control loop is still running |

## 17. Known limitations

- Steering uses the **verified partial-deflection** values (`0x59`/`0xA6`),
  not full scale (`0x00`/`0xFF`) — full-scale steering was never captured
  from the original app and is unverified for this car.
- Combined-axis commands (e.g. Forward+Left) are produced by the field model
  (set both bytes, recompute checksum) but were never captured from the
  original app running a combined command — treat as unproven until
  bench-tested per Stage E.
- No RX/telemetry: the firmware only transmits; it does not read.
- The car's failsafe/keep-alive timeout (how long it tolerates a pause in
  frames) is unknown.

## 18. Remaining unverified assumptions

Mirrors [`PROTOCOL_ANALYSIS.md`](PROTOCOL_ANALYSIS.md) §10-11:

- Steering full scale (`0x00`/`0xFF`) — unverified for this car.
- Combined-axis packets — inferred from the independent field model, not
  directly captured.
- The `ca 47 d5 00 00 00 00 00` outer prefix's exact semantics in the
  reference app's 16-byte UDP packet — inferred to be stripped before the
  yellow wire, not proven from a module firmware dump.
- Car keep-alive/timeout behavior beyond the ~166 ms capture window.
- Yellow-wire electrical levels (voltage, drive direction, ESP32 tolerance) —
  a logic analyzer records digital timing only; these must be measured
  before any physical connection (§9, §14).

## 19. Attribution

Protocol cross-checked against
[DEEFRAG/WL-Tech-FPV-CAR](https://github.com/DEEFRAG/WL-Tech-FPV-CAR)
(targets the WLtoys 6401), specifically
`xbox_gamepad_proxy_mediamtx_deadzone_browser.py`. Credit to the repo author
for the reference UDP packet format and axis-mapping/checksum logic that
this firmware's capture-derived protocol was verified against.

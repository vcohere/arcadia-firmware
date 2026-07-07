# WLtoys 6401 Direct-Drive Firmware (ESP32-S3)

## 1. What it does

This is prototype ESP32-S3 firmware that replaces the WiFi/camera module in a
WLtoys 6401 1:64 FPV car and drives it directly, using four push buttons
(Forward, Reverse, Left, Right), over the car's internal signal wire.

## 2. Supported model

WLtoys 6401 Mini RC Car, 1:64 scale, FPV variant.

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

| Function               | GPIO  |
|-------------------------|-------|
| Signal out (UART1 TX)   | GPIO4 |
| Forward button          | GPIO5 |
| Reverse button          | GPIO6 |
| Left button             | GPIO7 |
| Right button            | GPIO8 |
| Ground                  | GND (shared) |

## 6. Wiring table

| From              | To                          | Notes |
|-------------------|-----------------------------|-------|
| ESP32-S3 GPIO4    | Car yellow signal wire      | Through a level check / shifter if required — see §9 |
| ESP32-S3 GPIO5    | Forward button, one leg     | Other leg to GND |
| ESP32-S3 GPIO6    | Reverse button, one leg     | Other leg to GND |
| ESP32-S3 GPIO7    | Left button, one leg        | Other leg to GND |
| ESP32-S3 GPIO8    | Right button, one leg       | Other leg to GND |
| ESP32-S3 GND      | Car controller GND          | Common ground, required |

## 7. Simple wiring diagram

```
                 +------------------------+
                 |       ESP32-S3         |
                 |                        |
  Forward btn ---| GPIO5              GPIO4|--- (level check!) --- car yellow wire
  Reverse  btn ---| GPIO6                  |
  Left     btn ---| GPIO7                  |
  Right    btn ---| GPIO8                  |
                 |                    GND |--- shared GND --- car controller GND
                 +------------------------+
     (each button's other leg -> shared GND)
```

## 8. Button wiring

Each button has one leg wired to its GPIO (5/6/7/8) and the other leg to
shared GND. Firmware enables the internal pull-up on each pin, so the pin
reads HIGH when not pressed and LOW when pressed (active-low).

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
- **Power the ESP32-S3 from its own supply** (USB/battery) with GND shared to
  the car, until the car's onboard regulator's current capacity has been
  verified — Wi-Fi/radio current transients can brown out the ESP32 if
  powered from an undersized rail.

## 10. Build prerequisites

- ESP-IDF v5.x installed and exported (`. $HOME/esp/esp-idf/export.sh` or
  equivalent).
- USB cable to the ESP32-S3's USB port (console runs over USB-Serial-JTAG).
- `idf.py` on `PATH`.

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
4. **Stage D:** momentary single-direction presses, wheels still lifted.
5. **Stage E:** combined directions and conflict cases, wheels still lifted.

Only run the car on the ground after Stage E passes.

## 15. Expected behavior

| Input state        | STEER  | THR    |
|---------------------|--------|--------|
| No buttons (Idle)   | 0x80   | 0x80   |
| Forward             | 0x80   | 0xFF   |
| Reverse             | 0x80   | 0x00   |
| Left                | 0x59   | 0x80   |
| Right               | 0xA6   | 0x80   |
| Forward + Left      | 0x59   | 0xFF   |
| Forward + Right     | 0xA6   | 0xFF   |
| Reverse + Left      | 0x59   | 0x00   |
| Reverse + Right     | 0xA6   | 0x00   |
| Forward + Reverse   | 0x80   | 0x80 (neutral throttle) |
| Left + Right        | 0x80   | — (neutral steering) |

The first ~500 ms after boot always transmits Idle regardless of button
state (startup neutral window). After that, a press changes the payload of
the *next* periodic frame — the wire keeps emitting at a steady 50 ms.

## 16. Troubleshooting

| Symptom | Likely cause |
|---------|--------------|
| No motion at all | Check level compatibility/ground/original-module isolation (§9) |
| Reversed direction | Check button-to-GPIO mapping (§5) and STEER/THR polarity |
| Jitter / erratic motion | Check debounce, loose wiring, or bus contention with the original module |
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

# ESP32-S3 Firmware — Implementation Plan

Prototype firmware to drive a WLtoys 6401 1:64 FPV car directly over the
internal yellow signal wire formerly driven by the car's WiFi/camera module,
using four push buttons.

**This is a plan. No firmware is implemented yet.** It rests on the verified
findings in [`PROTOCOL_ANALYSIS.md`](PROTOCOL_ANALYSIS.md):
UART **4800 8N1, non-inverted, idle HIGH**, 8-byte packet
`66 STEER THR 80 00 00 CHK 99` with `CHK = STEER ^ THR ^ 0x80`, repeated every
**~50 ms**.

---

## 1. Firmware framework choice — **ESP-IDF (chosen)**

**Decision: ESP-IDF v5.x, native.**

Rationale for this tiny prototype:
- The whole job is "drive one hardware UART at 4800 8N1 out of GPIO4 and poll
  four GPIOs." ESP-IDF exposes exactly this with `driver/uart.h` and
  `driver/gpio.h` — no abstraction guesswork.
- First-class control over **UART pin matrix routing** (route TX→GPIO4, leave RX
  unassigned) and over the **idle line level**, which matters here (idle HIGH,
  non-inverted) and is fiddly under Arduino's `HardwareSerial`.
- Deterministic, non-blocking timing via FreeRTOS ticks / `esp_timer` for the
  50 ms cadence without `delay()` habits.
- Reliable flashing/monitoring with `idf.py flash monitor`; USB-Serial-JTAG
  console is independent of GPIO4 (satisfies the diagnostics requirement).

Arduino-esp32 would also work and is marginally faster to bootstrap, but hides
UART inversion/idle-level and pin-matrix details we specifically care about.
Given only one peripheral and four GPIOs, ESP-IDF's small extra setup cost buys
full control. (If the builder strongly prefers Arduino, the same design maps
onto `HardwareSerial` on Serial1 with `uart_set_line_inverse(...)` left at
defaults — noted as a fallback, not the plan.)

## 2. Project / tooling structure — **native ESP-IDF (`idf.py`)**

Use native ESP-IDF tooling (`idf.py`), not PlatformIO — fewer moving parts for a
single-target prototype and the canonical toolchain for the framework above.

```
prototype-firmware/
├── PROTOCOL_ANALYSIS.md
├── IMPLEMENTATION_PLAN.md
├── README.md                     (to be written; plan in §11)
├── tools/
│   └── analyze_captures.py       (capture analysis, already present)
├── CMakeLists.txt                (top-level ESP-IDF project file)
├── sdkconfig.defaults            (target esp32s3, console on USB-Serial-JTAG)
└── main/
    ├── CMakeLists.txt
    ├── protocol.h                (packet struct, field constants, build fn)
    ├── protocol.c                (packet builder + checksum; unit-testable)
    ├── buttons.h / buttons.c     (debounced active-low GPIO reading)
    └── main.c                    (init, control loop, UART TX, diagnostics)
```

Keeping `protocol.c` free of ESP-IDF headers lets the packet builder be
compiled and tested on a host PC and its output diffed against the captured
bytes in `PROTOCOL_ANALYSIS.md`.

## 3. Signal output

- **Use the hardware UART peripheral** (UART1), *not* bit-banging — 4800 8N1 is
  verified, so the peripheral is exact and jitter-free.
- Config: `baud=4800, data=8, parity=none, stop=1`, no flow control, **not
  inverted** (`UART_SIGNAL_*` defaults; do not enable `UART_SIGNAL_TXD_INV`).
- **Route TX to GPIO4** via the GPIO matrix (`uart_set_pin(UART_NUM_1, GPIO4,
  UART_PIN_NO_CHANGE, ...)`). **RX not assigned** — the captures show no reply
  traffic and the reference repo never reads; RX is unnecessary.
- **Open-drain, not push-pull.** With the original module disconnected, the
  car's own board was measured pulling the yellow wire to a steady **~2.81 V**
  — the car supplies its own pull-up. GPIO4 is therefore configured
  `GPIO_MODE_OUTPUT_OD`: output level 1 (idle/mark, and every UART "1" bit)
  releases the pad to high-Z so the car's pull-up sets the HIGH level, and
  output level 0 (every UART "0"/start bit) actively pulls the line to GND.
  The ESP32 must never drive the line HIGH itself. On the ESP32-S3 this is a
  single pad-level bit (`GPIO_PINn_REG.pad_driver`, set via
  `gpio_set_direction(..., GPIO_MODE_OUTPUT_OD)`) that is independent of
  whether the pad's input comes from plain GPIO or the UART1 TX matrix
  signal, so it applies equally before and after the UART peripheral takes
  the pin over. No internal or external pull-up is added on the ESP32 side —
  the car's own pull-up is the only one on the net.
  **Correction:** while the `pad_driver` (open-drain) bit itself is
  independent of the signal routing as described above, calling
  `gpio_set_direction()` *again* after `uart_set_pin()` has a side effect
  beyond that bit: it re-invokes `gpio_output_enable()`, which reconnects the
  pad to the plain-GPIO matrix signal (`SIG_GPIO_OUT_IDX`), silently
  disconnecting UART1 TX from GPIO4 (the pin then sits released-HIGH forever
  since the output level was last set to 1, and `uart_write_bytes()` writes
  to a pad no longer connected to the pin — see `main.c`'s
  `signal_uart_start()` and the bug writeup this doc's §3 was corrected
  from). `uart_set_pin()` never touches `pad_driver`, so the OD mode set by
  the earlier `gpio_config()` already persists through `uart_set_pin()` —
  there is no need to re-assert it via `gpio_set_direction()` afterward; if
  re-asserting `pad_driver` is wanted anyway, the UART TX matrix signal must
  be explicitly reconnected right after via
  `esp_rom_gpio_connect_out_signal()`.
- **Idle level = HIGH (released).** An idle UART TX line already idles HIGH
  (mark), which — combined with open-drain — means idle is simply "released,
  car's pull-up holds it up," matching the capture. Ensure GPIO4 is released
  (not driven) from boot *before* the UART drives it (see §7) so there is no
  spurious LOW/start-bit glitch during init.
- **Cadence:** transmit the current 8-byte packet **every 50 ms** (measured),
  not 100 ms. Bytes may be written in one `uart_write_bytes()` burst; the ~16.7 ms
  on-wire frame + ~33 ms idle reproduces the observed pattern. A software timer
  or a `vTaskDelayUntil` loop at 50 ms drives it.

## 4. Packet generation

Do **not** store five raw waveforms. Generate packets from fields:

```c
// protocol.h  (values from PROTOCOL_ANALYSIS.md — all [CAP] verified)
#define PKT_START     0x66
#define PKT_END       0x99
#define PKT_CONST3    0x80   // byte 3
#define PKT_CONST4    0x00   // byte 4
#define PKT_CONST5    0x00   // byte 5

#define STEER_CENTER  0x80
#define STEER_LEFT    0x59   // verified deflection (partial, known-good)
#define STEER_RIGHT   0xA6   // verified deflection (partial, known-good)

#define THR_NEUTRAL   0x80
#define THR_FORWARD   0xFF   // verified full forward
#define THR_REVERSE   0x00   // verified full reverse

// build 8-byte packet; checksum computed dynamically
void protocol_build(uint8_t steer, uint8_t thr, uint8_t out[8]) {
    out[0] = PKT_START;
    out[1] = steer;
    out[2] = thr;
    out[3] = PKT_CONST3;
    out[4] = PKT_CONST4;
    out[5] = PKT_CONST5;
    out[6] = steer ^ thr ^ PKT_CONST3;   // == b1 ^ b2 ^ b3
    out[7] = PKT_END;
}
```

Neutral/idle is `protocol_build(STEER_CENTER, THR_NEUTRAL, ...)` →
`66 80 80 80 00 00 80 99` (matches the Idle capture; self-check at boot).

**Steering endpoints:** use the **verified** `0x59`/`0xA6`, not `0x00`/`0xFF`.
Full-scale steering was never captured and could over-travel or bind. Leave
full-scale as a documented, opt-in experiment after bench testing (§9).

## 5. Four-button control model

Direction → fields (checksum always recomputed by `protocol_build`):

| Input state        | STEER  | THR    |
|--------------------|--------|--------|
| no buttons (Idle)  | 0x80   | 0x80   |
| Forward            | 0x80   | 0xFF   |
| Reverse            | 0x80   | 0x00   |
| Left               | 0x59   | 0x80   |
| Right              | 0xA6   | 0x80   |
| Forward + Left     | 0x59   | 0xFF   |
| Forward + Right    | 0xA6   | 0xFF   |
| Reverse + Left     | 0x59   | 0x00   |
| Reverse + Right    | 0xA6   | 0x00   |

**Conflict resolution (deterministic):**
- Forward **and** Reverse pressed together → `THR = 0x80` (neutral throttle).
- Left **and** Right pressed together → `STEER = 0x80` (neutral steering).

Combined commands are produced by simply setting both fields independently and
recomputing the checksum. **This is [INF], not [CAP]** — no combined-direction
capture exists. Treat combined moves as unproven until bench-tested (§9 Stage E);
if the car misbehaves on combined input, fall back to a priority scheme
(throttle wins, steering neutral) — decided after testing.

Pseudologic:

```c
steer = STEER_CENTER; thr = THR_NEUTRAL;
if (fwd && !rev) thr = THR_FORWARD;
else if (rev && !fwd) thr = THR_REVERSE;      // both or neither -> neutral
if (left && !right) steer = STEER_LEFT;
else if (right && !left) steer = STEER_RIGHT; // both or neither -> center
protocol_build(steer, thr, packet);
```

## 6. Button handling

- Pins: **GPIO5=Forward, GPIO6=Reverse, GPIO7=Left, GPIO8=Right.**
- Config each as **input with internal pull-up** (`GPIO_PULLUP_ONLY`), active-low:
  not pressed = HIGH, pressed = LOW. Wiring: `GPIO → button → shared GND`.
- **Debounce:** poll every ~5 ms; a button's logical state flips only after it
  reads stable for ~20 ms (integrator/counter per pin). No `delay()` blocking.
- **Control loop is non-blocking:** a fast poll task updates debounced button
  state continuously; a separate 50 ms cadence uses the *latest* debounced state
  to build and send the next packet. So a press changes **the next periodic
  frame** — the wire keeps emitting at a steady 50 ms; only the payload changes.
- All ESP32-S3 GPIO4–8 are ordinary I/O with internal pull-ups and are safe here
  (no strapping conflicts: strapping pins are 0/3/45/46; GPIO4–8 are clear).

## 7. Safe startup / failsafe behavior

- **Boot into a silent released-HIGH hold (~5 s).** Immediately on boot, before
  the UART peripheral ever touches GPIO4, configure it open-drain
  (`GPIO_MODE_OUTPUT_OD`) and set output level 1 — i.e. release the line so the
  car's own pull-up (§3) holds it at its native ~2.81 V, never actively driven
  by the ESP32. Hold it released there, with **no UART traffic at all**, for a
  fixed ~5 s window. This reproduces the original WiFi/camera module's observed
  behavior of holding the yellow wire HIGH and silent before its control link
  goes active, then transitioning straight into periodic neutral traffic
  (PROTOCOL_ANALYSIS.md §10, "No RX / handshake observed" — the car's
  control-link startup logic, if any, is unproven, so this silence-to-traffic
  transition is reproduced defensively rather than assumed unnecessary).
- **Startup neutral window (~1 s) after UART start:** once the 5 s hold ends,
  attach the UART peripheral to GPIO4 and begin transmitting at the normal
  cadence, but for the first ~1 s of that traffic transmit **only the Idle
  packet** (`66 80 80 80 00 00 80 99`) and **ignore button input**, regardless
  of button state. This deliberately resolves the earlier conflict between
  "reflect button state" and "must start Idle": the button debouncer fills its
  history during this window, the checksum/builder self-test runs, and the
  line settles before any motion command is possible. After the window,
  switch to honoring the latest debounced button state.
- **Builder self-test at boot:** assert the builder reproduces
  `66 80 80 80 00 00 80 99` for neutral (and the four reference packets from
  their field values); if it does not, halt transmission and log — never emit
  throttle during init.
- **Contradictory inputs → neutral** (already handled in §5).
- **Fail-safe default:** if button reading fails or state is indeterminate,
  fall back to Idle fields. Consider an optional watchdog: if the control task
  stalls, stop toggling and let GPIO4 rest HIGH (car sees no valid frames).
- **Bring-up gate:** firmware should require the Stage A–C test sequence (§9)
  to pass before anyone runs it with wheels on the ground.

## 8. Electrical integration checkpoint (MANDATORY before wiring to the car)

The logic capture proves **digital timing only** — never analog voltage. Before
GPIO4 touches the car, measure/confirm:

1. **Yellow-wire HIGH voltage** with a multimeter/scope (idle level). If it is
   **5 V**, do **not** connect it directly to any ESP32-S3 pin.
2. **Original module disconnected / not driving** the line — two outputs fighting
   will damage one. Physically isolate the WiFi/camera module's signal output.
3. **Common ground** between ESP32-S3 GND and the car controller GND — required.
4. **3.3 V acceptance:** confirm the car controller reads a 3.3 V ESP32 output as
   a valid HIGH. If marginal, add a buffer.
5. **Level translation:** choose based on step 1 —
   - car input is 3.3 V-tolerant and 3.3 V is a valid HIGH → direct, optionally a
     ~100–330 Ω series resistor for protection.
   - car runs 5 V logic → use a proper level shifter (or MOSFET/transistor
     buffer). **Do not assume 5 V is safe for the ESP32-S3.**
   - if ESP32 must *read* the line later, protect its input (divider/shifter).
6. **Power the ESP32-S3 separately at first (shared GND).** Measuring ~3.3 V on
   the car's red wire does **not** prove that rail can source the ESP32-S3's
   current — brownout resets are likely under Wi-Fi/radio current spikes
   (hundreds of mA transients) even if this firmware doesn't actively use Wi-Fi.
   Default to powering the ESP32-S3 from its own USB/battery with **ground shared
   to the car**, and only power it from the car's rail after confirming the
   on-board regulator's current capacity and adding adequate bulk decoupling.

Do not proceed past this checkpoint on assumption.

## 9. Test strategy (staged, with pass/fail)

**Stage A — ESP32 alone, logic analyzer on GPIO4 (car disconnected).**
Capture GPIO4 at 12 MHz.
*Pass:* idles HIGH; bit period 2500 samples (4800 baud); 8N1, 0 framing errors;
Idle packet decodes to `66 80 80 80 00 00 80 99`; repeat cadence ≈50 ms.
*Fail:* any framing error, wrong bytes, wrong idle level, or cadence off >±10 %.

**Stage B — Diff against the original module capture.**
Compare Stage-A decode to the reference captures in this repo (reuse
`tools/analyze_captures.py`).
*Pass:* byte-identical packets for Idle and each direction; cadence within ±10 %.

**Stage C — Connect to car, wheels lifted off the ground, Idle only.**
Power on; only Idle transmitted.
*Pass:* motors do **not** move; no runaway; controller powers up normally.
*Fail:* any wheel/steering motion at Idle → stop, recheck neutral bytes/wiring.

**Stage D — Momentary single-direction, wheels still lifted.**
Briefly press Forward, then Reverse, then Left, then Right individually.
*Pass:* drive wheels spin forward/reverse; steering deflects left/right; returns
to neutral on release; correct direction per button.
*Fail:* reversed/incorrect direction, no motion, or no return to neutral.

**Stage E — Combined directions, wheels lifted.**
Forward+Left, Forward+Right, Reverse+Left, Reverse+Right; then the conflict cases
Forward+Reverse and Left+Right.
*Prerequisite (preferred):* first **capture the four combined commands from the
original app** (Forward+Left, Forward+Right, Reverse+Left, Reverse+Right) and
decode them with `tools/analyze_captures.py`. If they match the field-model
prediction (both bytes set, `CHK=STEER^THR^0x80`), the combined-axis packets are
verified [CAP] and Stage E only re-confirms them on hardware; if they differ,
correct the model before bench testing.
*Pass:* combined moves set both axes; Forward+Reverse → no drive; Left+Right →
centred steering.
*Fail:* erratic behaviour on combined input → adopt priority fallback (§5).

Only after Stage E passes should the car be run on the ground.

## 10. Diagnostics

- **USB-Serial-JTAG console** for logs — independent of GPIO4 (UART1), so logging
  never disturbs the signal. `idf.py monitor`.
- On each state change (and/or every N frames) print: **debounced button state**
  (F/R/L/Rt) and the **current 8-byte packet in hex**.
- **Compile-time `DEBUG` flag** (`menuconfig`/macro) to enable verbose per-frame
  logging; off by default to keep the 50 ms loop light.
- Optional: a boot self-test line confirming the builder reproduces all five
  reference packets from their field values.

---

## 11. README plan (to be written as `README.md`)

The future README must be writable from this plan without re-deriving anything.
Required sections and the content each pulls from:

1. **What it does** — ESP32-S3 replaces the WiFi/camera module and drives the
   WLtoys 6401 directly via 4 buttons over the internal signal wire.
2. **Supported model** — WLtoys 6401 Mini RC Car 1:64 (FPV).
3. **Reverse-engineering background** — summarise `PROTOCOL_ANALYSIS.md`: sigrok
   captures at 12 MHz, single active line, UART discovered from raw run lengths.
4. **Protocol summary** — 4800 8N1, idle HIGH, LSB-first; 8-byte packet
   `66 STEER THR 80 00 00 CHK 99`, `CHK = STEER^THR^0x80`; ~50 ms cadence; the
   five reference packets table; note relation to the reference repo's 16-byte
   UDP packet (last 8 bytes).
5. **Pin assignment** — table:

   | Function | GPIO |
   |----------|------|
   | Signal out (UART1 TX) | GPIO4 |
   | Forward button | GPIO5 |
   | Reverse button | GPIO6 |
   | Left button    | GPIO7 |
   | Right button   | GPIO8 |
   | Ground | GND (shared) |

6. **Wiring table** — every connection incl. shared GND.
7. **Simple wiring diagram** — ASCII: ESP32 GPIO4→(level check)→yellow wire;
   buttons GPIO5–8→button→GND; common GND to car.
8. **Button wiring** — one leg to GPIO, other leg to shared GND; internal
   pull-ups; pressed = LOW.
9. **Car signal wiring** — GPIO4 → car signal input; shared GND; **~10 kΩ
   pull-up on GPIO4 to the logic-HIGH rail** (holds idle HIGH during ESP32
   reset/boot); **electrical warning** (measure voltage, isolate original
   module, level-shift if 5 V); **power the ESP32-S3 from its own supply with
   shared GND** until the car's regulator capacity is verified.
10. **Build prerequisites** — ESP-IDF v5.x, USB cable, `idf.py`.
11. **How to build** — `idf.py set-target esp32s3 && idf.py build`.
12. **How to flash** — `idf.py -p <port> flash`.
13. **How to open serial logs** — `idf.py -p <port> monitor` (USB-Serial-JTAG).
14. **First safe test** — reproduce Stage A–C: capture GPIO4, verify bytes,
    connect with wheels **lifted**, Idle first.
15. **Expected behavior** — per-button mapping table from §5.
16. **Troubleshooting** — no motion (check level/ground/isolation), reversed
    direction, jitter, wrong idle level, framing errors, car failsafe cutout.
17. **Known limitations** — steering uses partial-deflection values; combined
    commands unproven until bench-tested; no RX/telemetry; failsafe timeout of
    the car unknown.
18. **Remaining unverified assumptions** — mirror `PROTOCOL_ANALYSIS.md` §10–11
    (steering full scale, prefix semantics, keep-alive timeout, electrical
    levels).
19. **Attribution** — protocol cross-checked against
    <https://github.com/DEEFRAG/WL-Tech-FPV-CAR> (WLtoys 6401); link it and
    credit the author.

---

## Blockers before firmware implementation

1. **Electrical levels unknown** (§8) — must measure yellow-wire voltage and
   decide direct vs. level-shifted before any car connection. This is a hardware
   gate, not a code gate.
2. **Original module isolation** — confirm how to disconnect/disable the WiFi
   module's drive of the line.
3. **Steering full-scale & combined commands** are [INF] — safe to code with the
   verified partial values; **capture the four combined-axis commands from the
   original app** to promote them to [CAP] (see §9 Stage E), and validate on the
   bench before trusting.
4. **Car failsafe/keep-alive timeout** unknown — 50 ms cadence matches capture;
   confirm the car doesn't cut out if frames pause.

None of these block writing the firmware against the **verified** Idle/single-
direction behaviour; they block *connecting to the car*.

# WLtoys 6401 — Yellow-Wire Protocol Analysis

Evidence-based reverse engineering of the internal control signal of a WLtoys
6401 1:64 FPV car, derived from raw sigrok/PulseView `.sr` logic captures and
cross-checked against a public reference implementation.

All packet and timing claims below are reproducible with
[`tools/analyze_captures.py`](tools/analyze_captures.py) (standard library only):

```
python3 tools/analyze_captures.py
```

Evidence tags used throughout:

- **[CAP]** VERIFIED FROM CAPTURE — measured directly from the raw waveform.
- **[REF]** VERIFIED FROM REFERENCE SOURCE — read from the reference repo code.
- **[INF]** STRONGLY SUPPORTED INFERENCE — not directly observed but strongly implied.
- **[ASM]** UNVERIFIED ASSUMPTION — plausible but not established here.

---

## 1. Capture inventory

| File | Bytes on disk | Command |
|------|--------------:|---------|
| `Idle 2M 12MHz.sr`     | 2601 | neutral / no input |
| `Forward 2M 12MHz.sr`  | 2611 | forward throttle |
| `Backward 2M 12MHz.sr` | 2620 | reverse throttle |
| `Left 2M 12MHz.sr`     | 2686 | steer left |
| `Right 2M 12MHz.sr`    | 2642 | steer right |

Each `.sr` is a ZIP containing `version` (=2), `metadata`, and one logic blob
`logic-1-1` of exactly **2,000,000 bytes** (the "2M" in the filename = 2 000 000
samples). The container is small on disk because the constant runs compress well.

## 2. `.sr` metadata (identical across all five captures) — [CAP]

```
sigrok version = 0.6.0-git-883c2ac
total probes   = 8   (D0..D7)
samplerate     = 12 MHz
total analog   = 0
unitsize       = 1   (1 byte per sample, 1 bit per channel)
```

Derived: **2 000 000 samples ÷ 12 000 000 Hz = 166.667 ms** capture window per file.

Only **channel D0** carries a signal; D1–D7 never transition in any capture.
D0 **idles HIGH** (first sample = 1; the long inter-packet runs are level 1). — [CAP]

## 3. Raw timing analysis → fundamental unit and baud — [CAP]

Run-length decomposition of D0 (see the full dump for `Idle` reproduced by the
script). Ignoring the leading/trailing partial-idle run and the huge
inter-packet idle runs, **every run length is an integer multiple of 2500
samples**:

```
observed run lengths: 2500, 5000, 20000, 22500, ...  (all = k × 2500)
2500 samples ÷ 12 MHz = 208.33 µs  →  1 / 208.33 µs = 4800.0 baud (exact)
12 000 000 / 4800 = 2500  (exactly)
```

This is the decisive measurement and it is taken from the **raw waveform**, not
from a decoder overlay. The bit period is 2500 samples with essentially zero
residual on the interior runs (e.g. `Left`: max residual 148 samples ≈ 5.9 % of
a bit; the larger residuals reported for some files come only from the
capture-edge partial-idle runs, which are not real bit cells).

Candidate rejection: 9600 baud would require a 1250-sample unit, but no
1250-sample runs exist; all runs are multiples of 2500. **4800 baud is the
unique fit.** — [CAP]

> Note on the earlier false-baud risk: a naive "shortest run" reading reports
> ~1480–2062 samples for some files, implying ~5800–8100 baud. Those short runs
> are the **leading partial-idle segment** at t=0 (capture started mid-idle),
> not a bit cell — a genuine UART bit can never be shorter than one bit time
> inside valid framing. The correct unit is the run-length **common divisor**
> (2500), confirmed by zero framing errors below.

## 4. UART framing validation (8N1, LSB-first, non-inverted) — [CAP]

A continuous UART decoder (start bit = falling edge, sample each of 10 bit
centres, LSB-first, idle-high so logic-1 = HIGH) was run over all five captures.
The decoder correctly resynchronises on back-to-back bytes.

**Result: 136 bytes decoded across all captures with 0 framing errors.**
Every frame has start bit = LOW and stop bit = HIGH at the predicted 2500-sample
cell centres. This independently confirms **8 data bits, no parity, 1 stop bit,
non-inverted** — the transport is conventional 8N1 at 4800 baud. — [CAP]

## 5. Decoded frames per capture — [CAP]

Packets are separated by ~33 ms idle gaps (≈160 bit-times). **Within a packet
the 8 bytes are strictly back-to-back:** measured byte start-to-start intervals
are **exactly 10.00 bit-times** for all 7 gaps in every complete packet (1 start
+ 8 data + 1 stop, with no inter-byte idle). A full frame is therefore 80
contiguous bit-times = **16.67 ms** of transmission, followed by ~33 ms idle.
Complete 8-byte packets found (partial packets at the capture edges are listed
separately and are consistent truncations of the same 8-byte frame):

| Capture  | Complete 8-byte packets (×3 each, all identical) | Edge partials |
|----------|--------------------------------------------------|---------------|
| Idle     | `66 80 80 80 00 00 80 99` | `99` (head), — |
| Forward  | `66 80 FF 80 00 00 FF 99` | `66 80 FF 80` (tail) |
| Backward | `66 80 00 80 00 00 00 99` | `66 80 00 80 00 00` (tail) |
| Left     | `66 59 80 80 00 00 59 99` | `66 59 80 80 00` (tail) |
| Right    | `66 A6 80 80 00 00 A6 99` | (none) |

### Repeated-frame consistency — [CAP]
Within each capture the three fully-captured packets are **byte-for-byte
identical**. The partials are exact prefixes/suffixes of the same frame. There
is no per-frame sequence counter, rolling code, or variation.

### Measured frame cadence (packet start → next packet start) — [CAP]
Interior (non-edge) intervals:

```
Forward : 50.00, 51.89, 48.01 ms
Idle    : 50.50, 49.53 ms
Left    : 50.09, 49.74 ms
Right   : 49.70, 50.07 ms
Backward: 57.91 ms (+ edge-affected 60.5 / 31.9)
```

**Cadence ≈ 50 ms (≈20 Hz) on the yellow wire.** This is explicitly *not* the
reference repo's 100 ms UDP interval (see §8). Firmware should use ~50 ms.

## 6. Packet field schema — [CAP]

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

Value ranges observed:
- Steering: centre `0x80` (128); left `0x59` (89, −39); right `0xA6` (166, +38).
  Full-scale steering (`0x00`/`0xFF`) was **not** observed — the source app used
  a reduced deflection. — [CAP]
- Throttle: neutral `0x80` (128); forward `0xFF` (+127); reverse `0x00` (−128).
  Throttle *does* use full scale. — [CAP]

## 7. Checksum verification — [CAP]

For all five commands, `byte6 == byte1 ^ byte2 ^ byte3`:

| Cmd | b1 | b2 | b3 | b1^b2^b3 | b6 | match |
|-----|----|----|----|----------|----|-------|
| Idle     | 80 | 80 | 80 | 80 | 80 | ✅ |
| Forward  | 80 | FF | 80 | FF | FF | ✅ |
| Backward | 80 | 00 | 80 | 00 | 00 | ✅ |
| Left     | 59 | 80 | 80 | 59 | 59 | ✅ |
| Right    | A6 | 80 | 80 | A6 | A6 | ✅ |

The proposed checksum formula is **confirmed**. (Because b3 is constant 0x80,
this is equivalently `steer ^ throttle ^ 0x80`.)

## 8. Cross-check against reference repo — [REF]

Source: <https://github.com/DEEFRAG/WL-Tech-FPV-CAR> (targets the WLtoys 6401),
file `xbox_gamepad_proxy_mediamtx_deadzone_browser.py`.

```python
BASE_PACKET = bytearray.fromhex("ca 47 d5 00 00 00 00 00 66 80 80 80 00 00 80 99")  # 16 bytes
byte_9  = map_axis_to_byte(x)      # steering  (X axis)
byte_10 = map_axis_to_byte(-y)     # throttle  (-Y axis)
packet[14] = packet[9] ^ packet[10] ^ packet[11]   # XOR checksum
# map_axis_to_byte(v) = int((v + 1) / 2 * 255)      -> [-1,1] -> [0,255]
# deadzone: |byte-0x80| <= 10  -> reset to 0x80
# target 172.16.11.1:23458 UDP ; SEND_INTERVAL = 0.1 s (100 ms)
```

Alignment of the repo's 16-byte UDP packet to our 8-byte yellow-wire packet
(repo indices 8–15 = our indices 0–7):

| repo idx | repo byte | our idx | our field | agrees? |
|---------:|-----------|--------:|-----------|---------|
| 8  | `0x66`        | 0 | start `0x66`          | ✅ |
| 9  | steering (X)  | 1 | steering             | ✅ |
| 10 | throttle (-Y) | 2 | throttle             | ✅ |
| 11 | `0x80`        | 3 | `0x80`               | ✅ |
| 12 | `0x00`        | 4 | `0x00`               | ✅ |
| 13 | `0x00`        | 5 | `0x00`               | ✅ |
| 14 | `p9^p10^p11`  | 6 | `b1^b2^b3`           | ✅ (same formula) |
| 15 | `0x99`        | 7 | end `0x99`           | ✅ |

Answers to the specific cross-check questions:

1. **Neutral UDP packet:** `ca 47 d5 00 00 00 00 00 66 80 80 80 00 00 80 99`. — [REF]
2. **Steering = byte 9, throttle = byte 10.** — [REF]
3. **Checksum = byte9 ^ byte10 ^ byte11** (XOR). — [REF] Identical to our
   capture-derived `b1^b2^b3`. — [CAP]+[REF]
4. **Repo send cadence = 100 ms UDP.** — [REF] But the **yellow wire runs at
   ~50 ms** — [CAP]. The camera/WiFi module does not forward 1:1 in time.
5. **Last 8 bytes match our decoded internal packets exactly**, byte-for-byte,
   including the checksum position and formula. — [CAP]+[REF]
6. **Axis mapping is consistent:** forward = stick up = `-y>0` → `0xFF`
   (our Forward b2=`0xFF`); reverse → `0x00` (our Backward). Steering left/right
   land on our `0x59`/`0xA6`, though the original app used a **narrower steering
   range** than the repo's full `0x00–0xFF`. — [CAP]+[REF]
7. **8-byte outer-prefix stripping (`ca 47 d5 00 00 00 00 00`):** the inner 8
   bytes match exactly, so the hypothesis that the WiFi/camera module consumes
   the 8-byte prefix and forwards the remaining 8 bytes onto the yellow wire is
   **strongly supported**. It is an inference — we captured the wire, not the
   module's firmware — so the prefix's exact role (routing header / magic /
   length) is not proven. — [INF]
8. **Proven vs inferred:** see §9/§10.

## 9. Proven facts

- 12 MHz sample rate; single active channel D0; idle HIGH. — [CAP]
- Transport is UART **4800 8N1, non-inverted, LSB-first**; 0 framing errors over
  136 bytes. — [CAP]
- Internal frame is **exactly 8 bytes**, repeated identically. — [CAP]
- Field schema and the five command packets in §6 are exactly as decoded. — [CAP]
- Checksum `b6 = b1 ^ b2 ^ b3`. — [CAP]
- Yellow-wire repeat cadence ≈ **50 ms**. — [CAP]
- Inner 8 bytes equal the last 8 bytes of the reference UDP packet, same
  checksum formula. — [CAP]+[REF]

## 10. Remaining uncertainties

- **Steering full scale.** Only `0x59`/`0xA6` (partial) were captured. `0x00`/
  `0xFF` for steering are the repo's choice and are **[ASM]** for this car until
  tested — they may over-travel or be clamped.
- **Combined commands** (e.g. forward-left in one packet) were never captured.
  The independent steer/throttle field model predicts they work by setting both
  bytes and recomputing the checksum, but this is **[INF]** until physically
  tested. **Recommended: capture four more `.sr` files from the original app —
  Forward+Left, Forward+Right, Reverse+Left, Reverse+Right** — and decode them
  with `tools/analyze_captures.py`. If each equals `66 STEER THR 80 00 00 CHK 99`
  with both fields set and `CHK = STEER^THR^0x80`, the combined-axis model is
  promoted from [INF] to [CAP] and the firmware can trust it without bench guessing.
- **Prefix semantics** `ca 47 d5 …` — [INF]/[ASM] (see §8.7).
- **No RX / handshake observed.** Whether the car ever expects a reply, or a
  keep-alive faster than 50 ms to avoid a failsafe cutout, is **[ASM]**. Only
  three repeats per 166 ms window were captured; long-term timeout behaviour is
  unknown.
  **Update — connection-establishment phase now captured [CAP].** Two
  full-length working-car captures (5.05 s / 3.45 s from capture start) show
  a silent released-HIGH period followed directly by periodic Idle packets
  (`66 80 80 80 00 00 80 99`, ~50 ms cadence) — there is no handshake and no
  distinct first packet; "initialization" is simply silence-then-Idle-traffic.
  This confirms the firmware's 5 s silent released-HIGH hold before UART
  start (`main.c`'s `SIGNAL_HOLD_HIGH_MS`) is harmless, but it is now known
  to be **unnecessary** — the car does not appear to key off the
  silence-to-traffic transition's specific duration.

## 11. Electrical uncertainties a digital logic capture cannot resolve

A logic analyzer records **digital timing only**. The following must be measured
before driving the wire and cannot be inferred from these captures:

- **Yellow-wire HIGH voltage — measured [CAP].** With the original module
  disconnected, the car's own board pulls the line to a steady **~2.81 V**,
  confirming the car itself sources the pull-up rather than expecting the
  driver to supply it. Firmware now drives GPIO4 **open-drain**
  (`GPIO_MODE_OUTPUT_OD`, IMPLEMENTATION_PLAN.md §3): it only pulls the line
  to GND for LOW/start bits and releases (Hi-Z) for HIGH/idle, so it never
  imposes a competing 3.3 V HIGH against the car's own ~2.81 V rail.
- **Drive direction / bus contention** — the open-drain change above removes
  ESP32-vs-car HIGH-level contention by construction (the ESP32 never drives
  HIGH), but the original module must still be physically disconnected so
  nothing else pulls the line LOW unexpectedly.
- **Common ground** requirement between ESP32-S3 and the car controller.
- **Input tolerance** — whether the car's receiver reliably reads the ESP32's
  GND-pull as a valid LOW, and whether ~2.81 V is a valid HIGH for an
  ESP32-S3 input if RX is ever wired up (it is not currently).
- Whether a **level shifter, transistor buffer, or series resistor** is needed.

These feed directly into the Electrical Integration Checkpoint of the
implementation plan.

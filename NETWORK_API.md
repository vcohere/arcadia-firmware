# WLtoys 6401 Network API

This is the wire contract for the networked firmware running on a **Seeed
Studio XIAO ESP32-S3 Sense**. A separate webapp uses it to show the camera and
drive the car. There is **no authentication** — the device is meant to live on
a trusted LAN.

## 1. Finding the device

The board joins the WiFi network configured in
[`main/config.h`](main/config.h) (`WIFI_SSID` / `WIFI_PASSWORD`) as a DHCP
client. On boot it logs its acquired address over USB-serial:

```
I (nnnn) wifi: connected, IP address: 192.168.1.42
```

Read that IP from the serial monitor (`idf.py -p <port> monitor`) or your
router's DHCP lease table. All endpoints below are relative to that address.

### mDNS discovery

The board also advertises itself over mDNS so controllers (e.g. the
`prototype-raspberry` relay) can find cars without hardcoded IPs:

- **Service**: `_arcadia-car._tcp`, port 80 (the control/API server).
- **Instance name / hostname**: `arcadia-car-<last-3-MAC-bytes-hex>` (e.g.
  `arcadia-car-a1b2c3`). This string is the car's stable **car_id** — relay
  allowlists, logs, and metrics key on it. It is derived from the STA MAC, so
  it survives reflashes.
- **TXT records**: `version` (wire-contract version, currently `1`) and
  `capture_port` (the snapshot server port, currently `81`).

Browse it with `dns-sd -B _arcadia-car._tcp` (macOS) or
`avahi-browse -r _arcadia-car._tcp` (Linux). See
[`main/mdns_adv.c`](main/mdns_adv.c).

## 2. Endpoint summary

| Purpose        | Method | URL                          | Notes |
|----------------|--------|------------------------------|-------|
| Test page      | GET    | `http://<ip>/`               | Built-in HTML UI (video + touch controls) |
| **Control**    | WS     | `ws://<ip>/control`          | Send JSON command frames |
| **Video**      | GET    | `http://<ip>:81/capture`     | Single JPEG; poll in a loop for video |
| Status         | GET    | `http://<ip>/status`         | JSON device state |

Control/API endpoints (`/`, `/control`, `/status`) are on **port 80**; the
camera snapshot (`/capture`) is on a **separate server on port 81** so a ~25 KB
blocking JPEG send never occupies the control server's single worker while a
WS control frame waits.

## 3. Control — WebSocket `ws://<ip>/control`

Open a WebSocket and send **text** frames. Each frame is one JSON object with
two discrete fields:

```json
{ "steer": "left|center|right", "throttle": "forward|neutral|reverse" }
```

- `steer` — `"left"`, `"center"`, or `"right"`.
- `throttle` — `"forward"`, `"neutral"`, or `"reverse"`.

These map 1:1 to the **captured & verified** protocol bytes:

| Field value          | Protocol byte |
|----------------------|---------------|
| `steer: left`        | `0x59` |
| `steer: center`      | `0x80` |
| `steer: right`       | `0xA6` |
| `throttle: forward`  | `0xFF` |
| `throttle: neutral`  | `0x80` |
| `throttle: reverse`  | `0x00` |

The values are **discrete on purpose**: the intermediate analog range was never
captured from the original app and is unverified for this car (see README §17).
Any unknown/missing string falls back to the neutral value, so a malformed
frame can never command motion. Frames larger than 256 bytes are ignored.

### Send cadence and failsafe

- Send a fresh command about every **100 ms (~10 Hz)** while a control is held.
- If the firmware receives **no command within 400 ms**
  (`CONTROL_FAILSAFE_MS`), it reverts to neutral (`steer:center`,
  `throttle:neutral`) on the very next 50 ms car frame. So on release, send one
  explicit neutral frame *and* simply stopping will both stop the car within
  ~400 ms.
- The firmware does not push anything back over the WebSocket; it only reads.
  Poll `/status` if you need to display current state.

```js
const ws = new WebSocket(`ws://${host}/control`);
let cmd = { steer: "center", throttle: "neutral" };
ws.onopen = () => setInterval(() => {
  if (ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify(cmd));
}, 100);            // ~10 Hz keep-alive; well inside the 400 ms failsafe

// e.g. on a "forward" button:
//   down -> cmd = { steer: "center", throttle: "forward" };
//   up   -> cmd = { steer: "center", throttle: "neutral" };
```

## 4. Video — `http://<ip>:81/capture`

Video is **client-pull**, not a push stream: each request to `/capture`
returns one `image/jpeg` frame (HTTP 500 if the camera failed to initialize).
Fetch it in a loop — fetch, render, fetch again — and you get a self-clocked
video feed:

```js
async function playLoop(imgEl, host) {
  while (true) {
    const res = await fetch(`http://${host}:81/capture?t=${Date.now()}`, { cache: "no-store" });
    if (res.ok) {
      const url = URL.createObjectURL(await res.blob());
      imgEl.src = url;
      URL.revokeObjectURL(imgEl.dataset.prevUrl || "");
      imgEl.dataset.prevUrl = url;
    } else {
      await new Promise((r) => setTimeout(r, 500));
    }
  }
}
```

Why pull instead of push: an MJPEG push stream produces frames at a fixed
rate regardless of what the WiFi link can deliver, so when throughput dips
TCP queues and retransmits instead of dropping — every queued byte becomes
display latency, and the backlog never drains because production never
slows. With client-pull, exactly one frame is ever in flight: latency is
bounded at roughly one RTT plus one frame transfer, and link congestion
degrades fps instead of building up lag.

Frame size and JPEG quality come from `main/config.h` (`CAM_FRAME_SIZE`,
`CAM_JPEG_QUALITY`); default is 640×480. There's no server-side fps cap — the
pull loop self-paces to whatever the link and camera can sustain. Only one or
two simultaneous callers are practical on this hardware. `/capture` sets
`Access-Control-Allow-Origin: *`.

## 5. Status — `http://<ip>/status`

Returns JSON device state:

```json
{
  "ip": "192.168.1.42",
  "steer": "center",
  "throttle": "neutral",
  "ws_active": false,
  "camera": true,
  "uptime_ms": 12345
}
```

- `steer` / `throttle` — the command currently being emitted to the car (after
  the failsafe is applied, so this reflects what the wheels are actually
  getting).
- `ws_active` — `true` if a control command arrived within the last 400 ms.
- `camera` — `true` if the camera initialized successfully.

```js
const s = await (await fetch(`http://${host}/status`)).json();
console.log(s.ip, s.ws_active, s.camera);
```

## 6. Quick manual test (no webapp)

- Open `http://<ip>/` for the built-in page (live video + touch/click D-pad).
- Or from a browser console / `wscat`:
  ```
  wscat -c ws://<ip>/control
  > {"steer":"left","throttle":"forward"}
  ```
  The serial log shows the packet change to `66 59 FF ...` and revert to
  `66 80 80 ...` within ~400 ms after you stop sending.

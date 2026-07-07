# Prompt for the Next.js webapp session

Copy everything in the fenced block below and paste it as your first message into a
Claude Code session opened in your empty/boilerplate Next.js project.

---

```
Build a single-page proof-of-concept that lets me drive an RC car and watch its
camera. This is a POC — establish the connection and make it drivable, nothing
more. No auth, no state management libraries, no backend, no extra pages, no
polish beyond what's described. Keep it to the home page.

## The device (already running on my LAN)

An ESP32-S3 on the car exposes these plain HTTP/WS endpoints. It has NO security.
Hardcode the IP as a single easy-to-edit constant at the top of the file:

    const CAR_IP = "192.168.1.41";

- Video (MJPEG):  http://192.168.1.41:81/stream
    Use it directly as an <img> src — it's motion-JPEG, the browser renders it
    with no JS decoding. It already sends Access-Control-Allow-Origin: *.
- Control (WebSocket text frames):  ws://192.168.1.41/control
    Send ONE JSON object per frame:
      { "steer": "left" | "center" | "right",
        "throttle": "forward" | "neutral" | "reverse" }
    Values are DISCRETE — only those exact strings. Anything else is treated as
    neutral by the firmware. The device never sends messages back; it only reads.

## Control behavior (important, this is the whole point)

- Open the WebSocket on mount. If it closes, auto-reconnect after ~1s.
- Maintain a current command derived from whatever inputs are held right now, then
  send it on a fixed ~100ms interval (about 10 Hz) as a keep-alive, as long as the
  socket is open. Do NOT only send on change — the firmware has a 400ms failsafe
  that snaps the car back to neutral if it stops hearing fresh commands, so the
  steady 100ms resend is what keeps a held direction alive.
- When nothing is held, the command is { steer: "center", throttle: "neutral" }.
  Also send one explicit neutral frame immediately on any release so stopping is
  instant, not up-to-400ms later.

## Inputs — support BOTH, simultaneously

Keyboard (hold to drive, release to stop):
- Throttle: ArrowUp OR W = forward; ArrowDown OR S = reverse; neither = neutral.
- Steer:    ArrowLeft OR A = left;  ArrowRight OR D = right;  neither = center.
- Multiple keys combine (e.g. holding W + A = forward + left). If both opposing
  keys are held, that axis is neutral/center.
- Use keydown/keyup to track the held set; call preventDefault on the arrow keys
  so the page doesn't scroll. Ignore key auto-repeat (track held state, don't
  re-trigger on repeat).

On-screen (mobile-game style, press-and-hold):
- Four large hold-buttons laid out like a D-pad: Forward (top), Reverse (bottom),
  Left, Right. Pressing and holding a button is equivalent to holding its key;
  releasing clears it. They combine the same way (Forward + Left works).
- Use pointer events (pointerdown/pointerup/pointerleave/pointercancel) so it
  works with touch and mouse. Call preventDefault to avoid text selection / the
  iOS long-press menu, and give the buttons touch-action: none.
- The buttons must be big and thumb-friendly — this will be used on a phone in
  landscape. Show a clear pressed/active state.

## Layout

- The camera stream fills most of the screen (full-bleed <img>, object-fit).
- The D-pad controls overlay on top of the video (absolutely positioned, e.g.
  throttle buttons on one side, steer on the other, like a mobile racing game),
  so I can see the video and drive at the same time.
- Small unobtrusive connection indicator (e.g. a dot: green = WS open,
  red = disconnected).

## Tech constraints

- Next.js App Router, a single client component ("use client") on the home page
  (app/page.tsx). Plain React hooks only. TypeScript is fine.
- Styling: whatever is already in the project (Tailwind if present, otherwise a
  <style jsx> block or inline styles). Don't add UI libraries.
- Run the dev server over http://localhost:3000 (the default) — do NOT use HTTPS,
  because the browser would block the car's plain http:// and ws:// as mixed
  content. Keep everything http/ws.
- When done, tell me to run `npm run dev` and open http://localhost:3000, and
  remind me the laptop must be on the same WiFi as the car
  (SSID @Wyne438_2.4G). If the video or controls don't connect, the first thing
  to check is that CAR_IP still matches the car's current DHCP address.
```

---

## Notes for you (not part of the prompt)

- The car's IP `192.168.1.41` is a DHCP lease and can change on reboot. If the
  webapp can't connect later, re-check the address from the firmware serial log
  (`connected, IP address: ...`) or reserve it in your router.
- Full endpoint reference, if the other session needs more detail, is in this
  repo's [`NETWORK_API.md`](NETWORK_API.md).

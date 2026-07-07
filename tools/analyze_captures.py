#!/usr/bin/env python3
"""
Reproducible raw analysis of PulseView/sigrok .sr captures for the
WLtoys 6401 yellow-wire signal.

Everything here is derived from the RAW waveform, not from a decoder overlay:
  * parse .sr container + metadata
  * auto-select the active channel and idle level
  * compute transition run-lengths and the fundamental time unit
  * validate the 4800-baud hypothesis against raw run lengths
  * continuous UART 8N1 decode (LSB-first) with correct back-to-back resync
  * validate start bit (low) and stop bit (high) for every frame
  * split into packets by long idle gaps
  * verify the proposed XOR checksum
  * measure packet-start-to-packet-start cadence

Standard library only.  Run:  python3 tools/analyze_captures.py
"""
import zipfile
import os
import glob
from collections import Counter

CAPTURE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def load_capture(path):
    with zipfile.ZipFile(path) as z:
        meta = z.read("metadata").decode()
        chunks = sorted((n for n in z.namelist() if n.startswith("logic-")),
                         key=lambda n: int(n.rsplit("-", 1)[1]))
        raw = b"".join(z.read(n) for n in chunks)
    sr, unitsize, probes = None, 1, 0
    for line in meta.splitlines():
        line = line.strip()
        if line.startswith("samplerate="):
            num, unit = line.split("=", 1)[1].split()
            sr = int(float(num) * {"Hz": 1, "kHz": 1e3, "MHz": 1e6}[unit])
        elif line.startswith("unitsize="):
            unitsize = int(line.split("=", 1)[1])
        elif line.startswith("total probes="):
            probes = int(line.split("=", 1)[1])
    return sr, unitsize, probes, raw


def channel(raw, bit):
    mask = 1 << bit
    return [(b & mask) >> bit for b in raw]


def pick_channel(raw, probes):
    best = None
    for bit in range(probes):
        mask = 1 << bit
        prev = None
        trans = 0
        for b in raw:
            v = b & mask
            if prev is not None and v != prev:
                trans += 1
            prev = v
        if best is None or trans > best[1]:
            best = (bit, trans)
    return best  # (bit, transitions)


def run_lengths(samples):
    runs = []
    prev, c = samples[0], 1
    for v in samples[1:]:
        if v == prev:
            c += 1
        else:
            runs.append((prev, c))
            prev, c = v, 1
    runs.append((prev, c))
    return runs


def decode_uart(samples, sr, baud, idle_high=True):
    """
    Continuous UART 8N1 decoder, LSB-first.
    Correctly handles back-to-back bytes (stop bit immediately followed by
    next start bit) as well as idle gaps between bytes.
    Returns list of dicts: start(sample), byte, start_ok, stop_ok.
    """
    spb = sr / baud
    n = len(samples)
    idle = 1 if idle_high else 0
    active = 1 - idle
    frames = []
    i = 0
    while i < n:
        # skip idle level until the falling edge into a start bit
        while i < n and samples[i] == idle:
            i += 1
        if i >= n:
            break
        e = i  # first 'active' sample = start bit edge
        # need e + 9.5*spb to exist for a complete frame
        if e + 9.5 * spb >= n:
            frames.append({"start": e, "incomplete": True})
            break

        def at(k):
            return samples[int(round(e + (k + 0.5) * spb))]

        start_bit = at(0)
        data = [at(1 + k) for k in range(8)]
        stop_bit = at(9)
        byte = 0
        for k, b in enumerate(data):
            bit_val = b if idle_high else (1 - b)
            byte |= (bit_val & 1) << k
        frames.append({
            "start": e, "byte": byte,
            "start_ok": (start_bit == active),
            "stop_ok": (stop_bit == idle),
            "incomplete": False,
        })
        # advance to the end of the stop bit; the loop's idle-skip will then
        # find the next start bit whether back-to-back or gapped
        i = int(round(e + 10 * spb))
    return frames


def split_packets(frames, sr, baud, gap_bits=40):
    spb = sr / baud
    packets, cur = [], []
    for idx, f in enumerate(frames):
        if f.get("incomplete"):
            continue
        if cur and (f["start"] - cur[-1]["start"]) > gap_bits * spb:
            packets.append(cur)
            cur = []
        cur.append(f)
    if cur:
        packets.append(cur)
    return packets


def analyze(path):
    name = os.path.basename(path)
    sr, unitsize, probes, raw = load_capture(path)
    print("=" * 72)
    print(f"FILE: {name}")
    print(f"  samplerate={sr} Hz  unitsize={unitsize}  probes={probes}  "
          f"samples={len(raw)}  duration={len(raw)/sr*1000:.3f} ms")

    bit, trans = pick_channel(raw, probes)
    s = channel(raw, bit)
    idle_level = s[0]
    print(f"  active channel D{bit} ({trans} transitions), "
          f"idle level={idle_level} ({'HIGH' if idle_level else 'LOW'})")

    # Fundamental unit from raw runs (exclude the huge inter-packet idles)
    runs = run_lengths(s)
    body = [ln for (_, ln) in runs if ln < 40000]
    unit = min(body)
    print(f"  {len(runs)} runs; shortest non-idle run = {unit} samples "
          f"= {unit/sr*1e6:.2f} us -> {sr/unit:.1f} baud")
    # how cleanly do all body runs divide by 2500 (=1 bit @4800)?
    spb_4800 = sr / 4800
    residuals = [abs(ln - round(ln / spb_4800) * spb_4800) for ln in body]
    print(f"  4800 baud: 1 bit = {spb_4800:.0f} samples; "
          f"max run residual vs integer bits = {max(residuals):.1f} samples "
          f"({max(residuals)/spb_4800*100:.2f}% of a bit)")

    frames = decode_uart(s, sr, 4800, idle_high=(idle_level == 1))
    complete = [f for f in frames if not f.get("incomplete")]
    bad = [f for f in complete if not (f["start_ok"] and f["stop_ok"])]
    inc = [f for f in frames if f.get("incomplete")]
    print(f"  decoded {len(complete)} bytes @4800 8N1; "
          f"framing errors={len(bad)}; incomplete-at-edge={len(inc)}")

    packets = split_packets(frames, sr, 4800)
    print(f"  {len(packets)} packets (idle-gap separated):")
    starts = []
    for pi, pkt in enumerate(packets):
        hexs = " ".join(f"{f['byte']:02X}" for f in pkt)
        ok = all(f["start_ok"] and f["stop_ok"] for f in pkt)
        t0 = pkt[0]["start"] / sr * 1000
        edge = (pkt[0]["start"] < 3 * spb_4800) or \
               (pkt[-1]["start"] + 10 * spb_4800 > len(s) - 3 * spb_4800)
        tag = "  [complete 8B]" if len(pkt) == 8 else f"  [{len(pkt)}B PARTIAL/edge]"
        # checksum check for 8-byte packets
        chk = ""
        if len(pkt) == 8:
            b = [f["byte"] for f in pkt]
            calc = b[1] ^ b[2] ^ b[3]
            chk = f"  chk[6]={b[6]:02X} calc(b1^b2^b3)={calc:02X} " \
                  f"{'OK' if calc == b[6] else 'MISMATCH'}"
        print(f"    pkt{pi} t={t0:8.3f}ms framing_ok={ok}: {hexs}{tag}{chk}")
        starts.append(pkt[0]["start"])
    # cadence between consecutive complete-ish packets
    if len(starts) >= 2:
        d = [(starts[i+1]-starts[i])/sr*1000 for i in range(len(starts)-1)]
        print(f"  packet start-to-start (ms): {[f'{x:.2f}' for x in d]}")


def main():
    for path in sorted(glob.glob(os.path.join(CAPTURE_DIR, "*.sr"))):
        analyze(path)


if __name__ == "__main__":
    main()

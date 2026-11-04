#!/usr/bin/env python3
"""End-to-end audio latency: feed 10 ms chunks into the pulse default sink in
real time with a click in some of them, then find the clicks in the WAV the
CLI host wrote (host log has 'wav sample 0 at monotonic X us').

  tests/audio-click.py <host.log> <wav> [clicks]     (run the stream first;
  this script prints the python-side click times to stdin of the second step)

Two modes: `play <out.json>` plays clicks and records their monotonic times,
`check <host.log> <wav> <in.json>` compares.
"""
import json, re, subprocess, sys, time, wave
import numpy as np

RATE = 48000

def play(out, n=20):
    p = subprocess.Popen(["pacat", "--raw", "--format=s16le", "--rate=48000",
                          "--channels=2", "--latency-msec=20",
                          "--device=streamsink"], stdin=subprocess.PIPE)
    chunk = RATE // 100
    silence = bytes(chunk * 4)
    click = (np.sin(2 * np.pi * 1000 * np.arange(chunk) / RATE) * 20000).astype("<i2")
    click = np.repeat(click, 2).tobytes()
    times = []
    t = time.monotonic() + 0.2
    for i in range(300 + n * 50):
        while time.monotonic() < t - 0.005:
            time.sleep(0.001)
        is_click = i >= 200 and (i - 200) % 50 == 0 and len(times) < n
        times.append(time.monotonic()) if is_click else None
        p.stdin.write(click if is_click else silence)
        p.stdin.flush()
        t += 0.01
    p.stdin.close(); p.wait()
    json.dump(times, open(out, "w"))

def check(log, wavp, inj):
    t0 = int(re.findall(r"sample 0 at monotonic (\d+) us", open(log).read())[-1]) / 1e6
    w = wave.open(wavp)
    x = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").reshape(-1, w.getnchannels())[:, 0]
    a = np.abs(x.astype(float))
    hits = []
    i = 0
    while i < len(a):
        if a[i] > 5000:
            hits.append(i); i += RATE // 10
        else:
            i += 1
    times = json.load(open(inj))
    lats = []
    for h in hits:
        tp = t0 + h / RATE
        # nearest click
        c = min(times, key=lambda c: abs(c - tp))
        lats.append((tp - c) * 1000)
    lats = np.array(lats)
    print("clicks played %d, found %d" % (len(times), len(hits)))
    if len(lats):
        print("latency (write into pulse -> played) ms: min %.1f median %.1f mean %.1f max %.1f" %
              (lats.min(), np.median(lats), lats.mean(), lats.max()))

if sys.argv[1] == "play":
    play(sys.argv[2])
else:
    check(sys.argv[2], sys.argv[3], sys.argv[4])

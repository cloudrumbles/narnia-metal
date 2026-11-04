# audio (branch `audio`)

Sound the games make on the box comes out of the iPad. Status: box daemon,
host core, jitter buffer, CLI test sink and iPad player are written; the box
-> relay -> CLI host path is tested end to end (below). The iPad player
(AudioUnit RemoteIO) compiles in CI but **has not run on a real iPad**.

## design

```
game (Proton/SDL/FMOD) -> pulse null sink "streamsink"
   -> nm-audio (box): pa_simple capture of streamsink.monitor, libopus
      encode (10 ms packets, 48 kHz stereo, 128 kbit/s VBR, restricted
      low delay)
   -> nm-relay unix socket (as if it were a guest) -> TCP -> host
   -> nm_host_serve(): first message NM_AUDIO_HELLO -> nm_audio_serve()
   -> audio sink: nm_audio_player (Opus decode + adaptive jitter buffer)
      -> iPad: AudioUnit RemoteIO | CLI: simulated device clock + WAV
```

- **Transport.** The daemon is a relay "guest", so the relay asks the iPad
  for a data connection exactly as for a Venus app and nothing in the relay
  changed (I only widened its line buffer to 512 for longer STAT lines).
  Audio gets its own TCP connection, so it isn't queued behind video bytes in
  the same stream, but it does share the link: when the video burst saturates
  the iPad's downlink, audio jitters too (the jitter buffer is there for
  that). UDP/QUIC would fix it properly, see "not done".
- **Protocol** (`common/nm_proto.h`): `NM_AUDIO_HELLO` (rate, channels,
  samples per packet, bitrate), `NM_AUDIO_HELLO_REPLY` (accepted or not),
  `NM_AUDIO_PACKET` (seq, capture timestamp in us on the box's monotonic
  clock, Opus payload). No retransmission; gaps in `seq` are concealed with
  Opus PLC (up to 3 packets). **`NM_VERSION` is not bumped**: the audio
  stream has its own `NM_AUDIO_VERSION` (1) and shares no message with Venus
  guests, so a bump would only force a Mesa rebuild (the guest takes the
  version from the same header). Nothing to reconcile at merge.
- **Daemon doesn't send until the host answers** (hello -> reply), so
  connecting before the iPad is up leaves no stale backlog; the relay holds
  the connection up to its `-w` seconds and the daemon retries every 2 s
  after any failure or when the stream ends. A short send queue (default 8
  packets) drops the oldest packet if the relay doesn't keep up, and the unix
  socket's send buffer is 32 KB.
- **Pulse.** `nm-audio` runs `pulseaudio --start --exit-idle-time=-1` if
  `pactl info` fails, creates null sink `streamsink` if missing and makes it
  the default (`-n` skips all that). Proton/FMOD/SDL games then find a sink
  with no configuration. Packages on the box: `pulseaudio pulseaudio-utils`
  (plus `libpulse-dev libopus-dev` to build). PipeWire-pulse would work the
  same way (same `pactl`/`pa_simple` API) but pulseaudio needs no systemd
  user session.
- **Host core.** `host/nm_audio.h` is the seam (like `nm_present.h`):
  `nm_audio_sink_ops {start(fmt), packet(seq, ts, data, len), stop, stats}`
  and `nm_host_set_audio_sink()`. A new stream replaces a running one (daemon
  restart). `nm_host_audio_stats()` is what the app puts in telemetry.
- **Player** (`host/nm_audio_player.[ch]`, portable C + libopus, shared by
  all sinks). A mutex-protected ring of decoded PCM (critical sections are a
  memcpy of one packet / one device buffer). Target depth = 25 ms + recent
  peak of arrival delay (arrival time minus capture timestamp, relative to the
  minimum over the last ~6 s, so the unshared clocks don't matter), starts at
  50 ms, +10 ms on every underrun, at most 250 ms. The device callback is
  resampled by at most +-0.5% (2% when far above target after a stall) to
  steer the depth to the target, which also absorbs the box/device clock
  difference. If depth exceeds target + max(target, 80 ms) the oldest audio is
  dropped. On underrun it outputs silence until the buffer refills to target.
- **iPad sink** (`ios/app/nm_audio_ios.m`): AVAudioSession category
  `playback` (sounds with the silent switch on), preferred IO buffer 5 ms,
  RemoteIO with 16-bit interleaved PCM at the stream's rate; restarts on
  interruption end and media-services reset. The unit starts with the stream
  and outputs silence until the buffer is primed. No `UIBackgroundModes`
  audio yet, so audio stops when the app is backgrounded (video does too).
- **libopus on iOS**: `ios/fetch-deps.sh` clones xiph/opus v1.5.2,
  `ios/build-opus.sh` compiles it (float, no SIMD, no config.h) with plain
  clang like `vkr/build-vkr.sh`. Verified with gcc on the box (137 objects, 3
  s, encode/decode round trip works); not yet with Apple clang.
- **Telemetry.** The STAT line gets `aud_buf_ms aud_target_ms aud_jitter_ms
  aud_pkts aud_lost aud_under aud_drop` while a stream is live; the on-screen
  stats show `audio <buffer> ms` (a `!` after an underrun). The audio stream
  is not counted in `streams` any more.

## running it

On the box (the Venus relay as before):

```bash
make -C box                        # builds nm-relay nm-input nm-audio
box/nm-audio -v                    # relay socket /tmp/narnia-metal.sock
box/nm-audio -s /tmp/other.sock -f 20 -b 96000    # other socket, 20 ms, 96k
box/nm-audio -t                    # 440 Hz test tone, no pulse needed
```

Start it after (or before) `nm-relay`; games just need the same user's pulse
server (`pactl info`; sink `streamsink` is the default). For a game that
picks its device itself, point it at `streamsink`.

CLI host (tests): `NM_AUDIO_WAV=/tmp/out.wav host/nm-host relay:<host>:<port>:<token>`
plays the stream against a simulated 10 ms device clock, logs buffer depth,
capture-to-playout latency and counters once a second (and a latency summary
at the end of the stream) and writes the WAV, finalised when the stream ends.
Needs libopus (`pkg-config opus`).

Tests in `tests/audio-click.py` (clicks into pulse, compared with where they
land in the WAV).

## results (dev box, loopback; relay 7402)

| test | result |
|---|---|
| 440 Hz tone, 65 s (`nm-audio -t`) | WAV peak at 440.0 Hz, rms 5667 (expected 5657), 0 dropouts at 5 ms resolution, 0 underruns, 0 lost |
| `paplay` 880 Hz via pulse null sink | 881.6 Hz in a 6 s window (resampler nudging), no dropouts |
| vkQuake 1.34.1 demo loop, 60 s (SDL2 -> pulse -> stream) | rms 0..10000 following the game, 0 underruns, 0 lost, capture->playout p50 28 ms p99 47 ms |
| clicks written into pulse, found in the WAV, no network delay | 20/20 found, **45-51 ms** median from write into pulse to playout (includes pulse's ~20 ms playback buffer, capture, encode, decode, 30 ms jitter buffer) |
| same, netem 20 ms +-10 ms (normal) on the relay port | 120 ms median: +20 ms network and a jitter buffer that grew to ~75 ms; 0 packets lost, 1 underruns (at start) |
| netem 30+-15 ms and 2% loss (TCP retransmit stalls) | 200 ms median, buffer 120-160 ms, no further underruns |
| host frozen 3 s (SIGSTOP), then released | burst arrives, stale audio dropped (20 skips), latency 219 -> 40 ms in ~12 s |
| daemon started before the host, host killed and restarted mid-stream | daemon waits, then streams; reconnects within ~2 s each time |
| encode time | 50-270 us per 10 ms packet (max) |

Budget: loopback total is ~45-50 ms from pulse to playout, plus the iPad's
output buffer (5 ms requested), plus one-way network delay and whatever the
jitter buffer adds for the link. Target of < 100 ms on top of RTT holds on a
clean link; with 30 ms of jitter the buffer sits at ~60 ms.

Known quirks:
- `pa_simple_get_latency` is useless on record streams (0, or hundreds of ms
  at start), so the capture timestamp is "read time minus one packet" and the
  source's own buffering isn't in the latency numbers of the host log; the
  click test is the honest end-to-end figure.
- netem with jitter reorders packets, and TCP then holds data back, so the
  jitter the player sees is worse than the nominal figure.

## not done / ideas

- Real iPad test (RemoteIO format, interruption handling, route changes).
- Audio over UDP/QUIC with FEC; TCP head-of-line blocking is the main jitter
  source over the Internet. The player already conceals gaps, so only the
  transport would change.
- Background audio (`UIBackgroundModes`), 44.1 kHz device rates are handled by
  the AudioUnit's converter but untested.
- Silence gate / DTX: silent frames are ~3-20 bytes, so it costs ~30 kbit/s of
  headers; fine for now.
- PipeWire: untested, should work through pipewire-pulse unchanged.

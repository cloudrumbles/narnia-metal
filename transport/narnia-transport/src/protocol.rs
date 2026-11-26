//! Wire-format types shared between server and client.
//!
//! Tiny by design: we aren't standardizing this protocol, just defining
//! enough framing for the server to ship encoded frames + the client to
//! reassemble them. Versioning is implicit (bump the protocol marker in
//! [`Hello`] when we change the wire format).

use bytes::{Buf, BufMut, Bytes, BytesMut};

/// Wire-format protocol marker. Bumped on any breaking change to the
/// datagram header or hello message format. Client MUST refuse to talk to
/// a server with a different version.
pub const PROTOCOL_VERSION: u32 = 0x_0002_0001;

pub fn server_understands_frame_lost(protocol_version: u32) -> bool {
    protocol_version >= 0x_0002_0001
}

/// Track identifier — fits in a single byte on the wire so each datagram
/// header stays small. We have two tracks today (video, audio); plenty
/// of room to grow.
#[derive(Copy, Clone, Debug, PartialEq, Eq, Hash)]
#[repr(u8)]
pub enum TrackId {
    Video = 0,
    Audio = 1,
}

impl TrackId {
    pub fn from_u8(byte: u8) -> Option<Self> {
        match byte {
            0 => Some(TrackId::Video),
            1 => Some(TrackId::Audio),
            _ => None,
        }
    }
}

/// NTP-style 4-timestamp clock-sync exchange over the control bidi
/// stream. Sent by client RIGHT AFTER reading the server's Hello,
/// server stamps recv + send timestamps and echoes back. Lets the
/// client compute clock skew (server vs client wall clock) and one-way
/// transit time as separate quantities — quinn's smoothed RTT at
/// Hello receipt overcounts because of TLS handshake overhead, which
/// makes the simpler offset-only approach dangerous.
///
/// Wire format (ASCII, both messages newline-terminated):
///   client → server: `pixelping t1=US\n`
///   server → client: `pixelpong t1=US t2=US t3=US\n`
///
/// Compute on receipt of pong (with t4 stamped at recv):
///   skew_us    = ((t2 - t1) + (t3 - t4)) / 2     (server clock - client clock)
///   transit_us = ((t4 - t1) - (t3 - t2)) / 2     (one-way wire time)
///
/// Both assume the server's stamping interval (t3 - t2) is small
/// relative to network transit, which it is — we stamp immediately on
/// recv and right before send.
#[derive(Copy, Clone, Debug)]
pub struct PingMsg {
    pub t1_us: u64,
}

impl PingMsg {
    pub fn to_line(&self) -> String {
        format!("pixelping t1={}\n", self.t1_us)
    }

    pub fn parse(line: &str) -> Option<Self> {
        let line = line.trim();
        let mut parts = line.split_whitespace();
        if parts.next()? != "pixelping" {
            return None;
        }
        let mut t1_us = 0u64;
        for kv in parts {
            let (k, v) = kv.split_once('=')?;
            if k == "t1" {
                t1_us = v.parse().ok()?;
            }
        }
        Some(Self { t1_us })
    }
}

#[derive(Copy, Clone, Debug)]
pub struct PongMsg {
    pub t1_us: u64,
    pub t2_us: u64,
    pub t3_us: u64,
}

impl PongMsg {
    pub fn to_line(&self) -> String {
        format!("pixelpong t1={} t2={} t3={}\n", self.t1_us, self.t2_us, self.t3_us)
    }

    pub fn parse(line: &str) -> Option<Self> {
        let line = line.trim();
        let mut parts = line.split_whitespace();
        if parts.next()? != "pixelpong" {
            return None;
        }
        let mut t1_us = 0u64;
        let mut t2_us = 0u64;
        let mut t3_us = 0u64;
        for kv in parts {
            let (k, v) = kv.split_once('=')?;
            match k {
                "t1" => t1_us = v.parse().ok()?,
                "t2" => t2_us = v.parse().ok()?,
                "t3" => t3_us = v.parse().ok()?,
                _ => {}
            }
        }
        Some(Self { t1_us, t2_us, t3_us })
    }
}

/// Server → client controller rumble, on the control stream. The game
/// rumbles its (virtual Xbox) pad and the client plays it on the real
/// controller. Each line is the whole new state, not a pulse: it holds
/// until the next line, and all zeros means stop. Magnitudes are SDL's
/// 0..=65535: `low`/`high` are the low- and high-frequency (strong /
/// weak) motors, `lt`/`rt` the Xbox One trigger motors.
///
/// Wire format: `rumble low=L high=H lt=A rt=B\n`
#[derive(Copy, Clone, Debug, Default, PartialEq, Eq)]
pub struct RumbleMsg {
    pub low: u16,
    pub high: u16,
    pub lt: u16,
    pub rt: u16,
}

impl RumbleMsg {
    pub fn to_line(&self) -> String {
        format!("rumble low={} high={} lt={} rt={}\n", self.low, self.high, self.lt, self.rt)
    }

    pub fn parse(line: &str) -> Option<Self> {
        let mut parts = line.split_whitespace();
        if parts.next()? != "rumble" {
            return None;
        }
        let mut m = Self::default();
        for kv in parts {
            let (k, v) = kv.split_once('=')?;
            let v: u16 = v.parse().ok()?;
            match k {
                "low" => m.low = v,
                "high" => m.high = v,
                "lt" => m.lt = v,
                "rt" => m.rt = v,
                _ => {}
            }
        }
        Some(m)
    }

    pub fn is_stop(&self) -> bool {
        *self == Self::default()
    }
}

/// Block-id-aware deficit signal from client → server (or relay) on
/// the bidi control stream. Carries enough information for the
/// recipient to fulfill on-demand repair WITHOUT guessing: the exact
/// block that needs help, how many symbols the client already has,
/// and how many it needs to decode.
///
/// Wire format (ASCII line, newline-terminated):
///   `deficit track=T block=B k_received=R k=K\n`
///
/// Fields:
/// - `track`: 0=video, 1=audio (matches [`TrackId`] u8 repr)
/// - `block`: block id the client is missing
/// - `k_received`: source-equivalent symbols already received (so the
///   recipient knows how many MORE are needed)
/// - `k`: total source symbols required to decode this block
///
/// Only the structured form is accepted. Malformed lines (including a
/// bare `deficit\n`) parse to `None`.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub struct DeficitMsg {
    pub track: TrackId,
    pub block_id: u32,
    pub k_received: u32,
    pub k_total: u32,
}

impl DeficitMsg {
    pub fn to_line(&self) -> String {
        format!(
            "deficit track={} block={} k_received={} k={}\n",
            self.track as u8, self.block_id, self.k_received, self.k_total,
        )
    }

    /// Parse a structured deficit line. Returns `None` on the bare
    /// `deficit` form or on malformed lines.
    pub fn parse(line: &str) -> Option<Self> {
        let line = line.trim();
        let mut parts = line.split_whitespace();
        if parts.next()? != "deficit" {
            return None;
        }
        let mut track: Option<TrackId> = None;
        let mut block_id: Option<u32> = None;
        let mut k_received: Option<u32> = None;
        let mut k_total: Option<u32> = None;
        for kv in parts {
            let (k, v) = kv.split_once('=')?;
            match k {
                "track" => track = v.parse::<u8>().ok().and_then(TrackId::from_u8),
                "block" => block_id = v.parse().ok(),
                "k_received" => k_received = v.parse().ok(),
                "k" => k_total = v.parse().ok(),
                _ => {} // forward-compatible
            }
        }
        Some(Self {
            track: track?,
            block_id: block_id?,
            k_received: k_received?,
            k_total: k_total?,
        })
    }

    /// How many additional source-equivalent symbols the client needs
    /// to decode. The repairer should emit at least this many fresh
    /// repair symbols (typically more, scaled by expected loss rate).
    pub fn missing(&self) -> u32 {
        self.k_total.saturating_sub(self.k_received)
    }
}

/// Header on every media datagram. Fixed 17 bytes so the RaptorQ payload
/// gets the maximum possible body within the QUIC datagram MTU (~1200 b
/// over wireguard, leaving ~1180 b for symbols).
///
/// Wire layout:
/// ```text
/// |  0  | 1 .. 4 | 5 .. 8 | 9 .. 12 | 13 .. 16 |
/// |track|  esi   | block  |    k    |   size    |
/// ```
///
/// - `track`: see [`TrackId`].
/// - `esi`: encoding symbol id, the RaptorQ identifier for this symbol.
/// - `block`: 32-bit source block sequence number, increasing per track.
/// - `k`: number of source symbols required to decode this block. The
///   client sizes its decoder buffer from this on the first received
///   symbol of a new block.
/// - `size`: source object byte length (the encoded frame size before FEC
///   expansion). Needed by the RaptorQ decoder to know when it has enough.
#[derive(Copy, Clone, Debug)]
pub struct SymbolHeader {
    pub track: TrackId,
    pub esi: u32,
    pub block: u32,
    pub k: u32,
    pub size: u32,
}

impl SymbolHeader {
    pub const WIRE_LEN: usize = 17;

    pub fn encode(self, out: &mut BytesMut) {
        out.put_u8(self.track as u8);
        out.put_u32(self.esi);
        out.put_u32(self.block);
        out.put_u32(self.k);
        out.put_u32(self.size);
    }

    pub fn decode(buf: &mut Bytes) -> Option<Self> {
        if buf.remaining() < Self::WIRE_LEN {
            return None;
        }
        let track_byte = buf.get_u8();
        let esi = buf.get_u32();
        let block = buf.get_u32();
        let k = buf.get_u32();
        let size = buf.get_u32();
        Some(Self {
            track: TrackId::from_u8(track_byte)?,
            esi,
            block,
            k,
            size,
        })
    }
}

/// Server → client first message on the control stream. Lets the client
/// confirm protocol compatibility and learn about the track set before
/// receiving any datagrams. Sent ASCII-line-terminated for easy debugging
/// (trailing `\n`); body is just the protocol version + a few flags.
#[derive(Clone, Debug)]
pub struct Hello {
    pub protocol_version: u32,
    /// Sequence sent by server identifying this stream (so reconnects can
    /// distinguish an old session being replayed from a new one).
    pub session_id: u64,
    /// Resolution the server is encoding at. The client uses this to size
    /// decoder buffers and the present surface.
    pub width: u32,
    pub height: u32,
    /// Target framerate; informational, used by client for buffering hints.
    pub fps: f32,
    /// Server's wall-clock microseconds at the moment the Hello was
    /// constructed (SystemTime::UNIX_EPOCH delta). Optional — older
    /// servers won't send it. Used by the client to compute a clock
    /// offset against its own wall clock so the per-frame capture_us
    /// embedded in the source-object prefix is comparable to the
    /// client's decoded_us. Cross-host glass-to-glass measurement
    /// becomes honest (modulo Hello transit time, ~rtt/2 of one direction).
    pub server_anchor_us: Option<u64>,
    /// What the session carries instead of the default video track, e.g.
    /// `gl` for a glstream (OpenGL commands replayed by the client; see
    /// tools/glstream). None = video. Older clients ignore the key.
    pub mode: Option<String>,
}

impl Hello {
    /// Format: `pixelflux/PROTO_VERSION session=ID size=WxH fps=FPS [anchor=US]\n`
    pub fn to_line(&self) -> String {
        let mut s = format!(
            "pixelflux/{:08x} session={} size={}x{} fps={}",
            self.protocol_version, self.session_id, self.width, self.height, self.fps
        );
        if let Some(a) = self.server_anchor_us {
            s.push_str(&format!(" anchor={}", a));
        }
        if let Some(m) = &self.mode {
            s.push_str(&format!(" mode={}", m));
        }
        s.push('\n');
        s
    }

    pub fn parse(line: &str) -> Option<Self> {
        // Tolerate trailing newline / whitespace.
        let line = line.trim();
        let mut parts = line.split_whitespace();
        let proto_part = parts.next()?;
        let version_hex = proto_part.strip_prefix("pixelflux/")?;
        let protocol_version = u32::from_str_radix(version_hex, 16).ok()?;
        let mut session_id = 0u64;
        let mut width = 0u32;
        let mut height = 0u32;
        let mut fps = 0.0f32;
        let mut server_anchor_us: Option<u64> = None;
        let mut mode: Option<String> = None;
        for kv in parts {
            let (k, v) = kv.split_once('=')?;
            match k {
                "session" => session_id = v.parse().ok()?,
                "size" => {
                    let (w, h) = v.split_once('x')?;
                    width = w.parse().ok()?;
                    height = h.parse().ok()?;
                }
                "fps" => fps = v.parse().ok()?,
                "anchor" => server_anchor_us = v.parse().ok(),
                "mode" => mode = Some(v.to_string()),
                _ => {} // forward-compatible: ignore unknown keys
            }
        }
        Some(Self {
            protocol_version,
            session_id,
            width,
            height,
            fps,
            server_anchor_us,
            mode,
        })
    }
}

/// Client → server input events. Encoded over a reliable QUIC uni stream
/// because:
///   - events are small (~20 bytes); throughput is irrelevant
///   - dropping a key release would leave the server holding a stuck key,
///     which is much worse than the 1 RTT retransmit cost on loss
///   - ordering matters (a press must arrive before its corresponding
///     release)
///
/// Wire layout: 1-byte tag + fixed-size body per variant. Total size
/// 9-26 bytes depending on variant.
#[derive(Clone, Debug, PartialEq)]
pub enum InputWireEvent {
    /// Key press / release. `scancode` is the platform-independent
    /// physical key id (winit's `PhysicalKey::Code` mapped to evdev codes
    /// on the server side).
    Keyboard { scancode: u32, pressed: bool, repeat: bool },
    /// Pointer at an absolute position inside the client window. Server
    /// scales to the compositor's coordinate space using the reported
    /// `width` / `height`.
    PointerAbsolute { x: f64, y: f64, width: u32, height: u32 },
    /// Relative pointer motion (locked-pointer / FPS-style).
    PointerRelative { dx: f64, dy: f64 },
    /// Mouse button. `button` follows the linux evdev BTN_* numbering
    /// (1=left, 2=right, 3=middle, 4=back, 5=forward).
    MouseButton { button: u16, pressed: bool },
    /// Wheel scroll. `dx` / `dy` are in line-units (winit's LineDelta) or
    /// pixels (PixelDelta) — sign convention matches winit's.
    MouseScroll { dx: f64, dy: f64 },
    /// Client-side request that the server force the next encoded frame
    /// to be an IDR. Sent when the client's playout buffer detects loss
    /// (`is_skipping_until_idr` flips true) so recovery doesn't wait
    /// for the slow intra-refresh wave (~1 s @ default cadence). The
    /// server forwards this to `x11_capture::PipelineControl::request_idr`,
    /// which trips NVENC into emitting an IDR on its next encode tick.
    /// No payload — the request is idempotent.
    RequestIdr,
    /// Whole-pad snapshot from a virtual gamepad (the web client's
    /// on-screen controls). Sent on every change plus a heartbeat, so a
    /// lost datagram is repaired by the next one. `buttons` bit i is
    /// SDL_GamepadButton i (south, east, west, north, back, guide, start,
    /// l3, r3, lb, rb, up, down, left, right); `axes` follow
    /// SDL_GamepadAxis (lx, ly, rx, ry, lt, rt), sticks +y down, triggers
    /// -32768 at rest.
    Gamepad { buttons: u32, axes: [i16; 6] },
    FrameLost { block: u32 },
}

impl InputWireEvent {
    /// Largest possible wire size, used to size the receive buffer.
    pub const MAX_WIRE_LEN: usize = 1 + 8 + 8 + 4 + 4; // PointerAbsolute

    pub fn encode(&self, out: &mut bytes::BytesMut) {
        use bytes::BufMut;
        match *self {
            Self::Keyboard { scancode, pressed, repeat } => {
                out.put_u8(0);
                out.put_u32(scancode);
                out.put_u8(pressed as u8);
                out.put_u8(repeat as u8);
            }
            Self::PointerAbsolute { x, y, width, height } => {
                out.put_u8(1);
                out.put_f64(x);
                out.put_f64(y);
                out.put_u32(width);
                out.put_u32(height);
            }
            Self::PointerRelative { dx, dy } => {
                out.put_u8(2);
                out.put_f64(dx);
                out.put_f64(dy);
            }
            Self::MouseButton { button, pressed } => {
                out.put_u8(3);
                out.put_u16(button);
                out.put_u8(pressed as u8);
            }
            Self::MouseScroll { dx, dy } => {
                out.put_u8(4);
                out.put_f64(dx);
                out.put_f64(dy);
            }
            Self::RequestIdr => {
                out.put_u8(5);
            }
            Self::Gamepad { buttons, axes } => {
                out.put_u8(6);
                out.put_u32(buttons);
                for a in axes {
                    out.put_i16(a);
                }
            }
            Self::FrameLost { block } => {
                out.put_u8(7);
                out.put_u32(block);
            }
        }
    }

    /// Read one event from `buf`. Returns `None` if `buf` is missing
    /// bytes (caller should keep accumulating); returns `Err` if the tag
    /// is unrecognized (caller should treat the stream as corrupt and
    /// drop the connection).
    pub fn decode(buf: &mut bytes::Bytes) -> Result<Option<Self>, &'static str> {
        use bytes::Buf;
        if buf.is_empty() {
            return Ok(None);
        }
        // We need the tag first to know the body size, but we don't
        // commit to advancing `buf` until we know we have enough for the
        // whole event. Save the original cursor and restore on short read.
        let saved = buf.clone();
        let tag = buf.get_u8();
        let event = match tag {
            0 => {
                if buf.remaining() < 6 {
                    *buf = saved;
                    return Ok(None);
                }
                let scancode = buf.get_u32();
                let pressed = buf.get_u8() != 0;
                let repeat = buf.get_u8() != 0;
                Self::Keyboard { scancode, pressed, repeat }
            }
            1 => {
                if buf.remaining() < 24 {
                    *buf = saved;
                    return Ok(None);
                }
                let x = buf.get_f64();
                let y = buf.get_f64();
                let width = buf.get_u32();
                let height = buf.get_u32();
                Self::PointerAbsolute { x, y, width, height }
            }
            2 => {
                if buf.remaining() < 16 {
                    *buf = saved;
                    return Ok(None);
                }
                let dx = buf.get_f64();
                let dy = buf.get_f64();
                Self::PointerRelative { dx, dy }
            }
            3 => {
                if buf.remaining() < 3 {
                    *buf = saved;
                    return Ok(None);
                }
                let button = buf.get_u16();
                let pressed = buf.get_u8() != 0;
                Self::MouseButton { button, pressed }
            }
            4 => {
                if buf.remaining() < 16 {
                    *buf = saved;
                    return Ok(None);
                }
                let dx = buf.get_f64();
                let dy = buf.get_f64();
                Self::MouseScroll { dx, dy }
            }
            5 => Self::RequestIdr,
            6 => {
                if buf.remaining() < 16 {
                    *buf = saved;
                    return Ok(None);
                }
                let buttons = buf.get_u32();
                let mut axes = [0i16; 6];
                for a in &mut axes {
                    *a = buf.get_i16();
                }
                Self::Gamepad { buttons, axes }
            }
            7 => {
                if buf.remaining() < 4 {
                    *buf = saved;
                    return Ok(None);
                }
                Self::FrameLost { block: buf.get_u32() }
            }
            _ => return Err("unknown InputWireEvent tag"),
        };
        Ok(Some(event))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn symbol_header_roundtrip() {
        let h = SymbolHeader {
            track: TrackId::Video,
            esi: 65_535,
            block: 1234,
            k: 12,
            size: 16384,
        };
        let mut buf = BytesMut::with_capacity(SymbolHeader::WIRE_LEN);
        h.encode(&mut buf);
        assert_eq!(buf.len(), SymbolHeader::WIRE_LEN);
        let mut frozen = buf.freeze();
        let got = SymbolHeader::decode(&mut frozen).unwrap();
        assert_eq!(got.esi, 65_535);
        assert_eq!(got.block, 1234);
        assert_eq!(got.k, 12);
        assert_eq!(got.size, 16384);
        assert_eq!(got.track, TrackId::Video);
    }

    #[test]
    fn hello_roundtrip() {
        let h = Hello {
            protocol_version: PROTOCOL_VERSION,
            session_id: 0xdeadbeef_cafebabe,
            width: 1920,
            height: 1080,
            fps: 60.0,
            server_anchor_us: Some(1234567890),
            mode: Some("gl".into()),
        };
        let line = h.to_line();
        let parsed = Hello::parse(&line).unwrap();
        assert_eq!(parsed.protocol_version, h.protocol_version);
        assert_eq!(parsed.session_id, h.session_id);
        assert_eq!(parsed.width, h.width);
        assert_eq!(parsed.height, h.height);
        assert_eq!(parsed.fps, h.fps);
        assert_eq!(parsed.mode.as_deref(), Some("gl"));
    }

    #[test]
    fn rumble_roundtrip() {
        let m = RumbleMsg { low: 65535, high: 1, lt: 0, rt: 300 };
        assert_eq!(m.to_line(), "rumble low=65535 high=1 lt=0 rt=300\n");
        assert_eq!(RumbleMsg::parse(&m.to_line()), Some(m));
        assert!(RumbleMsg::parse("rumble low=70000").is_none());
        assert!(RumbleMsg::parse("deficit track=0").is_none());
        assert!(RumbleMsg::default().is_stop());
    }

    #[test]
    fn input_wire_event_roundtrip() {
        let cases = vec![
            InputWireEvent::Keyboard { scancode: 0x10001, pressed: true, repeat: false },
            InputWireEvent::PointerAbsolute { x: 123.5, y: 456.25, width: 1920, height: 1080 },
            InputWireEvent::PointerRelative { dx: -2.5, dy: 0.125 },
            InputWireEvent::MouseButton { button: 2, pressed: true },
            InputWireEvent::MouseScroll { dx: 0.0, dy: -3.0 },
            InputWireEvent::Gamepad { buttons: 0x4801, axes: [-32768, 32767, 0, -1, -32768, 32767] },
        ];
        for ev in cases {
            let mut buf = bytes::BytesMut::new();
            ev.encode(&mut buf);
            let mut frozen = buf.freeze();
            let decoded = InputWireEvent::decode(&mut frozen).unwrap().unwrap();
            assert_eq!(decoded, ev);
            assert!(frozen.is_empty(), "decode should consume exactly one event");
        }
    }

    #[test]
    fn input_wire_event_short_read_returns_none_without_consuming() {
        // Half a Keyboard event (tag + 3 of 6 body bytes).
        let mut frozen = bytes::Bytes::from(vec![0u8, 0, 0, 0]);
        let before = frozen.clone();
        let result = InputWireEvent::decode(&mut frozen).unwrap();
        assert!(result.is_none());
        assert_eq!(frozen, before, "short read must leave buf untouched");
    }

    #[test]
    fn only_servers_from_this_version_on_are_sent_frame_lost() {
        assert!(!server_understands_frame_lost(0x0002_0000));
        assert!(server_understands_frame_lost(0x0002_0001));
        assert!(server_understands_frame_lost(PROTOCOL_VERSION));
    }

    #[test]
    fn frame_lost_round_trips_and_a_truncated_one_waits_for_more() {
        let ev = InputWireEvent::FrameLost { block: 0xdead_beef };
        let mut buf = bytes::BytesMut::new();
        ev.encode(&mut buf);
        assert!(buf.len() <= InputWireEvent::MAX_WIRE_LEN);
        let mut whole = buf.clone().freeze();
        assert_eq!(InputWireEvent::decode(&mut whole).unwrap().unwrap(), ev);
        let mut short = bytes::Bytes::copy_from_slice(&buf[..buf.len() - 1]);
        let before = short.clone();
        assert!(InputWireEvent::decode(&mut short).unwrap().is_none());
        assert_eq!(short, before);
    }

    #[test]
    fn input_wire_event_unknown_tag_errors() {
        let mut frozen = bytes::Bytes::from(vec![99u8]);
        assert!(InputWireEvent::decode(&mut frozen).is_err());
    }

    #[test]
    fn deficit_msg_roundtrip() {
        let n = DeficitMsg {
            track: TrackId::Video,
            block_id: 1234,
            k_received: 25,
            k_total: 50,
        };
        let line = n.to_line();
        assert!(line.starts_with("deficit "));
        assert!(line.ends_with('\n'));
        let parsed = DeficitMsg::parse(&line).unwrap();
        assert_eq!(parsed, n);
        assert_eq!(parsed.missing(), 25);
    }

    #[test]
    fn deficit_msg_bare_form_returns_none() {
        // Bare `deficit\n` without block info is rejected — only the
        // structured form is accepted now.
        assert!(DeficitMsg::parse("deficit").is_none());
        assert!(DeficitMsg::parse("deficit\n").is_none());
    }

    #[test]
    fn deficit_msg_tolerates_extra_keys() {
        // Future protocol additions shouldn't break parsing.
        let parsed = DeficitMsg::parse(
            "deficit track=0 block=7 k_received=3 k=10 future_field=hello\n",
        )
        .unwrap();
        assert_eq!(parsed.block_id, 7);
        assert_eq!(parsed.missing(), 7);
    }

    #[test]
    fn hello_tolerates_unknown_keys() {
        // Forward-compatible: a future server might add fields we don't
        // know about. Should still parse the ones we recognize.
        let parsed = Hello::parse(
            "pixelflux/00010000 session=42 size=1920x1080 fps=60 future_field=hello\n",
        )
        .unwrap();
        assert_eq!(parsed.session_id, 42);
    }
}

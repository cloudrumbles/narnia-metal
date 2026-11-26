//! Publishing side of the transport.
//!
//! ## Lifecycle
//!
//! 1. Construct a [`Server`] with a bind address. This generates a fresh
//!    self-signed cert and listens for QUIC + WebTransport connections.
//! 2. Call [`Server::accept`] in a loop. Each call returns a [`Session`]
//!    when a client connects + completes the WebTransport handshake.
//! 3. From a [`Session`], grab a [`Publisher`] for each track. The
//!    publisher's [`Publisher::send`] takes a byte slice (one encoded
//!    HEVC frame, or one opus packet), splits it into RaptorQ symbols,
//!    and emits one QUIC datagram per symbol.
//!
//! ## What's NOT here
//!
//! - flow control / backpressure: datagrams are fire-and-forget by design;
//!   QUIC's congestion controller drops packets when the link is saturated
//!   and our redundancy absorbs the loss.
//! - retransmit: forbidden in datagram mode (see crate docs).
//! - multi-client fanout: caller is expected to call `accept` in a loop
//!   and spawn a task per session if they want concurrent clients. The
//!   trade-off is encoder duplication if you have multiple subscribers;
//!   for cloud gaming v1 it's one client per server, so this is fine.

use std::net::SocketAddr;
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::Arc;

use anyhow::{Context, Result};
use bytes::BytesMut;
use web_transport_quinn::Session;

use crate::cert::{self, GeneratedCert};
use crate::fec::{BlockEncoder, DEFAULT_REDUNDANCY};
use crate::protocol::{Hello, InputWireEvent, SymbolHeader, TrackId, PROTOCOL_VERSION};
use crate::socket_tuning;

/// Read one line from a web-transport recv stream, mirroring
/// `client::read_line`. Used by the server to receive the
/// post-Hello clock-sync ping.
async fn read_line_from(recv: &mut web_transport_quinn::RecvStream) -> Result<String> {
    let mut buf = Vec::with_capacity(64);
    let mut byte = [0u8; 1];
    loop {
        let n = recv.read(&mut byte).await.context("reading control stream byte")?;
        let n = n.unwrap_or(0);
        if n == 0 {
            anyhow::bail!("client closed control stream early");
        }
        if byte[0] == b'\n' {
            break;
        }
        buf.push(byte[0]);
        // Generous: a glstream client's `have` line lists ~1000 blob hashes.
        if buf.len() > 256 << 10 {
            anyhow::bail!("control message exceeds 256 KiB (malformed)");
        }
    }
    Ok(String::from_utf8(buf).context("control message not utf-8")?)
}

/// Listening server. Owns the quinn endpoint + cert.
pub struct Server {
    inner: web_transport_quinn::Server,
    cert: GeneratedCert,
    cert_cache: Option<std::path::PathBuf>,
    next_session_id: AtomicU64,
    hello_mode: Option<String>,
}

use std::sync::atomic::AtomicU64;

/// Build the quinn server config for one cert. Pulled out of `bind` so
/// [`Server::rotate_cert_if_due`] can swap in a re-minted cert on the
/// live endpoint without rebinding the socket.
fn build_server_config(cert: &GeneratedCert) -> Result<quinn::ServerConfig> {
    let cert_der = rustls::pki_types::CertificateDer::from(cert.cert_der.clone());
    let key_der = rustls::pki_types::PrivateKeyDer::try_from(cert.key_der.clone())
        .map_err(|e| anyhow::anyhow!("loading key: {e}"))?;

    let mut tls_config =
        rustls::ServerConfig::builder_with_provider(
            rustls::crypto::ring::default_provider().into(),
        )
        .with_protocol_versions(&[&rustls::version::TLS13])
        .context("ServerConfig::with_protocol_versions")?
        .with_no_client_auth()
        .with_single_cert(vec![cert_der], key_der)
        .context("ServerConfig::with_single_cert")?;
    tls_config.alpn_protocols = vec![web_transport_quinn::ALPN.as_bytes().to_vec()];

    let quic_server_config: quinn::crypto::rustls::QuicServerConfig = tls_config
        .try_into()
        .map_err(|e| anyhow::anyhow!("QuicServerConfig: {e:?}"))?;
    let mut server_config = quinn::ServerConfig::with_crypto(Arc::new(quic_server_config));

    // BBR congestion control. Same rationale as before: noisy wifi
    // last-hop, BBR ignores loss as a CC signal which is correct when
    // FEC is hiding loss anyway. Both ends must speak BBR — co-deployed
    // across server + relay + clients in one push. (Browser clients run
    // Chrome's own CC; BBR on our send side is what matters for video.)
    let mut transport = quinn::TransportConfig::default();
    transport.congestion_controller_factory(Arc::new(quinn::congestion::BbrConfig::default()));
    server_config.transport_config(Arc::new(transport));
    Ok(server_config)
}

impl Server {
    /// Bind on `addr`, mint a fresh self-signed cert. SANs default to
    /// `localhost / 127.0.0.1 / ::1`; pass `subject_alt_names` to add
    /// tailnet hostnames so a tailscale-aware client can bind by name.
    pub fn bind(addr: SocketAddr, subject_alt_names: Vec<String>) -> Result<Self> {
        Self::bind_with_cert_cache(addr, subject_alt_names, None)
    }

    /// Like [`Server::bind`], but keeps the cert in `cert_cache` (if
    /// given) so its SHA-256 — which browser clients pin — survives
    /// process restarts. See [`crate::cert::load_or_mint`].
    pub fn bind_with_cert_cache(
        addr: SocketAddr,
        subject_alt_names: Vec<String>,
        cert_cache: Option<std::path::PathBuf>,
    ) -> Result<Self> {
        // rustls requires a process-wide crypto provider. ring is the
        // default but explicit install lets multiple bind() calls in the
        // same process not race.
        let _ = rustls::crypto::ring::default_provider().install_default();

        let cert = match &cert_cache {
            Some(dir) => cert::load_or_mint(dir, subject_alt_names).context("cert cache")?,
            None => cert::self_signed(subject_alt_names).context("self-signed cert")?,
        };

        // Bind the UDP socket ourselves so we can tune kernel buffers +
        // GSO/GRO BEFORE quinn takes ownership of the fd. The builder's
        // with_certificate() calls quinn::Endpoint::server(config, addr)
        // internally which binds the socket with no knobs exposed, so we
        // bypass the builder and call web_transport_quinn::Server::new
        // with a manually constructed quinn::Endpoint instead.
        let udp = socket_tuning::bind_tuned_socket(addr)
            .context("bind_tuned_socket for server")?;

        let server_config = build_server_config(&cert)?;

        let runtime = quinn::default_runtime()
            .ok_or_else(|| anyhow::anyhow!("no async runtime found for quinn"))?;
        let endpoint = quinn::Endpoint::new(
            quinn::EndpointConfig::default(),
            Some(server_config),
            udp,
            runtime,
        )
        .context("quinn::Endpoint::new")?;

        let inner = web_transport_quinn::Server::new(endpoint);

        Ok(Self {
            inner,
            cert,
            cert_cache,
            next_session_id: AtomicU64::new(1),
            hello_mode: None,
        })
    }

    /// Re-mint the cert if it is close to expiry and install it on the
    /// live endpoint. Existing connections keep their handshake; only new
    /// ones see the new cert (and the new hash). Hash-pinned browser certs
    /// can't live longer than 14 days, so a long-running server must call
    /// this — the accept loop does it before each accept. Returns true if
    /// the cert changed.
    pub fn rotate_cert_if_due(&mut self) -> Result<bool> {
        if !self.cert.needs_remint() {
            return Ok(false);
        }
        let sans = self.cert.subject_alt_names.clone();
        let fresh = match &self.cert_cache {
            Some(dir) => cert::load_or_mint(dir, sans)?,
            None => cert::self_signed(sans)?,
        };
        self.inner.set_server_config(Some(build_server_config(&fresh)?));
        tracing::info!(sha256 = %fresh.sha256_hex(), "rotated server cert");
        self.cert = fresh;
        Ok(true)
    }

    /// SHA-256 of the current cert (what browsers pin), lowercase hex.
    /// Announce a non-video session mode in every Hello (see
    /// [`Hello::mode`]), e.g. `gl` for narnia-glserver.
    pub fn set_hello_mode(&mut self, mode: Option<String>) {
        self.hello_mode = mode;
    }

    pub fn cert_sha256_hex(&self) -> String {
        self.cert.sha256_hex()
    }

    /// The current cert, for callers that want its hash or expiry.
    pub fn cert(&self) -> &GeneratedCert {
        &self.cert
    }

    /// SANs the server cert claims authority for. Useful for clients that
    /// want to pin the cert hash instead of using `--insecure`.
    pub fn cert_subject_alt_names(&self) -> &[String] {
        &self.cert.subject_alt_names
    }

    /// DER-encoded server cert bytes. A client can pin against the SHA-256
    /// hash of this for a tighter trust model than `--insecure`.
    pub fn cert_der(&self) -> &[u8] {
        &self.cert.cert_der
    }

    /// Local address the server is listening on. Useful for tests that bind
    /// on port 0 and need to know which port the OS assigned.
    pub fn local_addr(&self) -> Result<std::net::SocketAddr> {
        // web_transport_quinn::Server Derefs to quinn::Endpoint, which has
        // local_addr(). We reach it via that Deref.
        self.inner.local_addr().map_err(|e| anyhow::anyhow!("local_addr: {e}"))
    }

    /// Wait for the next incoming connection + WebTransport handshake.
    /// Returns once the session is fully established.
    pub async fn accept(
        &mut self,
        width: u32,
        height: u32,
        fps: f32,
    ) -> Result<SessionHandle> {
        // Wait for a client, but wake up to re-mint the cert when it gets
        // close to expiry — a server idling for two weeks would otherwise
        // hand browsers an expired cert. web_transport_quinn's accept()
        // keeps its in-flight handshakes in `self`, so dropping the future
        // on the timer branch loses nothing.
        let request = loop {
            if let Err(e) = self.rotate_cert_if_due() {
                tracing::warn!(error = ?e, "cert rotation failed; keeping current cert");
            }
            let until_remint = self
                .cert
                .remaining()
                .saturating_sub(cert::REMINT_BEFORE)
                .max(std::time::Duration::from_secs(60));
            tokio::select! {
                req = self.inner.accept() => {
                    break req.ok_or_else(|| anyhow::anyhow!("server endpoint closed"))?;
                }
                _ = tokio::time::sleep(until_remint) => continue,
            }
        };

        // Promote the WebTransport request to a Session. The client is
        // already past TLS at this point.
        let session = request.ok().await.context("WebTransport handshake")?;

        let session_id = self.next_session_id.fetch_add(1, Ordering::Relaxed);

        // Send the hello on the control bidi stream so the client knows
        // the protocol version + resolution before any datagrams arrive.
        // We also keep the recv half open after Hello to handle the
        // post-Hello clock-sync ping (see below).
        let (mut send, mut recv) = session
            .open_bi()
            .await
            .context("opening control stream")?;
        let hello = Hello {
            protocol_version: PROTOCOL_VERSION,
            session_id,
            width,
            height,
            fps,
            // Server-clock anchor sampled at Hello-construction time.
            // Client subtracts its own anchor at Hello-receive time to
            // get clock_offset = client - server (modulo Hello transit
            // time of ~rtt/2). Used downstream to translate per-frame
            // capture_us (server clock) into client clock so
            // glass-to-decoded latency is meaningful cross-host.
            server_anchor_us: Some(
                std::time::SystemTime::now()
                    .duration_since(std::time::SystemTime::UNIX_EPOCH)
                    .map(|d| d.as_micros() as u64)
                    .unwrap_or(0),
            ),
            mode: self.hello_mode.clone(),
        };
        let line = hello.to_line();
        // SendStream::write returns the number of bytes written; loop until
        // all are flushed.
        let mut buf = line.as_bytes();
        while !buf.is_empty() {
            let n = send.write(buf).await.context("write hello")?;
            buf = &buf[n..];
        }

        // NTP-style clock-sync exchange. Read the client's ping, stamp
        // server-recv (t2) immediately on first byte, parse t1, stamp
        // server-send (t3) right before flushing pong. Failure here is
        // non-fatal — older clients won't send a ping, so we time out
        // after 1s and continue without sync. The client falls back to
        // its quinn-rtt-based estimate in that case.
        let ping_result = tokio::time::timeout(
            std::time::Duration::from_millis(1000),
            read_line_from(&mut recv),
        )
        .await;
        let t2_us = std::time::SystemTime::now()
            .duration_since(std::time::SystemTime::UNIX_EPOCH)
            .map(|d| d.as_micros() as u64)
            .unwrap_or(0);
        if let Ok(Ok(line)) = ping_result {
            if let Some(ping) = crate::protocol::PingMsg::parse(&line) {
                let t3_us = std::time::SystemTime::now()
                    .duration_since(std::time::SystemTime::UNIX_EPOCH)
                    .map(|d| d.as_micros() as u64)
                    .unwrap_or(0);
                let pong = crate::protocol::PongMsg {
                    t1_us: ping.t1_us,
                    t2_us,
                    t3_us,
                };
                let pong_line = pong.to_line();
                let mut buf = pong_line.as_bytes();
                while !buf.is_empty() {
                    match send.write(buf).await {
                        Ok(n) => buf = &buf[n..],
                        Err(e) => {
                            tracing::warn!(error = ?e, "write pong failed");
                            break;
                        }
                    }
                }
                tracing::info!(
                    t1_us = ping.t1_us,
                    t2_us,
                    t3_us,
                    "clock-sync pong sent"
                );
            } else {
                tracing::warn!(line = %line, "received non-ping line on control stream");
            }
        } else {
            tracing::info!("no ping received from client (older client or timeout); skipping clock-sync");
        }

        // Spawn the control-stream reader task. After Hello+Pong, we
        // keep reading lines from the bidi recv half. The only line we
        // expect is a structured `deficit ...\n` from the client
        // signaling FEC loss for a specific block. Future protocol
        // additions can layer here.
        // Channel of structured DeficitMsg events, one per client-detected
        // block loss. The server (or relay) consumer drives on-demand
        // repair from this. Bounded(64) absorbs short bursts of distinct
        // losses.
        let (deficit_event_tx, deficit_event_rx) =
            tokio::sync::mpsc::channel::<crate::protocol::DeficitMsg>(64);
        // Lines the transport doesn't know go to the application
        // (SessionHandle::take_control_lines), e.g. glstream's `have`.
        let (other_line_tx, other_line_rx) = tokio::sync::mpsc::unbounded_channel::<String>();
        tokio::spawn(async move {
            loop {
                match read_line_from(&mut recv).await {
                    Ok(line) => {
                        let trimmed = line.trim();
                        if trimmed.is_empty() {
                            continue;
                        }
                        // Structured form is the only accepted shape.
                        // If parse returns Some, push the event so the
                        // repair path can emit targeted extra symbols.
                        if let Some(deficit) = crate::protocol::DeficitMsg::parse(trimmed) {
                            let _ = deficit_event_tx.try_send(deficit);
                            tracing::debug!(
                                block = deficit.block_id,
                                k = deficit.k_total,
                                k_received = deficit.k_received,
                                "client deficit (structured)"
                            );
                        } else {
                            let _ = other_line_tx.send(trimmed.to_string());
                        }
                    }
                    Err(_) => return, // stream closed
                }
            }
        });

        // Spawn the input datagram reader. Client sends each
        // InputWireEvent as one QUIC datagram (one UDP packet on the
        // wire). Datagrams skip quinn's stream scheduler / pacer,
        // which on the prior uni-stream path delayed tiny low-volume
        // input writes by 100-700ms end-to-end (verified 2026-05).
        let session = Arc::new(session);
        let (input_tx, input_rx) = tokio::sync::mpsc::unbounded_channel::<InputWireEvent>();
        let session_for_input = session.clone();
        tokio::spawn(async move {
            loop {
                let dgram = match session_for_input.read_datagram().await {
                    Ok(d) => d,
                    Err(e) => {
                        tracing::warn!(error = ?e, "input datagram read failed");
                        return;
                    }
                };
                let mut frozen = dgram;
                match InputWireEvent::decode(&mut frozen) {
                    Ok(Some(ev)) => {
                        if input_tx.send(ev).is_err() {
                            return; // receiver dropped
                        }
                    }
                    Ok(None) => {
                        // Partial — datagrams are atomic so this is
                        // a malformed input event. Drop and continue;
                        // not a session-killer.
                        tracing::warn!("input datagram too short, dropping");
                    }
                    Err(e) => {
                        tracing::warn!(error = e, "input datagram tag invalid, dropping");
                    }
                }
            }
        });

        Ok(SessionHandle {
            control_tx: Arc::new(tokio::sync::Mutex::new(send)),
            control_lines: Arc::new(tokio::sync::Mutex::new(Some(other_line_rx))),
            deficit_events: Arc::new(tokio::sync::Mutex::new(Some(deficit_event_rx))),
            inner: session,
            video_block: Arc::new(AtomicU32::new(0)),
            audio_block: Arc::new(AtomicU32::new(0)),
            redundancy: DEFAULT_REDUNDANCY,
            input_rx: Arc::new(tokio::sync::Mutex::new(Some(input_rx))),
            datagrams_queued: Arc::new(AtomicU64::new(0)),
        })
    }
}

/// One client's session. Hand out [`Publisher`]s per track.
#[derive(Clone)]
pub struct SessionHandle {
    inner: Arc<Session>,
    /// Server → client half of the control stream, kept open after
    /// Hello/pong for later server messages (e.g. [`crate::protocol::RumbleMsg`]).
    control_tx: Arc<tokio::sync::Mutex<web_transport_quinn::SendStream>>,
    /// Client → server control lines the transport itself doesn't handle.
    control_lines: Arc<tokio::sync::Mutex<Option<tokio::sync::mpsc::UnboundedReceiver<String>>>>,
    video_block: Arc<AtomicU32>,
    audio_block: Arc<AtomicU32>,
    redundancy: f32,
    /// Receive side of the input event channel, populated by the per-
    /// session input reader task. Wrapped in `Mutex<Option<...>>` so the
    /// caller can `take()` it (single consumer) but the SessionHandle
    /// stays Clone for the publisher fan-out pattern.
    input_rx: Arc<tokio::sync::Mutex<Option<tokio::sync::mpsc::UnboundedReceiver<InputWireEvent>>>>,
    /// Stream of structured deficit events from the client (block_id +
    /// k_received + k_total). Single-consumer: the relay (or other
    /// repair-driving consumer) calls `take_deficit_events()` once
    /// and owns the receiver. Cloned SessionHandles share the same
    /// Mutex<Option<...>> so only one drain is possible per session.
    deficit_events: Arc<tokio::sync::Mutex<Option<tokio::sync::mpsc::Receiver<crate::protocol::DeficitMsg>>>>,
    /// Media datagrams handed to quinn by this session's publishers.
    /// Against quinn's count of datagrams actually transmitted, it shows
    /// when quinn is buffering or dropping what we send (see
    /// [`crate::rate`]).
    datagrams_queued: Arc<AtomicU64>,
}

impl SessionHandle {
    /// Cumulative link counters for [`crate::rate::RateController`].
    /// `at_ms` is the caller's monotonic clock; `unrecovered_blocks` is
    /// the caller's count of blocks the client reported it couldn't
    /// recover (it reads the deficit channel, not the transport).
    pub fn link_sample(&self, at_ms: u64, unrecovered_blocks: u64) -> crate::rate::LinkSample {
        let conn: &quinn::Connection = &self.inner;
        let s = conn.stats();
        crate::rate::LinkSample {
            at_ms,
            rtt_us: s.path.rtt.as_micros() as u64,
            sent_packets: s.path.sent_packets,
            lost_packets: s.path.lost_packets,
            sent_bytes: s.udp_tx.bytes,
            datagrams_queued: self.datagrams_queued.load(Ordering::Relaxed),
            datagrams_sent: s.frame_tx.datagram,
            unrecovered_blocks,
        }
    }

    /// Take the structured deficit receiver. Returns `None` after the
    /// first call (single-consumer semantics). The returned channel
    /// emits one [`DeficitMsg`] per client-detected block loss; the
    /// caller is expected to drive an on-demand-repair pipeline (e.g.
    /// `Publisher::emit_extra_repair`) from these events.
    pub async fn take_deficit_events(
        &self,
    ) -> Option<tokio::sync::mpsc::Receiver<crate::protocol::DeficitMsg>> {
        self.deficit_events.lock().await.take()
    }

    pub fn video_publisher(&self) -> Publisher {
        Publisher {
            session: self.inner.clone(),
            track: TrackId::Video,
            block_counter: self.video_block.clone(),
            redundancy: self.redundancy,
            datagrams_queued: self.datagrams_queued.clone(),
            encoder_ring: Arc::new(std::sync::Mutex::new(EncoderRing::new(
                ENCODER_RING_CAPACITY,
            ))),
        }
    }

    pub fn audio_publisher(&self) -> Publisher {
        Publisher {
            session: self.inner.clone(),
            track: TrackId::Audio,
            block_counter: self.audio_block.clone(),
            redundancy: self.redundancy,
            datagrams_queued: self.datagrams_queued.clone(),
            encoder_ring: Arc::new(std::sync::Mutex::new(EncoderRing::new(
                ENCODER_RING_CAPACITY,
            ))),
        }
    }

    /// Default redundancy for publishers made after this call
    /// ([`Publisher::send`]). Adaptive FEC passes its ratio per frame
    /// through [`Publisher::send_with_redundancy`] instead.
    pub fn set_redundancy(&mut self, ratio: f32) {
        self.redundancy = ratio.clamp(0.0, 1.0);
    }

    /// Wait for the underlying session to close. Useful for an outer task
    /// to know when to stop encoding.
    pub async fn closed(&self) {
        let _ = self.inner.closed().await;
    }

    /// Take the receiver of client control lines the transport doesn't
    /// handle itself (single consumer; None after the first call).
    pub async fn take_control_lines(&self) -> Option<tokio::sync::mpsc::UnboundedReceiver<String>> {
        self.control_lines.lock().await.take()
    }

    /// Open a reliable, ordered server → client stream (e.g. narnia-glserver's
    /// glstream). The client reads it from `incomingUnidirectionalStreams`.
    pub async fn open_uni(&self) -> Result<web_transport_quinn::SendStream> {
        self.inner.open_uni().await.context("open_uni")
    }

    /// Write one newline-terminated line on the control stream. Reliable
    /// and ordered, unlike the datagram tracks.
    pub async fn send_control_line(&self, line: &str) -> Result<()> {
        let mut tx = self.control_tx.lock().await;
        let mut buf = line.as_bytes();
        while !buf.is_empty() {
            let n = tx.write(buf).await.context("control stream write")?;
            buf = &buf[n..];
        }
        Ok(())
    }

    /// Close the session with a WebTransport application code and a
    /// reason the client can show (e.g. another device took over).
    pub fn close(&self, code: u32, reason: &str) {
        self.inner.close(code, reason.as_bytes());
    }

    /// Take ownership of the input-event receiver. Returns `None` after
    /// the first call (single-consumer). Caller drains
    /// [`InputWireEvent`]s and dispatches them to the application input
    /// path (typically `WaylandBackend::inject_*`).
    pub async fn take_input_receiver(
        &self,
    ) -> Option<tokio::sync::mpsc::UnboundedReceiver<InputWireEvent>> {
        self.input_rx.lock().await.take()
    }
}

/// Power-of-two capacity of the per-Publisher encoder ring. At 60fps
/// this is ~1.07s of in-flight encoders — enough that the relay can
/// fulfill on-demand repair for any block within the most recent
/// second. Memory cost: ~6 MB per second of 1080p60 video at typical
/// bitrates (one BlockEncoder per source object, each ~50-100KB).
const ENCODER_RING_CAPACITY: usize = 64;

/// Per-track publisher. Cheap to clone; thread-safe.
///
/// Holds an encoder ring so that on-demand repair (the relay's
/// recovery-from-ring path) can mint *fresh* repair symbols at new
/// ESIs, instead of re-encoding the source from scratch and
/// retransmitting the whole K+R packet sequence. This is the
/// bandwidth-efficient version of phase-2 replay-on-NACK — only the
/// genuinely needed extra repair goes on the wire, not duplicated
/// source.
#[derive(Clone)]
pub struct Publisher {
    session: Arc<Session>,
    track: TrackId,
    block_counter: Arc<AtomicU32>,
    redundancy: f32,
    /// Recently-sent source blocks, keyed by block_id, holding the
    /// `BlockEncoder` so we can call `extra_packets(n)` on demand.
    /// Wrapped in std::sync::Mutex; lock contention is bounded
    /// (operations are O(1) insert + O(N) lookup with N ≤ 64).
    /// Inserts during normal `send`, lookups during `emit_extra_repair`.
    encoder_ring: Arc<std::sync::Mutex<EncoderRing>>,
    datagrams_queued: Arc<AtomicU64>,
}

/// Fixed-capacity ring of `BlockEncoder` instances, keyed by block_id.
/// Slot index = `block_id & (capacity - 1)` so writing block N
/// overwrites whatever was in slot `N % capacity` (typically the
/// block from `capacity` writes ago).
struct EncoderRing {
    capacity: usize,
    mask: u32,
    slots: Vec<Option<EncoderSlot>>,
}

struct EncoderSlot {
    block_id: u32,
    encoder: BlockEncoder,
}

impl EncoderRing {
    fn new(capacity: usize) -> Self {
        assert!(capacity.is_power_of_two() && capacity > 0);
        let mut slots = Vec::with_capacity(capacity);
        for _ in 0..capacity {
            slots.push(None);
        }
        Self {
            capacity,
            mask: (capacity as u32) - 1,
            slots,
        }
    }

    fn insert(&mut self, block_id: u32, encoder: BlockEncoder) {
        let idx = (block_id & self.mask) as usize;
        self.slots[idx] = Some(EncoderSlot { block_id, encoder });
    }

    fn get_mut(&mut self, block_id: u32) -> Option<&mut BlockEncoder> {
        let idx = (block_id & self.mask) as usize;
        match &mut self.slots[idx] {
            Some(slot) if slot.block_id == block_id => Some(&mut slot.encoder),
            _ => None,
        }
    }

    /// Iterate over the most-recent `n` blocks (newest first) that
    /// are still present in the ring. Skips overwritten slots.
    fn recent_block_ids(&self, n: usize) -> Vec<u32> {
        let n = n.min(self.capacity);
        // Find highest block_id present.
        let mut ids: Vec<u32> = self
            .slots
            .iter()
            .filter_map(|s| s.as_ref().map(|x| x.block_id))
            .collect();
        ids.sort_unstable();
        ids.into_iter().rev().take(n).collect()
    }
}

impl Publisher {
    /// Return a point-in-time CC / path stats snapshot for the underlying
    /// QUIC connection. Cheap to call — quinn's stats() is a lock-free copy.
    /// Useful for adaptive-bitrate loops that want to know live bandwidth
    /// estimates, RTT, or cumulative loss without adding any side-channels.
    pub fn cc_stats(&self) -> crate::cc_stats::CcStats {
        crate::cc_stats::snapshot(&self.session)
    }

    /// Send one source object using the publisher's session-level
    /// redundancy. See [`Publisher::send_with_redundancy`] for the
    /// per-call override variant — that's what video uses to apply
    /// extra redundancy to keyframes specifically. Returns the
    /// block_id assigned to this source object so callers can later
    /// reference it for on-demand repair via `emit_extra_repair`.
    pub fn send(&self, source: &[u8]) -> Result<u32> {
        self.send_with_redundancy(source, self.redundancy)
    }

    /// Send one source object (one encoded video frame or one opus
    /// packet) with a CALLER-CHOSEN redundancy ratio. RaptorQ being a
    /// fountain code, the encoder happily emits more or fewer repair
    /// symbols on demand — we use this to apply more aggressive FEC
    /// to keyframes (which can't be lost without ruining ~1s of
    /// downstream P-frames) while keeping P-frame overhead modest.
    ///
    /// The wire format is unchanged: each datagram still carries a
    /// SymbolHeader with K, ESI, etc. The client decodes when it has
    /// any K source symbols regardless of how many repair packets the
    /// server chose to emit. So clients don't need to know what R was
    /// in effect for any given block.
    pub fn send_with_redundancy(&self, source: &[u8], redundancy: f32) -> Result<u32> {
        if source.is_empty() {
            return Ok(0);
        }

        let block_id = self.block_counter.fetch_add(1, Ordering::Relaxed);
        // Tracy span: covers raptorq fragmentation + N send_datagram
        // calls for one source object. Bar = total time the publisher
        // thread spends transmitting one frame's worth of symbols.
        // Stays under a few hundred microseconds in healthy state.
        #[cfg(feature = "trace")]
        let _send_span = tracing::info_span!(
            "transport.send",
            block = block_id,
            track = ?self.track,
            bytes = source.len(),
            redundancy,
        ).entered();
        let mut enc = BlockEncoder::new(source, block_id, redundancy);

        let mut hdr_buf = BytesMut::with_capacity(SymbolHeader::WIRE_LEN + crate::fec::SYMBOL_SIZE as usize);
        while let Some(packet) = enc.next_packet() {
            let payload = packet.data();
            let esi = packet.payload_id().encoding_symbol_id();

            hdr_buf.clear();
            SymbolHeader {
                track: self.track,
                esi,
                block: block_id,
                k: enc.k(),
                size: enc.size,
            }
            .encode(&mut hdr_buf);
            hdr_buf.extend_from_slice(payload);

            let datagram = hdr_buf.clone().freeze();
            // send_datagram returns Err only if the connection is closing.
            // We swallow per-symbol errors so a transient hiccup doesn't
            // tear down the whole publisher; the FEC tolerates them.
            if self.session.send_datagram(datagram).is_err() {
                // Connection's gone. Bubble up so the caller can stop.
                anyhow::bail!("session closed during send");
            }
            self.datagrams_queued.fetch_add(1, Ordering::Relaxed);
        }
        // Stash the encoder so on-demand repair can mint extra symbols
        // for this block_id later (relay's deficit-recovery path). The
        // ring evicts the oldest entry by overwrite when full —
        // ENCODER_RING_CAPACITY blocks ago is no longer recoverable
        // via this publisher, which is fine: the relay's worst-case
        // repair window is bounded by ring capacity / fps.
        if let Ok(mut ring) = self.encoder_ring.lock() {
            ring.insert(block_id, enc);
        }
        Ok(block_id)
    }

    /// On-demand repair for a previously-sent block. Mints `count`
    /// fresh repair symbols at new ESIs (continuing the ESI sequence
    /// past the original K+R emission), wraps each in a SymbolHeader
    /// matching the original block's metadata, and ships as
    /// datagrams. Used by the relay's deficit handler to fulfill client
    /// loss recovery WITHOUT re-sending source bytes the client may
    /// already have.
    ///
    /// Returns the number of symbols actually emitted. Returns 0 if
    /// the block_id has been evicted from the encoder ring (= too old
    /// to repair from this publisher).
    pub fn emit_extra_repair(&self, block_id: u32, count: u32) -> Result<u32> {
        if count == 0 {
            return Ok(0);
        }
        let (k, size, packets) = {
            let mut ring = self
                .encoder_ring
                .lock()
                .map_err(|_| anyhow::anyhow!("encoder ring lock poisoned"))?;
            let Some(enc) = ring.get_mut(block_id) else {
                return Ok(0); // block evicted from ring or never sent
            };
            let pkts = enc.extra_packets(count);
            (enc.k(), enc.size, pkts)
        };

        let mut hdr_buf = BytesMut::with_capacity(SymbolHeader::WIRE_LEN + crate::fec::SYMBOL_SIZE as usize);
        let mut emitted = 0u32;
        for packet in packets {
            let payload = packet.data();
            let esi = packet.payload_id().encoding_symbol_id();
            hdr_buf.clear();
            SymbolHeader {
                track: self.track,
                esi,
                block: block_id,
                k,
                size,
            }
            .encode(&mut hdr_buf);
            hdr_buf.extend_from_slice(payload);
            let datagram = hdr_buf.clone().freeze();
            if self.session.send_datagram(datagram).is_err() {
                anyhow::bail!("session closed during extra-repair send");
            }
            self.datagrams_queued.fetch_add(1, Ordering::Relaxed);
            emitted += 1;
        }
        Ok(emitted)
    }

    /// IDs of the most-recently-sent N blocks still alive in the
    /// encoder ring, newest first. Used by the relay to know what's
    /// recoverable when a client signals loss without specifying a
    /// block_id (the structured `deficit ...\n` protocol).
    pub fn recent_block_ids(&self, n: usize) -> Vec<u32> {
        self.encoder_ring
            .lock()
            .map(|r| r.recent_block_ids(n))
            .unwrap_or_default()
    }

    pub fn track(&self) -> TrackId {
        self.track
    }
}

// Re-export Bytes for callers that want to construct one.
pub use bytes::Bytes as SendBytes;

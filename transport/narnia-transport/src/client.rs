//! Subscribing side of the transport.
//!
//! ## Lifecycle
//!
//! 1. Construct a [`Client`] pointed at a server URL (`https://...`).
//! 2. `connect()` does the TLS + WebTransport handshake, reads the server's
//!    hello on the control stream, and returns a [`Subscription`].
//! 3. From the subscription, get a [`TrackReceiver`] per track. Each
//!    receiver yields recovered source-object payloads (one HEVC frame
//!    or one opus packet per item).
//!
//! ## Loss tolerance
//!
//! The client maintains one [`BlockDecoder`] per (track, block) tuple.
//! As datagrams arrive their [`SymbolHeader`] routes them to the correct
//! decoder; when a decoder has enough symbols to recover the source it
//! emits the result and drops out of the active set. Older blocks are
//! garbage-collected after a small horizon so that lost-block scenarios
//! don't leak memory.

use std::collections::HashMap;
use std::sync::Arc;

use anyhow::{Context, Result};
use bytes::Bytes;
use tokio::sync::mpsc;

use crate::fec::BlockDecoder;
use crate::protocol::{Hello, InputWireEvent, SymbolHeader, TrackId, PROTOCOL_VERSION};
use crate::socket_tuning;

/// Active blocks kept per track before garbage collecting undecoded ones.
/// At 60fps, this is the eviction window — a block held this many frames
/// without decoding gets declared lost. Trade-off: bigger window catches
/// more late-arriving symbols (recovers more blocks); smaller window
/// declares loss faster, freeing the renderer to skip ahead instead of
/// stalling. 64 is a defensive ceiling — real eviction happens via
/// [`BLOCK_EVICTION_TIMEOUT_MS`], not capacity. We keep capacity for
/// memory safety on a misbehaving source, but the time-based check is
/// what triggers nacks in normal operation.
const MAX_INFLIGHT_BLOCKS: usize = 64;

/// When to declare a block "in trouble" and emit a NACK to the relay
/// for on-demand repair. Doesn't drop the decoder — the existing
/// in-flight symbols stay so on-demand repair symbols can land into
/// the same decoder context. One frame interval (16.67ms at 60fps)
/// plus reassembly slack covers normal jitter without false alarms.
const BLOCK_DEFICIT_TIMEOUT_MS: u64 = 30;

/// When to actually drop a block's decoder + count it as a hard loss.
/// Has to be long enough to cover one full nack round-trip + repair
/// arrival (relay→client typically <30ms). 200ms gives ample margin
/// for the on-demand repair symbols to land into the live decoder
/// even on a noisy wifi where the nack round-trip itself sees loss.
/// After this, the block is genuinely unrecoverable and we free the
/// memory.
const BLOCK_EVICTION_TIMEOUT_MS: u64 = 200;

/// Noop TLS certificate verifier used by [`Client::insecure`]. Accepts any
/// server cert — appropriate when connecting over a trusted VPN (tailscale)
/// where transport-level encryption is enough and we're not relying on PKI.
#[derive(Debug)]
struct NoVerify(Arc<rustls::crypto::CryptoProvider>);

impl rustls::client::danger::ServerCertVerifier for NoVerify {
    fn verify_server_cert(
        &self,
        _end_entity: &rustls::pki_types::CertificateDer<'_>,
        _intermediates: &[rustls::pki_types::CertificateDer<'_>],
        _server_name: &rustls::pki_types::ServerName<'_>,
        _ocsp_response: &[u8],
        _now: rustls::pki_types::UnixTime,
    ) -> std::result::Result<rustls::client::danger::ServerCertVerified, rustls::Error> {
        Ok(rustls::client::danger::ServerCertVerified::assertion())
    }

    fn verify_tls12_signature(
        &self,
        _message: &[u8],
        _cert: &rustls::pki_types::CertificateDer<'_>,
        _dss: &rustls::DigitallySignedStruct,
    ) -> std::result::Result<rustls::client::danger::HandshakeSignatureValid, rustls::Error> {
        Ok(rustls::client::danger::HandshakeSignatureValid::assertion())
    }

    fn verify_tls13_signature(
        &self,
        _message: &[u8],
        _cert: &rustls::pki_types::CertificateDer<'_>,
        _dss: &rustls::DigitallySignedStruct,
    ) -> std::result::Result<rustls::client::danger::HandshakeSignatureValid, rustls::Error> {
        Ok(rustls::client::danger::HandshakeSignatureValid::assertion())
    }

    fn supported_verify_schemes(&self) -> Vec<rustls::SignatureScheme> {
        self.0.signature_verification_algorithms.supported_schemes()
    }
}

#[derive(Debug)]
struct PinnedCert {
    provider: Arc<rustls::crypto::CryptoProvider>,
    cert_sha256: [u8; 32],
}

impl rustls::client::danger::ServerCertVerifier for PinnedCert {
    fn verify_server_cert(
        &self,
        end_entity: &rustls::pki_types::CertificateDer<'_>,
        _intermediates: &[rustls::pki_types::CertificateDer<'_>],
        _server_name: &rustls::pki_types::ServerName<'_>,
        _ocsp_response: &[u8],
        _now: rustls::pki_types::UnixTime,
    ) -> std::result::Result<rustls::client::danger::ServerCertVerified, rustls::Error> {
        let digest = ring::digest::digest(&ring::digest::SHA256, end_entity.as_ref());
        if digest.as_ref() == self.cert_sha256 {
            Ok(rustls::client::danger::ServerCertVerified::assertion())
        } else {
            Err(rustls::Error::General(
                "server certificate does not match the pinned sha-256 hash".into(),
            ))
        }
    }

    fn verify_tls12_signature(
        &self,
        message: &[u8],
        cert: &rustls::pki_types::CertificateDer<'_>,
        dss: &rustls::DigitallySignedStruct,
    ) -> std::result::Result<rustls::client::danger::HandshakeSignatureValid, rustls::Error> {
        rustls::crypto::verify_tls12_signature(
            message,
            cert,
            dss,
            &self.provider.signature_verification_algorithms,
        )
    }

    fn verify_tls13_signature(
        &self,
        message: &[u8],
        cert: &rustls::pki_types::CertificateDer<'_>,
        dss: &rustls::DigitallySignedStruct,
    ) -> std::result::Result<rustls::client::danger::HandshakeSignatureValid, rustls::Error> {
        rustls::crypto::verify_tls13_signature(
            message,
            cert,
            dss,
            &self.provider.signature_verification_algorithms,
        )
    }

    fn supported_verify_schemes(&self) -> Vec<rustls::SignatureScheme> {
        self.provider.signature_verification_algorithms.supported_schemes()
    }
}

/// Connector. Holds rustls + quinn config; cheap to clone.
pub struct Client {
    inner: web_transport_quinn::Client,
}

impl Client {
    /// Build a client that does NOT verify the server cert. Required when
    /// connecting to a tailnet-resident server using a self-signed cert
    /// (which is most of our use cases). For production-grade trust pin
    /// the server's cert hash via [`Client::pinned`].
    pub fn insecure() -> Result<Self> {
        let verifier = Arc::new(NoVerify(rustls::crypto::ring::default_provider().into()));
        Self::with_verifier(verifier)
    }

    pub fn pinned(cert_sha256: [u8; 32]) -> Result<Self> {
        let verifier = Arc::new(PinnedCert {
            provider: rustls::crypto::ring::default_provider().into(),
            cert_sha256,
        });
        Self::with_verifier(verifier)
    }

    fn with_verifier(verifier: Arc<dyn rustls::client::danger::ServerCertVerifier>) -> Result<Self> {
        let _ = rustls::crypto::ring::default_provider().install_default();

        // Build the rustls ClientConfig the same way ClientBuilder would, but
        // intercept the quinn::Endpoint creation so we can bind with a tuned
        // socket first. ClientBuilder::build() calls
        // quinn::Endpoint::client("[::]:0") internally with no hooks, so we
        // bypass it and call web_transport_quinn::Client::new directly.
        let mut tls_config = rustls::ClientConfig::builder_with_provider(
            rustls::crypto::ring::default_provider().into(),
        )
        .with_protocol_versions(&[&rustls::version::TLS13])
        .context("ClientConfig::with_protocol_versions")?
        .dangerous()
        .with_custom_certificate_verifier(verifier)
        .with_no_client_auth();
        tls_config.alpn_protocols = vec![web_transport_quinn::ALPN.as_bytes().to_vec()];

        let quic_client_config: quinn::crypto::rustls::QuicClientConfig =
            tls_config.try_into().map_err(|e| anyhow::anyhow!("QuicClientConfig: {e:?}"))?;
        let mut client_config = quinn::ClientConfig::new(Arc::new(quic_client_config));

        // BBR congestion control — same rationale as server.rs: both ends
        // must speak BBR for the win. Co-deployed in one push.
        let mut transport = quinn::TransportConfig::default();
        transport.congestion_controller_factory(Arc::new(quinn::congestion::BbrConfig::default()));
        client_config.transport_config(Arc::new(transport));

        // Bind a tuned socket on the wildcard IPv6 address (dual-stack).
        // The client doesn't need a fixed port so we use :0.
        let client_addr = std::net::SocketAddr::from((std::net::Ipv6Addr::UNSPECIFIED, 0));
        let udp = socket_tuning::bind_tuned_socket(client_addr)
            .context("bind_tuned_socket for client")?;

        let runtime = quinn::default_runtime()
            .ok_or_else(|| anyhow::anyhow!("no async runtime found for quinn"))?;
        let endpoint = quinn::Endpoint::new(
            quinn::EndpointConfig::default(),
            None,
            udp,
            runtime,
        )
        .context("quinn::Endpoint::new (client)")?;

        let inner = web_transport_quinn::Client::new(endpoint, client_config);
        Ok(Self { inner })
    }

    /// Connect to `url`, complete the WebTransport handshake, read the
    /// server's hello.
    pub async fn connect(&self, url: &str) -> Result<Subscription> {
        let url: url::Url = url
            .parse()
            .with_context(|| format!("parsing server URL: {url}"))?;

        let session = self.inner.connect(url).await.context("connecting")?;

        // Read the hello on the control stream. Keep the send half too —
        // we use it for the clock-sync ping that follows Hello.
        let (mut send, mut recv) = session
            .accept_bi()
            .await
            .context("accepting control stream")?;
        let line = read_line(&mut recv).await.context("reading hello")?;
        let hello = Hello::parse(&line).context("parsing hello")?;

        // Wire-format version is now tracked at the narnia ALPN layer
        // (see narnia_transport::alpn). The underlying TLS ALPN is "h3"
        // (required by WebTransport); narnia/v1 is our identifier for
        // telemetry. This numeric field is retained for forensic logs only.
        if hello.protocol_version != PROTOCOL_VERSION {
            tracing::warn!(
                server = format!("0x{:08x}", hello.protocol_version),
                client = format!("0x{:08x}", PROTOCOL_VERSION),
                narnia_alpn = std::str::from_utf8(crate::alpn::ALPN_CURRENT).unwrap_or("?"),
                "PROTOCOL_VERSION numeric mismatch — telemetry-only check"
            );
        } else {
            tracing::debug!(
                narnia_alpn = std::str::from_utf8(crate::alpn::ALPN_CURRENT).unwrap_or("?"),
                protocol_version = format!("0x{:08x}", hello.protocol_version),
                "protocol version ok"
            );
        }

        // NTP-style clock-sync. Stamp t1 right before send, ship the
        // ping, read pong, stamp t4 right after recv. Both timestamps
        // are SystemTime → microseconds since UNIX epoch, sampled
        // tightly around the syscalls so client-side processing time
        // doesn't bias the math. Failure here is non-fatal — we fall
        // back to (skew = 0, transit = 0) which lets older servers
        // still work, just without the clock correction.
        let (skew_us, transit_us) = match clock_sync(&mut send, &mut recv).await {
            Ok((s, t)) => (s, t),
            Err(e) => {
                tracing::warn!(error = ?e, "clock-sync failed; per-frame G2G measurements will not be honest");
                (0, 0)
            }
        };

        let session = Arc::new(session);
        Ok(Subscription {
            session,
            hello,
            skew_us,
            transit_us,
            // Park the bidi control stream's send half so split() can
            // hand it to the NACK writer task. We're done with the
            // recv half (Hello + Pong already read); a future
            // bidirectional control protocol would need to keep that
            // alive too.
            deficit_send: std::sync::Mutex::new(Some(send)),
        })
    }
}

async fn clock_sync(
    send: &mut web_transport_quinn::SendStream,
    recv: &mut web_transport_quinn::RecvStream,
) -> Result<(i64, i64)> {
    use crate::protocol::{PingMsg, PongMsg};
    let t1_us = std::time::SystemTime::now()
        .duration_since(std::time::SystemTime::UNIX_EPOCH)
        .map(|d| d.as_micros() as u64)
        .unwrap_or(0);
    let ping = PingMsg { t1_us };
    let line = ping.to_line();
    let mut buf = line.as_bytes();
    while !buf.is_empty() {
        let n = send.write(buf).await.context("writing ping")?;
        buf = &buf[n..];
    }
    let pong_line = tokio::time::timeout(
        std::time::Duration::from_millis(2000),
        read_line(recv),
    )
    .await
    .context("clock-sync pong timeout")?
    .context("reading pong")?;
    let t4_us = std::time::SystemTime::now()
        .duration_since(std::time::SystemTime::UNIX_EPOCH)
        .map(|d| d.as_micros() as u64)
        .unwrap_or(0);
    let pong = PongMsg::parse(&pong_line)
        .ok_or_else(|| anyhow::anyhow!("malformed pong: {pong_line}"))?;
    if pong.t1_us != t1_us {
        anyhow::bail!("pong t1 mismatch: sent {t1_us}, got {}", pong.t1_us);
    }
    // skew (server clock - client clock); positive means server clock ahead.
    let skew_us = ((pong.t2_us as i64 - t1_us as i64)
        + (pong.t3_us as i64 - t4_us as i64))
        / 2;
    // one-way transit, symmetric assumption.
    let transit_us = ((t4_us as i64 - t1_us as i64)
        - (pong.t3_us as i64 - pong.t2_us as i64))
        / 2;
    tracing::info!(
        t1_us, t2 = pong.t2_us, t3 = pong.t3_us, t4_us,
        skew_us, transit_us,
        "clock-sync complete"
    );
    Ok((skew_us, transit_us))
}

async fn read_line(recv: &mut web_transport_quinn::RecvStream) -> Result<String> {
    let mut buf = Vec::with_capacity(64);
    let mut byte = [0u8; 1];
    loop {
        let n = recv.read(&mut byte).await.context("reading hello byte")?;
        let n = n.unwrap_or(0);
        if n == 0 {
            anyhow::bail!("server closed control stream before hello");
        }
        if byte[0] == b'\n' {
            break;
        }
        buf.push(byte[0]);
        if buf.len() > 256 {
            anyhow::bail!("hello message exceeds 256 bytes (malformed)");
        }
    }
    Ok(String::from_utf8(buf).context("hello not utf-8")?)
}

/// One active subscription. Spawns a background task that drains datagrams
/// and routes them to per-track receivers.
pub struct Subscription {
    pub(crate) session: Arc<web_transport_quinn::Session>,
    hello: Hello,
    /// Server clock minus client clock, microseconds. Computed via the
    /// NTP-style 4-timestamp ping/pong on the control stream right
    /// after Hello. Add to capture_us_server to get the equivalent
    /// client-clock instant.
    skew_us: i64,
    /// Estimated one-way wire transit time, microseconds. Symmetric
    /// assumption (forward and reverse paths take the same time).
    /// Logged for diagnostics; not directly used in per-frame math.
    transit_us: i64,
    /// Bidi control stream's send half, parked here after the Hello
    /// + clock-sync exchange. Taken at split() time to drive the NACK
    /// writer task. Mutex+Option because split() consumes Subscription
    /// and we need interior mutability for clone_for_input which
    /// shares the same Arc<Session>; only one place actually drains
    /// it.
    deficit_send: std::sync::Mutex<Option<web_transport_quinn::SendStream>>,
}

impl Subscription {
    pub fn hello(&self) -> &Hello {
        &self.hello
    }

    /// Server clock minus client clock from the post-Hello clock-sync
    /// exchange. Used by the client to translate per-frame
    /// `capture_us_server` into client-clock-equivalent times for
    /// honest cross-host glass-to-decoded measurement.
    pub fn skew_us(&self) -> i64 {
        self.skew_us
    }

    /// Estimated one-way network transit time from the same
    /// clock-sync exchange. Useful to log at session start so the
    /// budget breakdown makes sense.
    pub fn transit_us(&self) -> i64 {
        self.transit_us
    }

    /// Current quinn-tracked RTT for this connection. Kept available
    /// for diagnostics; the clock-sync exchange above is the
    /// authoritative source for per-frame correction because quinn's
    /// initial RTT estimate is inflated by handshake overhead.
    pub fn rtt(&self) -> std::time::Duration {
        self.session.rtt()
    }

    /// Return a point-in-time CC / path stats snapshot for the underlying
    /// QUIC connection. Cheap to call — quinn's stats() is a lock-free copy.
    /// Useful for the adaptive-bitrate feedback loop: client can read
    /// live cwnd / RTT / loss and send a NACK or bitrate hint to the server.
    pub fn cc_stats(&self) -> crate::cc_stats::CcStats {
        crate::cc_stats::snapshot(&self.session)
    }

    /// Make a sibling handle that shares the underlying QUIC session.
    /// Lets the caller hold a separate handle for opening the input
    /// uni stream while the split datagram pump consumes the original.
    pub fn clone_for_input(&self) -> Subscription {
        Subscription {
            session: self.session.clone(),
            hello: self.hello.clone(),
            skew_us: self.skew_us,
            transit_us: self.transit_us,
            // The clone never owns the bidi send half — only one
            // place in the codebase drains deficit_send, namely the
            // original Subscription's split() call.
            deficit_send: std::sync::Mutex::new(None),
        }
    }

    /// Open the client → server input uni stream and return an
    /// [`InputPublisher`] that batches and writes [`InputWireEvent`]s
    /// to it. Reliable / ordered delivery (loss-tolerant retransmits),
    /// because dropping a key release leaves the server with a stuck
    /// key, which is much worse than the few-ms retransmit cost.
    pub async fn open_input_publisher(&self) -> anyhow::Result<InputPublisher> {
        // Input goes via QUIC datagrams rather than a uni stream.
        // Streams are subject to quinn's per-connection scheduler +
        // pacer, which on a low-volume tiny-write input channel
        // produced 100-700ms arrival gaps end-to-end (verified with
        // client `input.window` + server `[x11-input] arrival gap`
        // instrumentation, 2026-05). Datagrams skip both the
        // scheduler and pacing — each `send_datagram` becomes a UDP
        // packet on the wire on the next quinn send tick (~1ms).
        //
        // Trade-off: datagrams are unreliable. A dropped mouse motion
        // event self-corrects on the next event. A dropped key
        // release leaves the server with a stuck key — but because
        // we don't yet observe loss in practice, the latency win is
        // worth the reliability risk for now. If stuck keys become a
        // problem, switch to "datagram first, periodic state-resync
        // over reliable stream" or hybrid (mouse → datagram, keys →
        // stream).
        Ok(InputPublisher {
            session: self.session.clone(),
        })
    }

    /// Spawn the datagram pump and return one channel per track. The pump
    /// runs until the underlying session closes.
    pub fn split(self) -> (TrackReceiver, TrackReceiver) {
        let (video_tx, video_rx) = mpsc::unbounded_channel::<RecoveredFrame>();
        let (audio_tx, audio_rx) = mpsc::unbounded_channel::<RecoveredFrame>();

        // Per-track lost-block counters. Incremented when
        // ReassemblyState evicts a block before it decoded. Cloned
        // into the pump task (writer) and the TrackReceivers (reader)
        // so the client app can poll "did we lose anything since last
        // check" and trigger skip-until-IDR rendering.
        let video_loss = Arc::new(std::sync::atomic::AtomicU64::new(0));
        let audio_loss = Arc::new(std::sync::atomic::AtomicU64::new(0));

        // NACK channel: reassembly pushes a structured NackMsg per
        // lost block; nack writer task drains and ships the line on
        // the bidi control stream. Bounded(8) so the relay's repair
        // pipeline can absorb a small burst of distinct losses
        // without backpressuring the reassembly hot path; try_send on
        // the producer side drops on overflow.
        let (deficit_tx, deficit_rx) = mpsc::channel::<crate::protocol::DeficitMsg>(8);

        // Spawn the NACK writer if we still have the control stream's
        // send half. It opens a fresh uni stream per NACK and writes
        // "pixelnack\n" — server's existing input-stream-accept loop
        // is the only place uni streams arrive, but we differentiate
        // by content: a uni stream whose first bytes are "pixelnack"
        // gets routed to the force-IDR signal instead of input.
        if let Some(send) = self.deficit_send.lock().unwrap().take() {
            spawn_deficit_writer(send, deficit_rx);
        }

        let session = self.session.clone();
        let video_loss_writer = video_loss.clone();
        let audio_loss_writer = audio_loss.clone();
        tokio::spawn(async move {
            let mut state = ReassemblyState::default();
            state.video_loss_counter = Some(video_loss_writer);
            state.audio_loss_counter = Some(audio_loss_writer);
            state.deficit_tx = Some(deficit_tx);
            let mut dgrams_seen: u64 = 0;
            let mut dgrams_with_frame: u64 = 0;
            let mut last_log = std::time::Instant::now();
            loop {
                let dgram = match session.read_datagram().await {
                    Ok(b) => b,
                    Err(e) => {
                        tracing::warn!(error = ?e, dgrams_seen, "read_datagram terminated");
                        return;
                    }
                };
                dgrams_seen += 1;
                if dgrams_seen <= 3 || dgrams_seen % 200 == 0 {
                    let header_preview = if dgram.len() >= 17 {
                        format!(
                            "track={} esi={} block={} k={} size={} dlen={}",
                            dgram[0],
                            u32::from_be_bytes(dgram[1..5].try_into().unwrap()),
                            u32::from_be_bytes(dgram[5..9].try_into().unwrap()),
                            u32::from_be_bytes(dgram[9..13].try_into().unwrap()),
                            u32::from_be_bytes(dgram[13..17].try_into().unwrap()),
                            dgram.len(),
                        )
                    } else {
                        format!("dlen={} (too short)", dgram.len())
                    };
                    tracing::info!(seen = dgrams_seen, with_frame = dgrams_with_frame, %header_preview, "rx.datagram");
                }
                if let Some(frame) = state.feed(&dgram) {
                    dgrams_with_frame += 1;
                    if last_log.elapsed() >= std::time::Duration::from_secs(2) {
                        tracing::info!(dgrams_seen, dgrams_with_frame, "rx.summary");
                        last_log = std::time::Instant::now();
                    }
                    let tx = match frame.track {
                        TrackId::Video => &video_tx,
                        TrackId::Audio => &audio_tx,
                    };
                    if tx.send(frame).is_err() {
                        // receiver dropped — keep pumping the other
                        // track, just skip emitting on this one.
                    }
                }
            }
        });

        (
            TrackReceiver {
                track: TrackId::Video,
                rx: video_rx,
                loss_counter: video_loss,
            },
            TrackReceiver {
                track: TrackId::Audio,
                rx: audio_rx,
                loss_counter: audio_loss,
            },
        )
    }
}

/// Background task that drains NACK signals and writes one
/// "pixelnack\n" line per signal on the bidi control stream's send
/// half. Stays alive for the session.
fn spawn_deficit_writer(
    mut send: web_transport_quinn::SendStream,
    mut deficit_rx: mpsc::Receiver<crate::protocol::DeficitMsg>,
) {
    tokio::spawn(async move {
        while let Some(deficit) = deficit_rx.recv().await {
            let line = deficit.to_line();
            let mut buf = line.as_bytes();
            while !buf.is_empty() {
                match send.write(buf).await {
                    Ok(n) => buf = &buf[n..],
                    Err(e) => {
                        tracing::warn!(error = ?e, "nack write failed; stopping nack writer");
                        return;
                    }
                }
            }
        }
    });
}

/// One recovered source object (one HEVC frame or opus packet) plus the
/// metadata needed to mux/order it.
#[derive(Debug, Clone)]
pub struct RecoveredFrame {
    pub track: TrackId,
    pub block: u32,
    pub payload: Bytes,
}

/// Per-track stream of recovered frames. `recv().await` returns the next
/// fully-decoded payload; returns `None` when the session closes.
pub struct TrackReceiver {
    track: TrackId,
    rx: mpsc::UnboundedReceiver<RecoveredFrame>,
    /// Cumulative count of blocks evicted from the reassembly pool
    /// before they could decode (= visible frame loss). Shared with
    /// the pump task. Caller polls this with `lost_count()` and
    /// triggers skip-until-IDR + NACK when the value increases.
    loss_counter: Arc<std::sync::atomic::AtomicU64>,
}

impl TrackReceiver {
    /// Cumulative count of lost blocks for this track since session
    /// start. Increments asynchronously from the pump task; the caller
    /// is expected to poll + remember the previous value to detect
    /// "did we lose anything between the last two checks." Cheap —
    /// just a relaxed atomic load.
    pub fn lost_count(&self) -> u64 {
        self.loss_counter.load(std::sync::atomic::Ordering::Relaxed)
    }

    /// Clone the inner loss-counter Arc so the caller can poll it from
    /// outside the receiver — needed when the receiver is moved into
    /// a tokio task (consumed by `recv().await`) but we want to also
    /// monitor losses from the main thread.
    pub fn loss_counter(&self) -> Arc<std::sync::atomic::AtomicU64> {
        self.loss_counter.clone()
    }
}

/// Send side of the input event channel — sends [`InputWireEvent`]s as
/// QUIC datagrams. Datagrams are unreliable but bypass the per-connection
/// stream scheduler / pacer that was adding 100-700ms of latency on the
/// stream-based path.
pub struct InputPublisher {
    session: Arc<web_transport_quinn::Session>,
}

impl InputPublisher {
    pub async fn send(&mut self, event: InputWireEvent) -> anyhow::Result<()> {
        let mut buf = bytes::BytesMut::with_capacity(InputWireEvent::MAX_WIRE_LEN);
        event.encode(&mut buf);
        // send_datagram is sync (returns Err only if connection is
        // closing); no await. The datagram is queued for the next
        // wire send tick.
        self.session
            .send_datagram(buf.freeze())
            .context("sending input datagram")
    }
}

impl TrackReceiver {
    pub fn track(&self) -> TrackId {
        self.track
    }
    pub async fn recv(&mut self) -> Option<RecoveredFrame> {
        self.rx.recv().await
    }
}

/// Per-track decoder pool. One decoder per active block; oldest blocks
/// evicted when the inflight budget is exceeded.
#[derive(Default)]
struct ReassemblyState {
    video: HashMap<u32, BlockTracker>,
    audio: HashMap<u32, BlockTracker>,
    /// Cumulative stats since session start, per track. Used by the
    /// periodic logger to surface loss/decode rates without us having
    /// to hold every block forever.
    video_stats: AggregateStats,
    audio_stats: AggregateStats,
    /// Cloned from TrackReceivers so each block-loss in feed() can be
    /// surfaced to the consumer for skip-until-IDR rendering.
    video_loss_counter: Option<Arc<std::sync::atomic::AtomicU64>>,
    audio_loss_counter: Option<Arc<std::sync::atomic::AtomicU64>>,
    /// Pushed once per video block loss. The NACK writer task drains
    /// this and ships a structured `pixelnack ...\n` line carrying
    /// block_id + k_received + k_total to the server (or relay), which
    /// uses the metadata to mint exactly the right number of fresh
    /// repair symbols via on-demand raptorq. The bounded(8) channel
    /// holds a small queue of distinct lost-block events; coalescing
    /// happens at the relay's repair scheduler if needed.
    deficit_tx: Option<mpsc::Sender<crate::protocol::DeficitMsg>>,
    /// Highest block_id we've ever emitted as a `RecoveredFrame` for
    /// this track. Late-arriving blocks (recovered via on-demand
    /// repair AFTER a higher block was already pushed downstream)
    /// would corrupt the HEVC decoder's DPB if fed out-of-order
    /// because their P-frame references mutate state that subsequent
    /// frames depend on. We drop those silently here and bump the
    /// loss counter so the client triggers skip-until-IDR. By the
    /// time the next IDR arrives the chain repairs cleanly. Without
    /// this check, on-demand repair recovers the bytes but
    /// out-of-order delivery causes green/chroma flashes that look
    /// like loss even though FEC succeeded.
    video_highest_emitted: Option<u32>,
    audio_highest_emitted: Option<u32>,
    /// Count of datagrams rejected at the SymbolHeader boundary —
    /// either too short or with an unknown TrackId byte. Logged at
    /// powers-of-two so a steady stream of garbage shows up without
    /// flooding the log.
    malformed_datagrams: u64,
}

/// Move a block out of the pool, count it as lost (if not yet
/// decoded), record stats, bump the loss counter, and emit a
/// structured NACK with the block_id + k_received metadata. Pulled
/// out of `feed()` so both eviction paths (time-based + capacity)
/// share one implementation.
fn evict_block(
    pool: &mut HashMap<u32, BlockTracker>,
    stats: &mut AggregateStats,
    track_name: &str,
    block_id: u32,
    loss_counter: &Option<Arc<std::sync::atomic::AtomicU64>>,
    deficit_tx: &Option<mpsc::Sender<crate::protocol::DeficitMsg>>,
    track: TrackId,
    now: std::time::Instant,
) {
    let Some(t) = pool.remove(&block_id) else {
        return;
    };
    if t.decoded {
        // Block decoded fine — track late-symbol count as
        // wasted-bandwidth indicator and we're done.
        stats.record_late_symbols(t.received_after_decode);
        return;
    }
    stats.blocks_lost += 1;
    stats.record_loss(t.k, t.received_symbols);
    if let Some(c) = loss_counter {
        c.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
    }
    if let Some(tx) = deficit_tx {
        let deficit = crate::protocol::DeficitMsg {
            track,
            block_id,
            k_received: t.received_symbols,
            k_total: t.k,
        };
        // try_send so we never block the reassembly path on a
        // backed-up nack channel. If full, drop — the next eviction
        // will get its turn and the relay's repair pipeline will
        // catch up.
        let _ = tx.try_send(deficit);
    }
    tracing::warn!(
        track = track_name,
        block = block_id,
        k = t.k,
        received = t.received_symbols,
        short_by = t.k.saturating_sub(t.received_symbols),
        held_us = now.duration_since(t.first_symbol_at).as_micros() as u64,
        "fec.lost"
    );
}

/// One in-flight or just-decoded block. Wraps the raptorq BlockDecoder
/// with the bookkeeping we want for FEC visibility: how many symbols
/// arrived, when the block was first seen, whether we ever decoded it.
struct BlockTracker {
    decoder: BlockDecoder,
    /// Symbols received before (and including) decode. Stays frozen
    /// after decode so `received_after_decode` can grow independently.
    received_symbols: u32,
    /// Symbols arriving after decode is complete — i.e., late repair
    /// symbols we didn't need. High counts here hint that FEC
    /// redundancy is fatter than the network actually requires.
    received_after_decode: u32,
    /// K (source symbols required to decode). Set on first symbol via
    /// the SymbolHeader's `k` field.
    k: u32,
    /// Set true once add_symbol returns Some (we got enough to decode).
    decoded: bool,
    /// Wall-clock of the last NACK we fired for this block, plus the
    /// `received_symbols` count at the time we fired. Used by the
    /// continuous-deficit feedback loop: we re-NACK every
    /// `BLOCK_DEFICIT_TIMEOUT_MS` while the block is still short, but
    /// dampen if `received_symbols` has progressed since the last
    /// NACK (the server's response is in flight; piling on more
    /// NACKs would over-emit repair). On wifi this is the right
    /// shape — bursty loss means a single round of extra repair
    /// often isn't enough; the periodic re-NACK lets the server keep
    /// topping up until the deficit closes.
    last_deficit_at: Option<std::time::Instant>,
    last_deficit_k_received: u32,
    /// Wall-clock when first symbol of this block arrived. Used with
    /// decoded_at for per-block decode latency.
    first_symbol_at: std::time::Instant,
    /// Wall-clock when add_symbol first returned Some.
    decoded_at: Option<std::time::Instant>,
}

/// Rolling stats for one track over the last log window. Cumulative
/// counters live alongside; window-scoped distributions reset on each
/// `maybe_log` flush so percentiles reflect "right now" instead of
/// session-start drift.
#[derive(Default)]
struct AggregateStats {
    // ─── cumulative since session start ───
    blocks_seen: u64,
    blocks_decoded: u64,
    blocks_lost: u64,

    // ─── window-scoped, reset on each log ───
    /// Per-block decode latency in microseconds (first symbol → decoded).
    decode_latency_us: Vec<u64>,
    /// How many symbols past K it took to decode. 0 = decoded with
    /// exactly K, 5 = needed K+5. Distribution skewed right means
    /// FEC repair is being used; flat at 0 means the network is clean.
    extra_symbols_to_decode: Vec<u32>,
    /// For lost blocks: how many MORE symbols would have been needed
    /// to decode. Loss + this number tells u "we needed N more
    /// symbols than ever arrived"; tune redundancy by this.
    short_by: Vec<u32>,
    /// Late symbols per decoded block (after decode). Wasted-bandwidth
    /// indicator — high values across many blocks hint at over-provisioned
    /// redundancy.
    late_symbols_per_block: Vec<u32>,
    /// Tracks consecutive lost blocks within the window. Bursty wifi
    /// loss looks like a high `max_burst` here even if total loss_pct
    /// is small.
    current_burst: u32,
    max_burst_this_window: u32,
    /// Window ticker.
    last_log: Option<std::time::Instant>,
    blocks_since_last_log: u64,
}

impl AggregateStats {
    fn record_decode(&mut self, latency_us: u64, k: u32, received: u32) {
        self.decode_latency_us.push(latency_us);
        self.extra_symbols_to_decode.push(received.saturating_sub(k));
        // decoding ends a loss-burst.
        self.current_burst = 0;
    }

    fn record_loss(&mut self, k: u32, received: u32) {
        self.short_by.push(k.saturating_sub(received));
        self.current_burst = self.current_burst.saturating_add(1);
        if self.current_burst > self.max_burst_this_window {
            self.max_burst_this_window = self.current_burst;
        }
    }

    fn record_late_symbols(&mut self, n: u32) {
        if n > 0 {
            self.late_symbols_per_block.push(n);
        }
    }

    /// Log + reset window-scoped counters every 5s or every 300 blocks
    /// (whichever comes first). Cumulative counters keep going.
    fn maybe_log(&mut self, track: &str) {
        let now = std::time::Instant::now();
        let last = self.last_log.get_or_insert(now);
        let elapsed = now.duration_since(*last);
        if elapsed.as_secs() < 5 && self.blocks_since_last_log < 300 {
            return;
        }

        // Compute p50/p99/max for u64 distribution. Sort once; cheap
        // for window sizes <= 300.
        fn pcts_u64(xs: &mut [u64]) -> (u64, u64, u64, u64) {
            if xs.is_empty() {
                return (0, 0, 0, 0);
            }
            xs.sort_unstable();
            let n = xs.len();
            let p50 = xs[n / 2];
            let p99 = xs[(n * 99 / 100).min(n - 1)];
            let max = *xs.last().unwrap();
            let mean = (xs.iter().sum::<u64>()) / (n as u64);
            (p50, p99, max, mean)
        }
        fn pcts_u32(xs: &mut [u32]) -> (u32, u32, u32) {
            if xs.is_empty() {
                return (0, 0, 0);
            }
            xs.sort_unstable();
            let n = xs.len();
            (xs[n / 2], xs[(n * 99 / 100).min(n - 1)], *xs.last().unwrap())
        }

        let (lat_p50, lat_p99, lat_max, lat_mean) = pcts_u64(&mut self.decode_latency_us);
        let (extra_p50, extra_p99, extra_max) = pcts_u32(&mut self.extra_symbols_to_decode);
        let (short_p50, short_p99, short_max) = pcts_u32(&mut self.short_by);
        let (late_p50, late_p99, late_max) = pcts_u32(&mut self.late_symbols_per_block);

        let decode_pct = if self.blocks_seen > 0 {
            (self.blocks_decoded as f64) * 100.0 / (self.blocks_seen as f64)
        } else {
            0.0
        };
        let loss_pct = if self.blocks_seen > 0 {
            (self.blocks_lost as f64) * 100.0 / (self.blocks_seen as f64)
        } else {
            0.0
        };

        tracing::info!(
            track,
            // cumulative
            blocks_seen = self.blocks_seen,
            blocks_decoded = self.blocks_decoded,
            blocks_lost = self.blocks_lost,
            decode_pct = format!("{decode_pct:.2}%"),
            loss_pct = format!("{loss_pct:.2}%"),
            // decode latency (window)
            decode_lat_p50_us = lat_p50,
            decode_lat_p99_us = lat_p99,
            decode_lat_max_us = lat_max,
            decode_lat_mean_us = lat_mean,
            // FEC budget usage on decoded blocks (window): K+N where N>0
            // means the network needed FEC repair. p99>0 = bursty.
            extra_symbols_p50 = extra_p50,
            extra_symbols_p99 = extra_p99,
            extra_symbols_max = extra_max,
            // when blocks were LOST: how many MORE symbols would have
            // saved them. tune redundancy = mean(K) * (extra_p99 / mean(K))
            // approximately; safer bet is short_max + small margin.
            short_by_p50 = short_p50,
            short_by_p99 = short_p99,
            short_by_max = short_max,
            // late symbols arriving after decode = bandwidth waste signal
            late_symbols_p50 = late_p50,
            late_symbols_p99 = late_p99,
            late_symbols_max = late_max,
            // burstiness in window
            max_burst_lost_blocks = self.max_burst_this_window,
            "fec.stats"
        );

        // Reset window-scoped buckets.
        self.decode_latency_us.clear();
        self.extra_symbols_to_decode.clear();
        self.short_by.clear();
        self.late_symbols_per_block.clear();
        self.max_burst_this_window = 0;
        // current_burst intentionally NOT reset — it carries over so a
        // burst spanning two windows is still counted as one burst.

        *last = now;
        self.blocks_since_last_log = 0;
    }
}

impl ReassemblyState {
    fn feed(&mut self, datagram: &Bytes) -> Option<RecoveredFrame> {
        // Boundary: WebTransport datagram → narnia symbol stream.
        // The header is fixed-size and front-loaded; if a datagram is
        // shorter than the header or has an unknown TrackId byte we
        // dropped it silently before, which made datagram-format
        // skew invisible. Now we count rejections and warn periodically
        // so an upstream framing change shows up in logs instead of as
        // mysterious "no frames flowing".
        if datagram.len() < SymbolHeader::WIRE_LEN {
            self.malformed_datagrams = self.malformed_datagrams.saturating_add(1);
            if self.malformed_datagrams.is_power_of_two() {
                tracing::warn!(
                    count = self.malformed_datagrams,
                    dlen = datagram.len(),
                    wire_len = SymbolHeader::WIRE_LEN,
                    "datagram shorter than SymbolHeader; either non-narnia traffic on this port or a wire-format mismatch"
                );
            }
            return None;
        }
        let mut buf = datagram.clone();
        let header = match SymbolHeader::decode(&mut buf) {
            Some(h) => h,
            None => {
                self.malformed_datagrams = self.malformed_datagrams.saturating_add(1);
                if self.malformed_datagrams.is_power_of_two() {
                    tracing::warn!(
                        count = self.malformed_datagrams,
                        first_byte = datagram[0],
                        "SymbolHeader::decode rejected datagram (likely unknown TrackId byte)"
                    );
                }
                return None;
            }
        };
        let payload = buf; // remainder is the symbol body

        // Per-track external signal handles. Cloned out by ref before
        // we take a mutable borrow of the per-track pool/stats — the
        // borrow checker won't let us reach back into self once those
        // are borrowed mut.
        let loss_counter: Option<Arc<std::sync::atomic::AtomicU64>> = match header.track {
            TrackId::Video => self.video_loss_counter.clone(),
            TrackId::Audio => self.audio_loss_counter.clone(),
        };
        // NACK channel is video-only — losing audio doesn't need a
        // force-IDR; opus packets are independently decodable.
        let deficit_tx = if matches!(header.track, TrackId::Video) {
            self.deficit_tx.clone()
        } else {
            None
        };
        let (pool, stats, track_name) = match header.track {
            TrackId::Video => (&mut self.video, &mut self.video_stats, "video"),
            TrackId::Audio => (&mut self.audio, &mut self.audio_stats, "audio"),
        };

        let block_id = header.block;
        let now = std::time::Instant::now();
        // Was this block previously seen? If not, increment blocks_seen
        // and remember K + first-arrival timestamp for stats.
        let is_new_block = !pool.contains_key(&block_id);
        if is_new_block {
            stats.blocks_seen += 1;
            stats.blocks_since_last_log += 1;
        }
        let tracker = pool
            .entry(block_id)
            .or_insert_with(|| BlockTracker {
                decoder: BlockDecoder::new(header.size, block_id),
                received_symbols: 0,
                received_after_decode: 0,
                k: header.k,
                decoded: false,
                first_symbol_at: now,
                decoded_at: None,
                last_deficit_at: None,
                last_deficit_k_received: 0,
            });

        let was_decoded = tracker.decoded;
        if was_decoded {
            tracker.received_after_decode += 1;
        } else {
            tracker.received_symbols += 1;
        }
        let result = tracker.decoder.add_symbol(header.esi, &payload);

        let recovered = if !was_decoded {
            if let Some(bytes) = result {
                tracker.decoded = true;
                tracker.decoded_at = Some(now);
                stats.blocks_decoded += 1;
                let lat_us = now.duration_since(tracker.first_symbol_at).as_micros() as u64;
                stats.record_decode(lat_us, tracker.k, tracker.received_symbols);
                if matches!(header.track, TrackId::Video) {
                    tracing::info!(
                        track = track_name,
                        block = block_id,
                        k = tracker.k,
                        received = tracker.received_symbols,
                        margin_used = tracker.received_symbols.saturating_sub(tracker.k),
                        pump_dwell_us = lat_us,
                        used_repair = tracker.received_symbols > tracker.k,
                        "fec.dwell"
                    );
                }
                Some(Bytes::copy_from_slice(bytes))
            } else {
                None
            }
        } else {
            None
        };

        stats.maybe_log(track_name);

        // Bound memory: if more than MAX_INFLIGHT_BLOCKS blocks (decoded
        // Three-tier eviction:
        //
        //   1. Continuous-deficit NACK (T=30ms tick): every 30ms,
        //      every still-short block fires a NACK reporting its
        //      current `k_received`. The server's repair-emit logic
        //      sees a fresh deficit picture and tops up. We dampen
        //      re-fires when received_symbols has progressed since
        //      the last nack (the previous repair burst is still
        //      arriving), but otherwise keep nudging the server
        //      until the deficit closes — wifi loss is bursty
        //      enough that one repair round usually isn't enough.
        //      CRITICAL: keep the BlockDecoder alive so the repair
        //      symbols can land into the same decoder context that
        //      already has K/2-ish symbols.
        //
        //   2. Hard eviction (T=130ms): block STILL hasn't decoded
        //      after a full nack round-trip + repair window → free
        //      the decoder, count as loss. This is what "blocks_lost"
        //      counts.
        //
        //   3. Capacity bound: defensive ceiling against unbounded
        //      memory growth.
        //
        // The two-phase split is the difference between "we lose
        // 0.4% of blocks" (current) and "we lose ~0% of blocks but
        // recovery adds ~30ms" (target). Without it, on-demand repair
        // symbols arrive at a destroyed decoder and contribute
        // nothing.
        let deficit_threshold = std::time::Duration::from_millis(BLOCK_DEFICIT_TIMEOUT_MS);
        let evict_threshold = std::time::Duration::from_millis(BLOCK_EVICTION_TIMEOUT_MS);

        // Phase 1: continuous-deficit NACK. Fires the FIRST nack at
        // `deficit_threshold` after the first symbol arrives, then
        // RE-FIRES every `deficit_threshold` while the block is still
        // short — UNLESS received_symbols has progressed since the
        // last nack, in which case the server's previous repair burst
        // is still arriving and we don't want to pile on more. Wifi
        // bursty loss often needs multiple repair rounds, so the
        // re-fire is what makes the FEC actually adapt; the dampening
        // is what keeps us from over-requesting.
        let deficit_ready: Vec<u32> = pool
            .iter()
            .filter_map(|(id, t)| {
                if t.decoded || t.received_symbols >= t.k {
                    return None;
                }
                let due = match t.last_deficit_at {
                    None => now.duration_since(t.first_symbol_at) >= deficit_threshold,
                    Some(last) => {
                        let stale = now.duration_since(last) >= deficit_threshold;
                        let no_progress = t.received_symbols <= t.last_deficit_k_received;
                        stale && no_progress
                    }
                };
                if due { Some(*id) } else { None }
            })
            .collect();
        for id in deficit_ready {
            if let Some(t) = pool.get_mut(&id) {
                if let Some(tx) = &deficit_tx {
                    let deficit = crate::protocol::DeficitMsg {
                        track: header.track,
                        block_id: id,
                        k_received: t.received_symbols,
                        k_total: t.k,
                    };
                    let _ = tx.try_send(deficit);
                }
                let is_refire = t.last_deficit_at.is_some();
                t.last_deficit_at = Some(now);
                t.last_deficit_k_received = t.received_symbols;
                tracing::debug!(
                    track = track_name,
                    block = id,
                    k = t.k,
                    received = t.received_symbols,
                    short_by = t.k.saturating_sub(t.received_symbols),
                    held_us = now.duration_since(t.first_symbol_at).as_micros() as u64,
                    refire = is_refire,
                    "fec.nack_sent"
                );
            }
        }

        // Phase 2: hard-evict blocks past the eviction threshold.
        let timed_out: Vec<u32> = pool
            .iter()
            .filter_map(|(id, t)| {
                if !t.decoded && now.duration_since(t.first_symbol_at) >= evict_threshold {
                    Some(*id)
                } else {
                    None
                }
            })
            .collect();
        for id in timed_out {
            // evict_block also fires a final NACK as a safety net,
            // even if one was already sent. The relay's bounded
            // channel coalesces.
            evict_block(
                pool,
                stats,
                track_name,
                id,
                &loss_counter,
                &deficit_tx,
                header.track,
                now,
            );
        }
        // Defensive capacity bound — should never fire under
        // time-based eviction, but kept as a memory safety net.
        if pool.len() > MAX_INFLIGHT_BLOCKS {
            if let Some(min) = pool.keys().min().copied() {
                evict_block(
                    pool,
                    stats,
                    track_name,
                    min,
                    &loss_counter,
                    &deficit_tx,
                    header.track,
                    now,
                );
            }
        }

        // Out-of-order guard: only emit if this block_id is strictly
        // higher than the last one we passed downstream for this
        // track. Late on-demand-repair recoveries arrive AFTER newer
        // blocks have already been emitted to the application; if we
        // forwarded them, the application's HEVC decoder would feed
        // them as P-frames against a DPB that's already moved on,
        // producing chroma corruption (green flashes). Drop silently
        // and bump the loss counter so the renderer can skip-until-IDR
        // to repair the chain. The client still LOST the visual
        // content for this block — the on-demand repair was wasted —
        // but the chain stays consistent and visual corruption is
        // bounded by GOP interval (~167ms with our current settings).
        let highest = match header.track {
            TrackId::Video => &mut self.video_highest_emitted,
            TrackId::Audio => &mut self.audio_highest_emitted,
        };
        // Pass-through: emit every recovered frame regardless of
        // block_id ordering. Out-of-order arrivals (late on-demand
        // repair landing after newer blocks decoded) are handled by
        // the application's reorder buffer downstream — see
        // `pixelflux_client::display::pump_and_render`'s ADC/QADC-G
        // playout buffer. Dropping late blocks here would defeat the
        // playout buffer's reordering, causing visible gaps in the
        // block_id sequence delivered to the application.
        //
        // We still track `highest` purely for the gap detector —
        // emitting an ADVANCED block_id (header.block > prev + 1)
        // means at least one block was never recovered, which we
        // surface to the application via loss_counter so it can
        // skip-until-IDR even if the missing block never arrives at
        // all. Late arrivals (header.block <= prev) are NOT a
        // permanent loss; they're a reordering and the playout
        // buffer rearranges them.
        if let Some(payload) = recovered {
            if let Some(prev) = *highest {
                if header.block > prev + 1 {
                    let gap = header.block - prev - 1;
                    tracing::debug!(
                        track = track_name,
                        block = header.block,
                        prev_emitted = prev,
                        gap,
                        "fec.gap_detected"
                    );
                    if let Some(c) = &loss_counter {
                        c.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
                    }
                }
                // Track the max so we still know the top of the
                // window for stats; don't gate emission on it.
                if header.block > prev {
                    *highest = Some(header.block);
                }
            } else {
                *highest = Some(header.block);
            }
            return Some(RecoveredFrame {
                track: header.track,
                block: header.block,
                payload,
            });
        }
        None
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::fec::BlockEncoder;
    use crate::protocol::SymbolHeader;
    use bytes::BytesMut;

    /// Wrap one BlockEncoder packet in a SymbolHeader + body datagram.
    fn make_datagram(
        track: TrackId,
        block: u32,
        k: u32,
        size: u32,
        esi: u32,
        body: &[u8],
    ) -> Bytes {
        let mut buf = BytesMut::with_capacity(SymbolHeader::WIRE_LEN + body.len());
        SymbolHeader { track, esi, block, k, size }.encode(&mut buf);
        buf.extend_from_slice(body);
        buf.freeze()
    }

    /// Generate ALL encoded packets for a source block (k + r).
    fn encode_block(source: &[u8], block: u32, redundancy: f32) -> Vec<(SymbolHeader, Vec<u8>)> {
        let mut enc = BlockEncoder::new(source, block, redundancy);
        let k = enc.k();
        let size = enc.size;
        let mut out = Vec::new();
        while let Some(packet) = enc.next_packet() {
            let payload_id = packet.payload_id();
            let esi = payload_id.encoding_symbol_id();
            let body = packet.data().to_vec();
            out.push((
                SymbolHeader { track: TrackId::Video, esi, block, k, size },
                body,
            ));
        }
        out
    }

    #[test]
    fn single_symbol_block_emits_one_frame() {
        // Source small enough to fit in a single RaptorQ symbol — K=1.
        let source = b"hello world".to_vec();
        let mut state = ReassemblyState::default();

        let packets = encode_block(&source, 0, 0.20);
        // K=1 gets one source symbol and the small-frame repair floor.
        assert_eq!(packets.len(), 3);

        // First packet should decode immediately and emit the frame.
        let dgram = make_datagram(
            packets[0].0.track,
            packets[0].0.block,
            packets[0].0.k,
            packets[0].0.size,
            packets[0].0.esi,
            &packets[0].1,
        );
        let recovered = state.feed(&dgram).expect("first symbol should decode");
        assert_eq!(recovered.payload.as_ref(), source.as_slice());
        assert_eq!(recovered.block, 0);
    }

    #[test]
    fn redundancy_symbol_after_decode_does_not_emit_duplicate() {
        // The exact bug from 2026-05-02: K=1 frames triggered a second
        // RecoveredFrame on the redundancy symbol because the decoder was
        // dropped from the pool on first decode and re-created on second.
        let source = b"packet-once-only".to_vec();
        let mut state = ReassemblyState::default();

        let packets = encode_block(&source, 7, 0.20);
        assert!(packets.len() >= 2, "need at least one repair packet");

        let first = make_datagram(TrackId::Video, 7, packets[0].0.k, packets[0].0.size,
            packets[0].0.esi, &packets[0].1);
        let second = make_datagram(TrackId::Video, 7, packets[1].0.k, packets[1].0.size,
            packets[1].0.esi, &packets[1].1);

        // First packet decodes, emits.
        assert!(state.feed(&first).is_some());
        // Second (redundancy) packet must NOT emit again. Pre-fix this
        // returned Some(...).
        assert!(
            state.feed(&second).is_none(),
            "repair symbol after successful decode must not emit a second frame"
        );
    }

    #[test]
    fn multi_symbol_block_emits_only_after_threshold() {
        // Build a block big enough to require K >= 4 source symbols.
        let source: Vec<u8> = (0u8..255).cycle().take(8 * 1024).collect();
        let mut state = ReassemblyState::default();

        let packets = encode_block(&source, 1, 0.20);
        let k = packets[0].0.k;
        assert!(k >= 4, "fixture should require multiple source symbols, got k={k}");

        // First k-1 packets must NOT decode (insufficient symbols).
        for p in packets.iter().take((k - 1) as usize) {
            let dgram = make_datagram(TrackId::Video, 1, p.0.k, p.0.size, p.0.esi, &p.1);
            assert!(state.feed(&dgram).is_none(), "decoder shouldn't emit before K");
        }

        // The k-th packet completes the block and emits.
        let p = &packets[(k - 1) as usize];
        let dgram = make_datagram(TrackId::Video, 1, p.0.k, p.0.size, p.0.esi, &p.1);
        let recovered = state.feed(&dgram).expect("Kth symbol should complete decode");
        assert_eq!(recovered.payload.as_ref(), source.as_slice());

        // Any further packets (repair symbols) must not emit again.
        for p in packets.iter().skip(k as usize) {
            let dgram = make_datagram(TrackId::Video, 1, p.0.k, p.0.size, p.0.esi, &p.1);
            assert!(state.feed(&dgram).is_none(), "repair after decode must not duplicate");
        }
    }

    #[test]
    fn out_of_order_symbols_decode_correctly() {
        // Some symbols arrive in reverse order. RaptorQ doesn't care about
        // order — any K of K+R works.
        let source: Vec<u8> = (0u8..=255).cycle().take(4 * 1024).collect();
        let mut state = ReassemblyState::default();
        let mut packets = encode_block(&source, 9, 0.30);
        packets.reverse();

        let mut recovered: Option<RecoveredFrame> = None;
        for p in &packets {
            let dgram = make_datagram(TrackId::Video, 9, p.0.k, p.0.size, p.0.esi, &p.1);
            if let Some(frame) = state.feed(&dgram) {
                assert!(recovered.is_none(), "should emit exactly once");
                recovered = Some(frame);
            }
        }
        let recovered = recovered.expect("should have decoded");
        assert_eq!(recovered.payload.as_ref(), source.as_slice());
    }

    #[test]
    fn two_concurrent_blocks_each_emit_once() {
        // Interleave symbols from two different blocks. Each block
        // independently decodes; neither leaks into the other.
        let source_a: Vec<u8> = b"AAAAAAAAAAAA".to_vec();
        let source_b: Vec<u8> = b"BBBBBBBBBBBB".to_vec();
        let mut state = ReassemblyState::default();
        let pa = encode_block(&source_a, 1, 0.20);
        let pb = encode_block(&source_b, 2, 0.20);

        let mut got_a = false;
        let mut got_b = false;

        // Interleave: a[0], b[0], a[1], b[1], ...
        for i in 0..pa.len().max(pb.len()) {
            if let Some(p) = pa.get(i) {
                let d = make_datagram(TrackId::Video, 1, p.0.k, p.0.size, p.0.esi, &p.1);
                if let Some(f) = state.feed(&d) {
                    assert_eq!(f.payload.as_ref(), source_a.as_slice());
                    assert!(!got_a, "block 1 emitted twice");
                    got_a = true;
                }
            }
            if let Some(p) = pb.get(i) {
                let d = make_datagram(TrackId::Video, 2, p.0.k, p.0.size, p.0.esi, &p.1);
                if let Some(f) = state.feed(&d) {
                    assert_eq!(f.payload.as_ref(), source_b.as_slice());
                    assert!(!got_b, "block 2 emitted twice");
                    got_b = true;
                }
            }
        }
        assert!(got_a && got_b, "both blocks should decode");
    }

    #[test]
    fn video_and_audio_tracks_route_independently() {
        // Same block id 0 on both tracks — pool isolation should keep
        // them separate so neither leaks into the other.
        let source_v: Vec<u8> = b"VIDEO_FRAME_ZERO".to_vec();
        let source_a: Vec<u8> = b"AUDIO_PACKET_ZERO".to_vec();
        let mut state = ReassemblyState::default();
        let pv = encode_block(&source_v, 0, 0.20);
        let pa = encode_block(&source_a, 0, 0.20);

        let dv = make_datagram(TrackId::Video, 0, pv[0].0.k, pv[0].0.size,
            pv[0].0.esi, &pv[0].1);
        let recovered_v = state.feed(&dv).expect("video");
        assert_eq!(recovered_v.track, TrackId::Video);
        assert_eq!(recovered_v.payload.as_ref(), source_v.as_slice());

        // Audio override the same block id but on a different track.
        let mut audio_hdr = pa[0].0;
        audio_hdr.track = TrackId::Audio;
        let da = make_datagram(TrackId::Audio, 0, audio_hdr.k, audio_hdr.size,
            audio_hdr.esi, &pa[0].1);
        let recovered_a = state.feed(&da).expect("audio");
        assert_eq!(recovered_a.track, TrackId::Audio);
        assert_eq!(recovered_a.payload.as_ref(), source_a.as_slice());
    }
}

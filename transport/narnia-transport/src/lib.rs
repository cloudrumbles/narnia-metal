//! narnia-transport — QUIC datagram + RaptorQ FEC for cloud gaming.
//!
//! # Architecture
//!
//! The server publishes encoded video frames (HEVC NALs) and audio packets
//! (opus) by splitting each into RaptorQ symbols and emitting each symbol
//! as one QUIC datagram. The client reassembles symbols into source blocks,
//! recovers the original encoded frame from any K of K+R received symbols,
//! and feeds the result to the codec layer.
//!
//! ## Why datagrams, not streams
//!
//! For cloud gaming a stale frame is worse than a missing one. QUIC streams
//! retransmit on loss, blocking newer frames behind older ones — fine for
//! recorded video, fatal for interactive use. Datagrams skip retransmit
//! entirely; RaptorQ handles loss without retransmit by encoding redundancy
//! into every transmission.
//!
//! ## Transport choice
//!
//! Built on [`web_transport_quinn`], the same WebTransport-over-quinn crate
//! moq-native uses. This means TLS, ALPN, certificate handling, and quinn
//! endpoint configuration are all "the moq way," but we bypass moq-lite's
//! group/frame stream-based protocol because it doesn't fit our latency
//! requirements. Future browser clients can use chrome's WebTransport API
//! to speak the same protocol with no bridge code.
//!
//! ## Wire layout
//!
//! Per session:
//! - **bidi stream 0** (control): hello message + track metadata + later
//!   bitrate-control acks. ASCII-ish, line-delimited for now.
//! - **datagrams**: each datagram carries one RaptorQ symbol with a small
//!   header identifying which track + source block + symbol index it
//!   belongs to. The client groups by (track, source_block_id), feeds the
//!   group to a `raptorq::Decoder`, and emits the original payload when
//!   the decoder has K' >= K symbols.
//!
//! ## Crate layout
//!
//! - [`server`] — accepts connections, mints self-signed certs, exposes
//!   `Publisher` handles for video / audio tracks.
//! - [`client`] — connects to a server, exposes a stream of decoded
//!   payloads per track.
//! - [`protocol`] — wire-format types shared between server and client.
//! - [`fec`] — RaptorQ encoder + decoder wrappers with sane defaults for
//!   1080p60 cloud gaming.

// Pure modules: no sockets, no tokio, no clock reads. These compile for
// wasm32-unknown-unknown and are shared with the browser client.
pub mod alpn;
pub mod fec;
pub mod playout;
pub mod protocol;
pub mod rate;
pub mod ring;

// Native-only modules (quinn / tokio / rustls).
#[cfg(feature = "native")]
pub mod cc_stats;
#[cfg(feature = "native")]
pub mod cert;
#[cfg(feature = "native")]
pub mod client;
#[cfg(feature = "native")]
pub mod server;
#[cfg(feature = "native")]
pub mod socket_tuning;
pub mod pointer;

#[cfg(feature = "native")]
pub use cc_stats::CcStats;
#[cfg(feature = "native")]
pub use client::{Client, InputPublisher};
pub use playout::{PlayoutBuffer, PlayoutConfig, PlayoutFrame, PlayoutStats};
pub use protocol::{Hello, InputWireEvent, PingMsg, PongMsg, SymbolHeader, TrackId};
pub use rate::{LinkSample, RateConfig, RateController, RateDecision, RateEvent};
#[cfg(feature = "native")]
pub use server::{Publisher, Server, SessionHandle};

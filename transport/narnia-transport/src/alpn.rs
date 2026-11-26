//! Narnia protocol-version identifiers.
//!
//! WebTransport-over-QUIC mandates that the TLS ALPN be `"h3"` — that
//! negotiation is owned by the HTTP/3 layer and is not something we can
//! repurpose for narnia-level versioning. The constants here serve two
//! purposes:
//!
//! 1. **Telemetry / forensic logs** — `ALPN_CURRENT` is logged at session
//!    start alongside the `protocol_version` u32 field from [`Hello`], giving
//!    ops a human-readable version label without having to decode a hex int.
//!
//! 2. **Forward allocation** — `ALPN_V2` is named now so client and server
//!    land together when Phase C upgrades the wire format. When that time
//!    comes, move `ALPN_CURRENT` to point at `ALPN_V2` and update both
//!    endpoints in the same commit.
//!
//! [`Hello`]: crate::protocol::Hello

/// Phase A wire format — the format that has shipped historically.
/// Bump to ALPN_V2 when the wire format breaks in Phase C.
pub const ALPN_V1: &[u8] = b"narnia/v1";

/// Phase C wire format. Allocated now so client and server can land
/// together with the protocol revision; kept inert until then.
pub const ALPN_V2: &[u8] = b"narnia/v2";

/// The narnia protocol identifier for the current wire format.
/// Logged at session start for telemetry. Not used as the TLS ALPN
/// (which must remain `web_transport_quinn::ALPN` = `"h3"` for the
/// WebTransport handshake to succeed).
pub const ALPN_CURRENT: &[u8] = ALPN_V1;

//! Congestion-control stats snapshot, populated from quinn's
//! [`quinn::ConnectionStats`] and exposed on both [`crate::server::Publisher`]
//! and [`crate::client::Subscription`] for adaptive-bitrate decisions.

use quinn::Connection as QuinnConnection;

/// A point-in-time snapshot of the CC / path statistics for one QUIC
/// connection. Cheap to read — `quinn::Connection::stats()` is a lock-free
/// copy of an atomically-updated counter struct inside quinn-proto.
#[derive(Debug, Clone)]
pub struct CcStats {
    /// CC algorithm configured at endpoint build time. Quinn 0.11 does not
    /// expose the runtime CC name via `ConnectionStats`, so we mirror the
    /// `CongestionControl::LowLatency` variant we set in the builders.
    pub cc_algorithm: String,

    /// Delivery-rate estimate in bytes per second. Quinn 0.11 / quinn-proto
    /// 0.11 do not expose a direct bandwidth estimator field. We approximate
    /// this as `cwnd_bytes * 1_000_000 / smoothed_rtt_us` (i.e. cwnd / RTT).
    /// Returns 0 when smoothed_rtt_us is 0 (before the first ACK round).
    pub delivery_rate_bytes_per_sec: u64,

    /// Current congestion window in bytes.
    pub cwnd_bytes: u64,

    /// Bytes currently in-flight (sent but not yet ACKed). Quinn-proto 0.11
    /// `PathStats` does not expose a direct bytes_in_flight field; this is
    /// always 0 until a future quinn version adds it.
    pub bytes_in_flight: u64,

    /// Smoothed RTT in microseconds (`PathStats::rtt`, converted from
    /// `std::time::Duration`). 0 before the first ACK round-trip completes.
    pub smoothed_rtt_us: u64,

    /// Total lost packets across the connection lifetime (`PathStats::lost_packets`).
    pub lost_packets: u64,
}

/// Snapshot CC stats from an underlying `quinn::Connection`.
///
/// Called by the `cc_stats()` methods on `Publisher` and `Subscription`.
/// Kept as a free function so both sides share identical extraction logic
/// without duplicating it.
pub(crate) fn snapshot(conn: &QuinnConnection) -> CcStats {
    let s = conn.stats();
    let path = s.path;

    let smoothed_rtt_us = path.rtt.as_micros() as u64;
    let cwnd_bytes = path.cwnd;
    let delivery_rate_bytes_per_sec = if smoothed_rtt_us > 0 {
        cwnd_bytes.saturating_mul(1_000_000) / smoothed_rtt_us
    } else {
        0
    };

    CcStats {
        cc_algorithm: "LowLatency".to_owned(),
        delivery_rate_bytes_per_sec,
        cwnd_bytes,
        bytes_in_flight: 0,
        smoothed_rtt_us,
        lost_packets: path.lost_packets,
    }
}

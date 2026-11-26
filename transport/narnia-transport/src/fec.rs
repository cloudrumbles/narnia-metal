//! RaptorQ encode/decode wrappers tuned for cloud gaming.
//!
//! Each encoded video frame is a "source object" that we feed to a
//! `raptorq::SourceBlockEncoder`. The encoder partitions the object into
//! K equal-sized "source symbols" of `SYMBOL_SIZE` bytes, then generates
//! K + R total encoded symbols (the extra R are the redundancy). On the
//! receiving side, the client collects symbols until it has at least K
//! valid ones (any K of the K+R will do) and reconstructs the original
//! object.
//!
//! ## Why these constants
//!
//! - `SYMBOL_SIZE = 1024`: fits one symbol + 17-byte [`SymbolHeader`] +
//!   QUIC datagram framing comfortably under wireguard's 1280-byte MTU.
//!   We deliberately undershoot (~200 bytes of headroom) because PMTU
//!   discovery on tailnet links is unreliable and a single
//!   too-large datagram would be silently dropped by an intermediate hop.
//! - `REDUNDANCY = 0.30` (30%): gives a wider margin for bursty wifi and
//!   tunnel jitter. Small blocks also get a two-symbol repair floor because
//!   a one-symbol P-frame otherwise loses the whole frame on one dropped
//!   datagram.
//!
//! Both constants are knobs we'll measure and tune on real network traces;
//! exposing them in a future `Config` struct is straightforward.

use raptorq::{
    EncodingPacket, ObjectTransmissionInformation, PayloadId, SourceBlockDecoder,
    SourceBlockEncoder,
};

/// Symbol size (bytes) — the unit of FEC and the body of each datagram.
pub const SYMBOL_SIZE: u16 = 1024;

/// Default target redundancy ratio. Repair symbols emitted is based on
/// K * REDUNDANCY, with a small-frame floor from [`repair_symbol_count`].
/// 0.5 = ~50% extra symbols on top of source. Calibrated for cachyos
/// home-wifi profile + relay-to-client metro RTT, with on-demand
/// repair as the second line of defense for any blocks that still
/// decode short.
pub const DEFAULT_REDUNDANCY: f32 = 0.5;

/// Minimum repair symbols for tiny video/audio blocks. K=1 is common for
/// low-motion P-frames, and one dropped datagram should not erase the frame.
pub const MIN_REPAIR_SYMBOLS: u32 = 2;

/// Maximum repair symbols for one source block. This keeps large keyframes
/// from creating a huge datagram burst when the ratio is high.
pub const MAX_REPAIR_SYMBOLS: u32 = 96;

/// Encoder for a single source block (one encoded frame).
///
/// Workflow:
///   1. construct with the source bytes,
///   2. drain `next_packet()` for K + R packets,
///   3. drop.
pub struct BlockEncoder {
    inner: SourceBlockEncoder,
    /// Number of source symbols.
    pub k: u32,
    /// How many encoded packets we've emitted from this block so far.
    emitted: u32,
    /// K + R: total packets we'll emit before stopping.
    target: u32,
    /// Source object byte length, sent in the symbol header.
    pub size: u32,
}

impl BlockEncoder {
    pub fn new(source: &[u8], block_id: u32, redundancy: f32) -> Self {
        // SourceBlockEncoder::new requires data to be an exact multiple of
        // symbol_size — RaptorQ doesn't pad internally. Round up by zero-
        // filling; the receiver knows the true length from `size` in the
        // SymbolHeader and slices padding off after decode.
        let symbol_size = SYMBOL_SIZE as usize;
        let k = symbol_count(source.len(), SYMBOL_SIZE);
        let padded_len = (k as usize) * symbol_size;
        let mut padded = Vec::with_capacity(padded_len);
        padded.extend_from_slice(source);
        padded.resize(padded_len, 0);

        // OTI matches the *padded* length; we record the real size separately
        // so the decoder reconstructs against the same OTI and the slice off
        // happens at the application layer.
        let oti = ObjectTransmissionInformation::with_defaults(padded_len as u64, SYMBOL_SIZE);
        let inner = SourceBlockEncoder::new(block_id as u8, &oti, &padded);
        let target = k + repair_symbol_count(k, redundancy);
        Self {
            inner,
            k,
            emitted: 0,
            target,
            size: source.len() as u32,
        }
    }

    /// Returns the next encoding packet, or `None` once we've emitted K+R.
    pub fn next_packet(&mut self) -> Option<EncodingPacket> {
        if self.emitted >= self.target {
            return None;
        }
        let packets = self.inner.repair_packets(self.emitted, 1);
        let packet = packets.into_iter().next()?;
        self.emitted += 1;
        Some(packet)
    }

    pub fn k(&self) -> u32 {
        self.k
    }

    pub fn target(&self) -> u32 {
        self.target
    }

    /// Generate `count` additional repair symbols beyond what
    /// `next_packet` has already emitted. Continues the ESI sequence
    /// (so the symbols don't collide with previously-sent ones), and
    /// advances `self.emitted` past them so subsequent calls keep
    /// minting fresh ESIs.
    ///
    /// Used by relays to fulfill on-demand repair requests:
    /// keep a `BlockEncoder` alive in a ring buffer, call
    /// `extra_packets(n)` whenever a downstream client signals loss,
    /// emit ONLY the new repair symbols (not the original source).
    /// Bandwidth-efficient compared to re-encoding the whole block,
    /// and lets the client's decoder accept these as additional
    /// inputs against its existing in-flight `BlockDecoder` for that
    /// block_id.
    pub fn extra_packets(&mut self, count: u32) -> Vec<EncodingPacket> {
        let packets = self.inner.repair_packets(self.emitted, count);
        self.emitted += packets.len() as u32;
        packets
    }
}

/// Decoder for a single source block. Fed one symbol at a time; returns
/// `Some(bytes)` once enough symbols have arrived to reconstruct.
pub struct BlockDecoder {
    inner: SourceBlockDecoder,
    /// Set once we recover the source; subsequent `add_symbol` calls
    /// short-circuit so late-arriving symbols don't waste work.
    /// Already trimmed to `real_size` (padding stripped).
    decoded: Option<Vec<u8>>,
    /// True application length; padded length lives in the OTI.
    real_size: usize,
    /// raptorq's PayloadId requires us to thread the block id through to
    /// the EncodingPacket — the receiver has it in the SymbolHeader.
    block_id: u8,
}

impl BlockDecoder {
    pub fn new(size: u32, block_id: u32) -> Self {
        // Match the encoder's padded OTI: round size up to the next
        // multiple of SYMBOL_SIZE. The application layer slices the
        // padding off after decode.
        let symbol_size = SYMBOL_SIZE as usize;
        let k = symbol_count(size as usize, SYMBOL_SIZE) as usize;
        let padded_len = k * symbol_size;
        let oti = ObjectTransmissionInformation::with_defaults(padded_len as u64, SYMBOL_SIZE);
        let inner = SourceBlockDecoder::new(block_id as u8, &oti, padded_len as u64);
        Self {
            inner,
            decoded: None,
            real_size: size as usize,
            block_id: block_id as u8,
        }
    }

    /// Add one received encoding symbol. Returns the recovered source
    /// object the first time enough symbols are present, `None` otherwise.
    pub fn add_symbol(&mut self, esi: u32, payload: &[u8]) -> Option<&[u8]> {
        if self.decoded.is_some() {
            return self.decoded.as_deref();
        }
        let packet = EncodingPacket::new(
            PayloadId::new(self.block_id, esi),
            payload.to_vec(),
        );
        if let Some(mut out) = self.inner.decode([packet]) {
            out.truncate(self.real_size);
            self.decoded = Some(out);
        }
        self.decoded.as_deref()
    }

    pub fn is_done(&self) -> bool {
        self.decoded.is_some()
    }
}

/// Number of source symbols required to cover a `len`-byte object at
/// `symbol_size` symbols. Rounds up — the last symbol is padded with zeros
/// inside RaptorQ.
pub fn symbol_count(len: usize, symbol_size: u16) -> u32 {
    let s = symbol_size as usize;
    ((len + s - 1) / s) as u32
}

pub fn repair_symbol_count(k: u32, redundancy: f32) -> u32 {
    if k == 0 {
        return 0;
    }
    let ratio = redundancy.clamp(0.0, 1.0);
    let repair = ((k as f32) * ratio).ceil() as u32;
    repair.max(MIN_REPAIR_SYMBOLS).min(MAX_REPAIR_SYMBOLS)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn encode_decode_roundtrip_no_loss() {
        let source: Vec<u8> = (0..4096u32).flat_map(|i| (i as u32).to_le_bytes()).collect();
        let mut enc = BlockEncoder::new(&source, 0, 0.20);
        let mut dec = BlockDecoder::new(source.len() as u32, 0);

        // Drain all packets, feed in order.
        let mut recovered = None;
        for _ in 0..enc.target() {
            if let Some(p) = enc.next_packet() {
                let payload = p.data();
                let id = p.payload_id();
                if let Some(out) = dec.add_symbol(id.encoding_symbol_id(), payload) {
                    recovered = Some(out.to_vec());
                    break;
                }
            }
        }
        assert!(recovered.is_some());
        assert_eq!(recovered.unwrap(), source);
    }

    #[test]
    fn encode_decode_with_20pct_loss() {
        // Drop every 5th packet (~20% loss). With 20% redundancy, decode
        // should still succeed (we have exactly K good packets).
        let source: Vec<u8> = (0u8..=255).cycle().take(20_000).collect();
        let mut enc = BlockEncoder::new(&source, 1, 0.30);
        let mut dec = BlockDecoder::new(source.len() as u32, 1);

        let mut idx = 0u32;
        let mut recovered = None;
        while let Some(p) = enc.next_packet() {
            idx += 1;
            if idx % 5 == 0 {
                continue; // simulate drop
            }
            if let Some(out) = dec.add_symbol(
                p.payload_id().encoding_symbol_id(),
                p.data(),
            ) {
                recovered = Some(out.to_vec());
                break;
            }
        }
        assert!(recovered.is_some(), "decode failed with 20% loss + 30% redundancy");
        assert_eq!(recovered.unwrap(), source);
    }

    #[test]
    fn symbol_count_rounds_up() {
        assert_eq!(symbol_count(0, 1024), 0);
        assert_eq!(symbol_count(1, 1024), 1);
        assert_eq!(symbol_count(1023, 1024), 1);
        assert_eq!(symbol_count(1024, 1024), 1);
        assert_eq!(symbol_count(1025, 1024), 2);
    }

    #[test]
    fn repair_policy_has_small_frame_floor() {
        assert_eq!(repair_symbol_count(0, 0.30), 0);
        assert_eq!(repair_symbol_count(1, 0.30), 2);
        assert_eq!(repair_symbol_count(4, 0.30), 2);
        assert_eq!(repair_symbol_count(10, 0.30), 3);
    }

    #[test]
    fn repair_policy_caps_large_frame_bursts() {
        assert_eq!(repair_symbol_count(500, 1.0), MAX_REPAIR_SYMBOLS);
    }

    #[test]
    fn extra_packets_continues_esi_sequence() {
        // After draining K+R packets, calling `extra_packets(n)` mints
        // n more packets at fresh ESIs, and the receiver decodes against
        // the combined set as if the encoder had just emitted more.
        let source: Vec<u8> = (0u8..=255).cycle().take(8_000).collect();
        let mut enc = BlockEncoder::new(&source, 11, 0.10); // K + R, R small
        let mut dec = BlockDecoder::new(source.len() as u32, 11);

        // Drop EVERY packet from the initial K+R emission to simulate
        // total wipeout on the original transmission. Decoder gets
        // nothing.
        let original_target = enc.target();
        for _ in 0..original_target {
            // Pull and discard.
            assert!(enc.next_packet().is_some());
        }
        assert!(!dec.is_done(), "no decode yet — discarded everything");

        // Now mint extra repair packets. Receiver should be able to
        // decode from these alone, since RaptorQ packets at any ESI
        // are decode-equivalent.
        let extras = enc.extra_packets(enc.k() + 8);
        let mut recovered: Option<Vec<u8>> = None;
        for p in extras {
            if let Some(out) = dec.add_symbol(p.payload_id().encoding_symbol_id(), p.data()) {
                recovered = Some(out.to_vec());
                break;
            }
        }
        assert!(
            recovered.is_some(),
            "extra repair symbols should let the receiver decode the block from scratch"
        );
        assert_eq!(recovered.unwrap(), source);
    }

    #[test]
    fn extra_packets_advances_emitted_counter() {
        let source = vec![0xab; 4096];
        let mut enc = BlockEncoder::new(&source, 12, 0.30);
        let target = enc.target();
        // Consume all K+R first.
        for _ in 0..target {
            enc.next_packet();
        }
        let before = enc.emitted;
        let pkts = enc.extra_packets(5);
        assert_eq!(pkts.len(), 5);
        assert_eq!(enc.emitted, before + 5);
        // ESIs should be distinct from the original sequence.
        let original_max_esi = target - 1;
        for p in &pkts {
            let esi = p.payload_id().encoding_symbol_id();
            assert!(
                esi > original_max_esi,
                "extra ESI {esi} should be past original max {original_max_esi}"
            );
        }
    }

    #[test]
    fn large_frame_can_emit_symbol_ids_above_one_byte() {
        let source = vec![0x5a; (260 * SYMBOL_SIZE as usize) + 17];
        let mut enc = BlockEncoder::new(&source, 9, 0.30);
        let mut highest_esi = 0;

        while let Some(packet) = enc.next_packet() {
            highest_esi = highest_esi.max(packet.payload_id().encoding_symbol_id());
        }

        assert!(highest_esi > u8::MAX as u32);
    }
}

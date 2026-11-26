//! Fixed-capacity ring buffer of recently-published source blocks, keyed
//! by block id. Used by the relay to replay recent frames to a downstream
//! client when that client signals loss.
//!
//! ## Why a ring (not a HashMap or a VecDeque)
//!
//! - HashMap<u32, Bytes> would work but allocates on every insert and we
//!   want zero allocator churn at 60Hz per track.
//! - VecDeque<Bytes> with len-cap eviction shifts elements on every
//!   removal (or rotates internally) and forces a linear scan to find a
//!   block by id.
//! - A fixed-size array slotted by `block_id % capacity` gives O(1)
//!   insert, O(1) lookup, and recycles slots in place — newest write
//!   overwrites the oldest entry by construction.
//!
//! ## Eviction semantics
//!
//! Capacity is a fixed power of two (so `block_id % capacity` is a cheap
//! mask). Writing block `N` lands in slot `N mod capacity`; if the slot
//! was previously holding block `N - capacity`, that older block is
//! overwritten and gone. There's no time-based eviction — at 60 fps with
//! capacity 64, the buffer covers ~1.07 seconds. Tune capacity by the
//! latency budget you want for repair: a client can only ask for replay
//! within the buffered window.
//!
//! ## Lookup as ownership check
//!
//! `get(block_id)` checks that the slot at `block_id % capacity` actually
//! holds the requested id (not just *some* block in that slot). This
//! gives us free expiration: once block 47 has been overwritten by block
//! 111 (assuming capacity 64), `get(47)` returns `None` because slot 47
//! reads back a tracker holding `block_id = 111`.

use bytes::Bytes;

/// Default capacity — 64 blocks. At 60 fps with one source block per
/// frame, that's ~1.07s of replay window per track. Power of two so
/// modulo is a cheap mask.
pub const DEFAULT_CAPACITY: usize = 64;

/// One slot in the ring. `None` until first written; thereafter holds
/// the most recent block to land in this slot.
#[derive(Clone, Debug)]
struct Slot {
    /// Source block id this slot's payload corresponds to. Compared on
    /// lookup to detect "the block you're asking for has already been
    /// overwritten by a newer one."
    block_id: u32,
    /// Source bytes (the recovered frame from upstream — exactly what
    /// would have been published by the upstream Publisher::send).
    payload: Bytes,
}

/// Fixed-capacity ring of source blocks.
pub struct BlockRing {
    /// Capacity is fixed at construction. We index slots by
    /// `block_id & mask` where `mask = capacity - 1`.
    capacity: usize,
    mask: u32,
    slots: Vec<Option<Slot>>,
    /// Highest block id ever inserted. Useful for bounding replay scans.
    highest_seen: Option<u32>,
}

impl BlockRing {
    /// Construct a ring with the given capacity. Capacity MUST be a
    /// power of two (panics otherwise) so the modulo operation reduces
    /// to a bitmask. Pick capacity by latency budget: at 60 fps,
    /// capacity = 64 ⇒ ~1s replay window.
    pub fn new(capacity: usize) -> Self {
        assert!(
            capacity.is_power_of_two() && capacity > 0,
            "BlockRing capacity must be a power of two, got {capacity}"
        );
        Self {
            capacity,
            mask: (capacity as u32) - 1,
            slots: vec![None; capacity],
            highest_seen: None,
        }
    }

    /// Insert a block at slot `block_id % capacity`, overwriting whatever
    /// was there. If the slot held an older block, that block is gone.
    pub fn insert(&mut self, block_id: u32, payload: Bytes) {
        let idx = (block_id & self.mask) as usize;
        self.slots[idx] = Some(Slot { block_id, payload });
        self.highest_seen = Some(match self.highest_seen {
            Some(prev) => prev.max(block_id),
            None => block_id,
        });
    }

    /// Look up a block by id. Returns `Some(payload)` only if the slot
    /// at `block_id % capacity` actually holds that exact block id —
    /// otherwise the requested block has been overwritten by a newer
    /// one (or never existed) and is no longer recoverable from the
    /// ring.
    pub fn get(&self, block_id: u32) -> Option<&Bytes> {
        let idx = (block_id & self.mask) as usize;
        match &self.slots[idx] {
            Some(slot) if slot.block_id == block_id => Some(&slot.payload),
            _ => None,
        }
    }

    /// Iterator over the most-recently-inserted N blocks, newest first.
    /// Skips empty slots. Useful for "replay the last N blocks to this
    /// client" without the client having to name specific block ids.
    pub fn recent(&self, n: usize) -> Vec<(u32, Bytes)> {
        let n = n.min(self.capacity);
        let highest = match self.highest_seen {
            Some(h) => h,
            None => return Vec::new(),
        };
        let mut out = Vec::with_capacity(n);
        // Walk backwards from the most-recent block id, looking for
        // slots whose stored block_id matches expectation. Any mismatch
        // means that slot has been overwritten — skip it.
        for offset in 0..(n as u32) {
            if offset > highest {
                break;
            }
            let want_id = highest - offset;
            if let Some(payload) = self.get(want_id) {
                out.push((want_id, payload.clone()));
            }
        }
        out
    }

    pub fn capacity(&self) -> usize {
        self.capacity
    }

    pub fn highest_seen(&self) -> Option<u32> {
        self.highest_seen
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    #[should_panic]
    fn rejects_non_power_of_two_capacity() {
        let _ = BlockRing::new(7);
    }

    #[test]
    fn rejects_zero_capacity() {
        // power_of_two(0) is false, so this panics via the same assert.
        let result = std::panic::catch_unwind(|| BlockRing::new(0));
        assert!(result.is_err());
    }

    #[test]
    fn insert_then_get_returns_payload() {
        let mut ring = BlockRing::new(4);
        ring.insert(1, Bytes::from_static(b"one"));
        ring.insert(2, Bytes::from_static(b"two"));
        assert_eq!(ring.get(1).map(|b| b.as_ref()), Some(b"one".as_ref()));
        assert_eq!(ring.get(2).map(|b| b.as_ref()), Some(b"two".as_ref()));
        assert!(ring.get(99).is_none());
    }

    #[test]
    fn overwrite_evicts_old_block() {
        // Capacity 4: block 0 and block 4 land in the same slot.
        let mut ring = BlockRing::new(4);
        ring.insert(0, Bytes::from_static(b"zero"));
        assert!(ring.get(0).is_some());
        // Block 4 overwrites slot 0. Asking for block 0 returns None
        // because the slot now holds block 4.
        ring.insert(4, Bytes::from_static(b"four"));
        assert!(ring.get(0).is_none(), "block 0 should be evicted");
        assert_eq!(ring.get(4).map(|b| b.as_ref()), Some(b"four".as_ref()));
    }

    #[test]
    fn lookup_for_unwritten_slot_returns_none() {
        let ring = BlockRing::new(4);
        assert!(ring.get(0).is_none());
        assert!(ring.get(7).is_none());
    }

    #[test]
    fn recent_returns_newest_first_skipping_gaps() {
        let mut ring = BlockRing::new(8);
        // Insert with a gap at id=2 (simulating a never-recovered block).
        for id in [0u32, 1, 3, 4, 5] {
            ring.insert(id, Bytes::copy_from_slice(format!("blk{id}").as_bytes()));
        }
        let recent = ring.recent(4);
        // Walk back from id=5 across the last 4 ids: 5, 4, 3, 2. Id 2
        // was never inserted so it's skipped. Id 1 is *outside* the
        // walked window so it doesn't appear — recent(N) is "the last
        // N consecutive ids," not "the N most-recently-inserted".
        let ids: Vec<u32> = recent.iter().map(|(id, _)| *id).collect();
        assert_eq!(ids, vec![5, 4, 3]);
    }

    #[test]
    fn recent_caps_at_capacity() {
        let mut ring = BlockRing::new(4);
        for id in 0..10u32 {
            ring.insert(id, Bytes::copy_from_slice(format!("blk{id}").as_bytes()));
        }
        // Asking for "last 100" should return at most 4 (capacity).
        let recent = ring.recent(100);
        assert_eq!(recent.len(), 4);
        let ids: Vec<u32> = recent.iter().map(|(id, _)| *id).collect();
        assert_eq!(ids, vec![9, 8, 7, 6]);
    }

    #[test]
    fn highest_seen_tracks_max() {
        let mut ring = BlockRing::new(8);
        assert_eq!(ring.highest_seen(), None);
        ring.insert(5, Bytes::from_static(b"a"));
        assert_eq!(ring.highest_seen(), Some(5));
        // Inserting a smaller id doesn't lower highest_seen.
        ring.insert(2, Bytes::from_static(b"b"));
        assert_eq!(ring.highest_seen(), Some(5));
        ring.insert(99, Bytes::from_static(b"c"));
        assert_eq!(ring.highest_seen(), Some(99));
    }

    #[test]
    fn lookup_distinguishes_stale_slot_from_present_block() {
        // capacity 4: ids 1 and 5 share slot 1.
        let mut ring = BlockRing::new(4);
        ring.insert(1, Bytes::from_static(b"first"));
        // get(1) sees the slot's stored id matches → returns payload.
        assert!(ring.get(1).is_some());
        // get(5) sees slot 1 holds id=1, mismatch → returns None even
        // though the slot is occupied.
        assert!(ring.get(5).is_none());
        // After inserting 5, slot 1 holds id=5; get(1) now returns None.
        ring.insert(5, Bytes::from_static(b"second"));
        assert!(ring.get(1).is_none());
        assert!(ring.get(5).is_some());
    }
}

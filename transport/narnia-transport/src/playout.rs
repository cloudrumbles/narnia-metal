use std::collections::BTreeMap;

#[derive(Debug, Clone)]
pub struct PlayoutFrame {
    pub id: u32,
    pub capture_us: u64,
    /// True if this frame is an IDR / keyframe — independently decodable
    /// and resets any skip-until-IDR state.
    pub is_idr: bool,
    pub data: Vec<u8>,
}

#[derive(Debug, Clone)]
pub struct PlayoutConfig {
    /// Neutral band J in microseconds — drift within ±J does not move D.
    pub neutral_band_us: i64,
    /// λ_u — coefficient for fast-up D updates on late arrivals.
    pub lambda_up: f64,
    /// ρ_u — power exponent for fast-up updates (concave when < 1.0).
    pub rho_up: f64,
    /// λ_l — coefficient for slow-down D updates on early arrivals.
    pub lambda_down: f64,
    /// ρ_l — power exponent for slow-down updates (convex when > 1.0).
    pub rho_down: f64,
    /// Δ — cap on how far above the baseline D may drift, in microseconds.
    pub delta_us: u64,
    /// γ — QADC quantization step in microseconds. E moves only in γ-sized
    /// jumps to avoid sub-frame schedule thrashing.
    pub gamma_us: u64,
    /// If the gap between successive arrivals exceeds this, D is reanchored
    /// to (A_n − S_n). Handles long-term sender/receiver clock drift.
    pub idle_reanchor_us: u64,
    pub gap_timeout_us: u64,
    pub nack_delay_us: u64,
}

impl PlayoutConfig {
    pub fn cloud_gaming() -> Self {
        Self {
            neutral_band_us: 2_000,
            lambda_up: 0.16,
            rho_up: 0.5,
            lambda_down: 0.04,
            rho_down: 1.5,
            delta_us: 75_000,
            gamma_us: 16_000,
            idle_reanchor_us: 1_000_000,
            gap_timeout_us: 75_000,
            nack_delay_us: 30_000,
        }
    }
}

#[derive(Debug, Clone, Copy)]
pub struct PlayoutStats {
    /// Adaptive offset D in microseconds — bridges sender/receiver clocks
    /// and absorbs jitter.
    pub d_us: i64,
    /// QADC-quantized scheduling offset E in microseconds (≥ 0).
    pub e_us: u64,
    /// Number of frames processed since construction.
    pub frames_seen: u64,
    /// Frames dropped because their id was at or below `last_fed` — late
    /// repair arrivals after the cursor moved past them.
    pub stragglers: u64,
}

struct Slot {
    frame: PlayoutFrame,
    release_at_us: u64,
}

pub struct PlayoutBuffer {
    cfg: PlayoutConfig,
    buf: BTreeMap<u32, Slot>,
    last_fed: Option<u32>,
    d_us: i64,
    baseline_d_us: i64,
    frames_seen: u64,
    last_arrival_us: Option<u64>,
    stragglers: u64,
    skip_until_idr: bool,
    rtt_us: Option<u64>,
    lost_block: Option<u32>,
    reported_lost: Option<u32>,
    gave_up: bool,
}

impl PlayoutBuffer {
    pub fn new(cfg: PlayoutConfig) -> Self {
        Self {
            cfg,
            buf: BTreeMap::new(),
            last_fed: None,
            d_us: 0,
            baseline_d_us: 0,
            frames_seen: 0,
            last_arrival_us: None,
            stragglers: 0,
            skip_until_idr: false,
            rtt_us: None,
            lost_block: None,
            reported_lost: None,
            gave_up: false,
        }
    }

    pub fn take_lost_block(&mut self) -> Option<u32> {
        self.lost_block.take()
    }

    pub fn take_gave_up(&mut self) -> bool {
        std::mem::take(&mut self.gave_up)
    }

    fn first_missing_block(&self) -> Option<u32> {
        let mut expected: u32 = match self.last_fed {
            Some(prev) => prev.wrapping_add(1),
            None => *self.buf.keys().next()?,
        };
        for &key in self.buf.keys() {
            if key != expected {
                return Some(expected);
            }
            expected = expected.wrapping_add(1);
        }
        None
    }

    pub fn set_round_trip_us(&mut self, rtt_us: u64) {
        self.rtt_us = Some(rtt_us);
    }

    fn gap_hold_us(&self) -> u64 {
        match self.rtt_us {
            Some(rtt) => {
                let hold: u64 = self.cfg.nack_delay_us.saturating_add(rtt);
                hold.min(self.cfg.gap_timeout_us)
            }
            None => self.cfg.gap_timeout_us,
        }
    }

    pub fn is_skipping_until_idr(&self) -> bool {
        self.skip_until_idr
    }

    pub fn stats(&self) -> PlayoutStats {
        PlayoutStats {
            d_us: self.d_us,
            e_us: self.quantized_e(),
            frames_seen: self.frames_seen,
            stragglers: self.stragglers,
        }
    }

    fn quantized_e(&self) -> u64 {
        if self.d_us <= 0 {
            return 0;
        }
        let d: u64 = self.d_us as u64;
        (d / self.cfg.gamma_us) * self.cfg.gamma_us
    }

    fn update_d(&mut self, frame: &PlayoutFrame, arrival_us: u64) {
        // First frame, or long quiet gap → reanchor D to bridge clocks.
        let idle_gap: bool = match self.last_arrival_us {
            Some(prev) => arrival_us.saturating_sub(prev) > self.cfg.idle_reanchor_us,
            None => false,
        };
        if self.frames_seen == 0 || idle_gap {
            let anchor: i64 = arrival_us as i64 - frame.capture_us as i64;
            self.d_us = anchor;
            self.baseline_d_us = anchor;
            return;
        }
        // x > 0 means the frame arrived later than D predicted (late).
        let x: i64 = arrival_us as i64 - (frame.capture_us as i64 + self.d_us);
        if x > self.cfg.neutral_band_us {
            let magnitude: f64 = x as f64;
            let bump: f64 = self.cfg.lambda_up * magnitude.powf(self.cfg.rho_up);
            self.d_us = self.d_us.saturating_add(bump.ceil() as i64);
        } else if x < -self.cfg.neutral_band_us {
            let magnitude: f64 = (-x) as f64;
            let drop: f64 = self.cfg.lambda_down * magnitude.powf(self.cfg.rho_down);
            self.d_us = self.d_us.saturating_sub(drop.ceil() as i64);
        }
        // Cap at baseline + delta.
        let cap: i64 = self
            .baseline_d_us
            .saturating_add(self.cfg.delta_us as i64);
        if self.d_us > cap {
            self.d_us = cap;
        }
    }

    pub fn insert(&mut self, frame: PlayoutFrame, arrival_us: u64) {
        // Straggler: id already passed by the cursor — drop without updating D.
        if let Some(prev) = self.last_fed {
            if frame.id <= prev {
                self.stragglers = self.stragglers.saturating_add(1);
                return;
            }
        }
        // IDR boundary: any buffered frames with smaller ids are old-GOP
        // references and can no longer be decoded — evict them.
        if frame.is_idr {
            let stale_keys: Vec<u32> = self
                .buf
                .keys()
                .copied()
                .filter(|k| *k < frame.id)
                .collect();
            for k in stale_keys {
                self.buf.remove(&k);
            }
            self.lost_block = None;
        }
        self.update_d(&frame, arrival_us);
        self.frames_seen = self.frames_seen.saturating_add(1);
        self.last_arrival_us = Some(arrival_us);
        let is_idr: bool = frame.is_idr;
        self.buf.insert(
            frame.id,
            Slot {
                frame,
                release_at_us: arrival_us,
            },
        );
        if !is_idr {
            if let Some(missing) = self.first_missing_block() {
                if self.reported_lost != Some(missing) {
                    self.reported_lost = Some(missing);
                    self.lost_block = Some(missing);
                }
            }
        }
    }

    pub fn drain(&mut self, now_us: u64) -> Option<PlayoutFrame> {
        let key: u32 = *self.buf.keys().next()?;
        let slot_release: u64 = self.buf.get(&key)?.release_at_us;
        let slot_is_idr: bool = self.buf.get(&key)?.frame.is_idr;

        // gap check
        let in_gap: bool = match self.last_fed {
            Some(prev) => key != prev.wrapping_add(1),
            None => false,
        };

        if in_gap {
            if now_us < slot_release.saturating_add(self.gap_hold_us()) {
                return None;
            }
            // predecessor declared missing. Two cases:
            //
            //  - IDR — fresh GOP. Jump the cursor, clear skip, emit.
            //  - P-frame — can't decode cleanly without its predecessor,
            //    but we still emit it so the decoder warms its DPB
            //    state and can pick up the intra-refresh wave that
            //    `narnia-server` is continuously emitting (NVENC
            //    `gopLength=NVENC_INFINITE_GOPLENGTH` + `enableIntraRefresh=1`,
            //    no scheduled IDRs). The render-side `skip_until_idr`
            //    flag suppresses the resulting corrupt frames until
            //    a recovery_point SEI marks the wave clean (or a
            //    real IDR lands via client-requested force-IDR).
            //    Dropping P-frames here was the historical behavior;
            //    on an intra-refresh stream that produces no
            //    natural IDRs it caused permanent freezes after
            //    every transient gap.
            let slot: Slot = self.buf.remove(&key)?;
            self.last_fed = Some(key);
            self.skip_until_idr = !slot_is_idr;
            self.gave_up = !slot_is_idr;
            return Some(slot.frame);
        }

        // schedule check
        if now_us < slot_release {
            return None;
        }

        let slot: Slot = self.buf.remove(&key)?;
        if slot.frame.is_idr {
            self.skip_until_idr = false;
        }
        self.gave_up = false;
        self.last_fed = Some(key);
        Some(slot.frame)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn reorders_out_of_order_arrivals() {
        let cfg: PlayoutConfig = PlayoutConfig::cloud_gaming();
        let mut pb: PlayoutBuffer = PlayoutBuffer::new(cfg);

        let now_us: u64 = 0;
        pb.insert(
            PlayoutFrame {
                id: 3,
                is_idr: false,
                capture_us: 0,
                data: vec![3],
            },
            now_us,
        );
        pb.insert(
            PlayoutFrame {
                id: 1,
                is_idr: false,
                capture_us: 0,
                data: vec![1],
            },
            now_us,
        );
        pb.insert(
            PlayoutFrame {
                id: 2,
                is_idr: false,
                capture_us: 0,
                data: vec![2],
            },
            now_us,
        );

        let mut drained: Vec<u32> = Vec::new();
        while let Some(f) = pb.drain(u64::MAX) {
            drained.push(f.id);
        }
        assert_eq!(drained, vec![1, 2, 3]);
    }

    #[test]
    fn gap_timeout_triggers_skip_until_idr() {
        let cfg: PlayoutConfig = PlayoutConfig::cloud_gaming();
        let g: u64 = cfg.gap_timeout_us;
        let mut pb: PlayoutBuffer = PlayoutBuffer::new(cfg);

        // anchor with an IDR at id=1
        pb.insert(
            PlayoutFrame {
                id: 1,
                capture_us: 0,
                is_idr: true,
                data: vec![],
            },
            0,
        );
        let _ = pb.drain(u64::MAX);

        // P-frame 3 arrives but predecessor 2 is missing
        pb.insert(
            PlayoutFrame {
                id: 3,
                capture_us: 0,
                is_idr: false,
                data: vec![],
            },
            0,
        );

        // before gap timeout: held
        assert!(pb.drain(g.saturating_sub(1)).is_none(), "should hold inside G window");

        // past gap timeout: emit the orphan P-frame so the decoder can
        // pick up the intra-refresh wave, but arm skip-until-IDR so the
        // render side suppresses corrupt output until recovery.
        let f: Option<PlayoutFrame> = pb.drain(g + 100_000);
        assert_eq!(
            f.map(|f| f.id),
            Some(3),
            "should yield the orphan P-frame with skip armed (intra-refresh recovery)"
        );
        assert!(
            pb.is_skipping_until_idr(),
            "skip-until-IDR flag should be set after emitting orphan P-frame"
        );
    }

    #[test]
    fn idr_clears_skip_until_idr() {
        let cfg: PlayoutConfig = PlayoutConfig::cloud_gaming();
        let g: u64 = cfg.gap_timeout_us;
        let mut pb: PlayoutBuffer = PlayoutBuffer::new(cfg);

        pb.insert(
            PlayoutFrame {
                id: 1,
                capture_us: 0,
                is_idr: true,
                data: vec![],
            },
            0,
        );
        let _ = pb.drain(u64::MAX);

        // gap → skip
        pb.insert(
            PlayoutFrame {
                id: 3,
                capture_us: 0,
                is_idr: false,
                data: vec![],
            },
            0,
        );
        let _ = pb.drain(g + 100_000);
        assert!(pb.is_skipping_until_idr(), "skip should be set after orphan P-frame");

        // IDR arrives further along; should release and clear skip
        pb.insert(
            PlayoutFrame {
                id: 5,
                capture_us: 0,
                is_idr: true,
                data: vec![],
            },
            0,
        );
        let f: Option<PlayoutFrame> = pb.drain(g * 2 + 100_000);
        assert_eq!(f.map(|f| f.id), Some(5), "IDR should be released across gap");
        assert!(
            !pb.is_skipping_until_idr(),
            "skip should be cleared once IDR is released"
        );
    }

    #[test]
    fn idr_invalidates_older_buffered_frames() {
        let cfg: PlayoutConfig = PlayoutConfig::cloud_gaming();
        let mut pb: PlayoutBuffer = PlayoutBuffer::new(cfg);

        pb.insert(
            PlayoutFrame {
                id: 1,
                capture_us: 0,
                is_idr: true,
                data: vec![],
            },
            0,
        );
        let _ = pb.drain(u64::MAX);
        pb.insert(
            PlayoutFrame {
                id: 2,
                capture_us: 0,
                is_idr: false,
                data: vec![],
            },
            0,
        );
        let _ = pb.drain(u64::MAX);

        // buffered orphans (gaps): ids 4 and 6
        pb.insert(
            PlayoutFrame {
                id: 4,
                capture_us: 0,
                is_idr: false,
                data: vec![],
            },
            0,
        );
        pb.insert(
            PlayoutFrame {
                id: 6,
                capture_us: 0,
                is_idr: false,
                data: vec![],
            },
            0,
        );

        // IDR arrives — must invalidate older buffered frames (4, 6)
        pb.insert(
            PlayoutFrame {
                id: 10,
                capture_us: 0,
                is_idr: true,
                data: vec![],
            },
            0,
        );

        // After enough wall time: only id=10 should be released
        let f: Option<PlayoutFrame> = pb.drain(1_000_000);
        assert_eq!(f.map(|f| f.id), Some(10));
        assert!(
            pb.drain(2_000_000).is_none(),
            "older buffered frames must be evicted by IDR"
        );
    }

    #[test]
    fn drops_straggler_with_id_below_last_fed() {
        let mut pb: PlayoutBuffer = PlayoutBuffer::new(PlayoutConfig::cloud_gaming());

        pb.insert(
            PlayoutFrame {
                id: 1,
                is_idr: false,
                capture_us: 0,
                data: vec![],
            },
            0,
        );
        pb.insert(
            PlayoutFrame {
                id: 2,
                is_idr: false,
                capture_us: 0,
                data: vec![],
            },
            0,
        );
        let _ = pb.drain(u64::MAX);
        let _ = pb.drain(u64::MAX);

        let stragglers_before: u64 = pb.stats().stragglers;

        // late frame 1 arrives long after the cursor has moved past it
        pb.insert(
            PlayoutFrame {
                id: 1,
                is_idr: false,
                capture_us: 0,
                data: vec![],
            },
            100_000,
        );

        let stragglers_after: u64 = pb.stats().stragglers;
        assert_eq!(
            stragglers_after,
            stragglers_before + 1,
            "straggler must be counted"
        );
        assert!(
            pb.drain(u64::MAX).is_none(),
            "straggler should not be retained"
        );
    }

    #[test]
    fn idle_reanchors_after_quiet_gap() {
        let mut pb: PlayoutBuffer = PlayoutBuffer::new(PlayoutConfig::cloud_gaming());

        pb.insert(
            PlayoutFrame {
                id: 1,
                is_idr: false,
                capture_us: 0,
                data: vec![],
            },
            0,
        );
        // bump D up via repeated late frames
        for i in 2..50u32 {
            pb.insert(
                PlayoutFrame {
                    id: i,
                    is_idr: false,
                capture_us: 0,
                    data: vec![],
                },
                100_000,
            );
        }
        let d_before_gap: i64 = pb.stats().d_us;
        assert!(d_before_gap > 0, "D should be > 0 after late bumps");

        // long quiet gap (>1s), then a fresh frame: A=5s, S=4990ms → A-S=10ms
        pb.insert(
            PlayoutFrame {
                id: 50,
                is_idr: false,
                capture_us: 4_990_000,
                data: vec![],
            },
            5_000_000,
        );
        let d_after_reanchor: i64 = pb.stats().d_us;

        assert_eq!(
            d_after_reanchor, 10_000,
            "D should reanchor to A-S after idle gap, got {d_after_reanchor}"
        );
    }

    #[test]
    fn qadc_quantizes_e_in_gamma_steps() {
        let cfg: PlayoutConfig = PlayoutConfig::cloud_gaming();
        let gamma: u64 = cfg.gamma_us;
        let mut pb: PlayoutBuffer = PlayoutBuffer::new(cfg);

        pb.insert(
            PlayoutFrame {
                id: 1,
                is_idr: false,
                capture_us: 0,
                data: vec![],
            },
            0,
        );

        // pump late frames so D saturates past several gamma boundaries
        for i in 2..1500u32 {
            pb.insert(
                PlayoutFrame {
                    id: i,
                    is_idr: false,
                capture_us: 0,
                    data: vec![],
                },
                1_000_000,
            );
        }

        let e: u64 = pb.stats().e_us;
        let d: i64 = pb.stats().d_us;
        assert!(e > 0, "E should be positive after sustained lateness, got {e}");
        assert_eq!(e % gamma, 0, "E must be a multiple of gamma: e={e}, gamma={gamma}");
        // 0 ≤ D − E < gamma when D ≥ 0 (floor quantization invariant)
        let gap: i64 = d - e as i64;
        assert!(
            gap >= 0 && gap < gamma as i64,
            "E should be floor(D/gamma)*gamma; D={d}, E={e}, gap={gap}, gamma={gamma}"
        );
    }

    #[test]
    fn d_caps_at_baseline_plus_delta() {
        let cfg: PlayoutConfig = PlayoutConfig::cloud_gaming();
        let delta_cap: i64 = cfg.delta_us as i64;
        let mut pb: PlayoutBuffer = PlayoutBuffer::new(cfg);

        // anchor D = 0
        pb.insert(
            PlayoutFrame {
                id: 1,
                is_idr: false,
                capture_us: 0,
                data: vec![],
            },
            0,
        );

        // pump pathologically late frames; D should saturate at delta cap
        for i in 2..200u32 {
            pb.insert(
                PlayoutFrame {
                    id: i,
                    is_idr: false,
                capture_us: 0,
                    data: vec![],
                },
                1_000_000,
            );
        }

        let d_final: i64 = pb.stats().d_us;
        assert!(
            d_final <= delta_cap,
            "D should be capped at baseline+delta={delta_cap}, got {d_final}"
        );
    }

    #[test]
    fn d_decreases_slowly_on_early_arrivals() {
        let mut pb: PlayoutBuffer = PlayoutBuffer::new(PlayoutConfig::cloud_gaming());

        // anchor at id=1 with D = 0
        pb.insert(
            PlayoutFrame {
                id: 1,
                is_idr: false,
                capture_us: 0,
                data: vec![],
            },
            0,
        );

        // late frame 2: bump D up so we have headroom to drop
        pb.insert(
            PlayoutFrame {
                id: 2,
                is_idr: false,
                capture_us: 0,
                data: vec![],
            },
            100_000,
        );
        let d_after_bump: i64 = pb.stats().d_us;
        assert!(d_after_bump > 0, "D should be positive after bump");

        // early frame 3: arrival 50ms before (capture + D)
        let s_n: u64 = 200_000;
        let early_arrival: u64 = (s_n as i64 + d_after_bump - 50_000) as u64;
        pb.insert(
            PlayoutFrame {
                id: 3,
                is_idr: false,
                capture_us: s_n,
                data: vec![],
            },
            early_arrival,
        );
        let d_after_drop: i64 = pb.stats().d_us;

        assert!(
            d_after_drop < d_after_bump,
            "D should decrease on early: was {d_after_bump}, now {d_after_drop}"
        );
    }

    #[test]
    fn d_increases_aggressively_on_late_arrivals() {
        let mut pb: PlayoutBuffer = PlayoutBuffer::new(PlayoutConfig::cloud_gaming());

        // first frame establishes D = A_0 - S_0 baseline (= 0 here)
        pb.insert(
            PlayoutFrame {
                id: 1,
                is_idr: false,
                capture_us: 0,
                data: vec![],
            },
            0,
        );
        let d_initial: i64 = pb.stats().d_us;

        // frame 2: capture=10ms, arrival=30ms → x = 30000 - (10000 + 0) = +20000us late
        pb.insert(
            PlayoutFrame {
                id: 2,
                is_idr: false,
                capture_us: 10_000,
                data: vec![],
            },
            30_000,
        );
        let d_after: i64 = pb.stats().d_us;

        assert!(
            d_after > d_initial,
            "D should increase on late arrival: was {d_initial}, now {d_after}"
        );
    }

    fn frame(id: u32, capture_us: u64) -> PlayoutFrame {
        PlayoutFrame {
            id,
            capture_us,
            is_idr: false,
            data: vec![],
        }
    }

    #[test]
    fn releases_an_in_order_frame_the_moment_it_arrives() {
        let mut pb: PlayoutBuffer = PlayoutBuffer::new(PlayoutConfig::cloud_gaming());
        pb.insert(frame(1, 0), 0);
        assert_eq!(pb.drain(0).map(|f| f.id), Some(1));
        pb.insert(frame(2, 5_000_000), 15_000);
        assert_eq!(pb.drain(15_000).map(|f| f.id), Some(2));
    }

    fn gap_released_at(pb: &mut PlayoutBuffer, arrival_us: u64, wait_us: u64) {
        pb.insert(frame(1, 0), 0);
        assert_eq!(pb.drain(0).map(|f| f.id), Some(1));
        pb.insert(frame(3, 0), arrival_us);
        assert!(pb.drain(arrival_us + wait_us - 1).is_none(), "released before the gap wait of {wait_us} us");
        assert_eq!(pb.drain(arrival_us + wait_us).map(|f| f.id), Some(3));
        assert!(pb.is_skipping_until_idr());
    }

    #[test]
    fn gap_wait_is_the_ceiling_until_the_round_trip_is_known() {
        let cfg: PlayoutConfig = PlayoutConfig::cloud_gaming();
        let ceiling: u64 = cfg.gap_timeout_us;
        let mut pb: PlayoutBuffer = PlayoutBuffer::new(cfg);
        gap_released_at(&mut pb, 1_000_000, ceiling);
    }

    #[test]
    fn gap_wait_is_one_nack_delay_plus_the_round_trip() {
        let cfg: PlayoutConfig = PlayoutConfig::cloud_gaming();
        let wait: u64 = cfg.nack_delay_us + 10_000;
        let mut pb: PlayoutBuffer = PlayoutBuffer::new(cfg);
        pb.set_round_trip_us(10_000);
        gap_released_at(&mut pb, 1_000_000, wait);
    }

    #[test]
    fn reports_the_first_missing_block_as_soon_as_a_later_one_arrives() {
        let cfg: PlayoutConfig = PlayoutConfig::cloud_gaming();
        let ceiling: u64 = cfg.gap_timeout_us;
        let mut pb: PlayoutBuffer = PlayoutBuffer::new(cfg);
        pb.insert(frame(1, 0), 0);
        assert_eq!(pb.drain(0).map(|f| f.id), Some(1));
        pb.insert(frame(4, 0), 1_000_000);
        assert_eq!(pb.take_lost_block(), Some(2));
        assert_eq!(pb.take_lost_block(), None);
        pb.insert(frame(5, 0), 1_016_000);
        assert_eq!(pb.take_lost_block(), None);
        assert_eq!(pb.drain(1_000_000 + ceiling).map(|f| f.id), Some(4));
        assert_eq!(pb.take_lost_block(), None);
    }

    #[test]
    fn a_new_gap_after_a_repaired_one_is_reported_again() {
        let mut pb: PlayoutBuffer = PlayoutBuffer::new(PlayoutConfig::cloud_gaming());
        pb.insert(frame(1, 0), 0);
        assert_eq!(pb.drain(0).map(|f| f.id), Some(1));
        pb.insert(frame(3, 0), 32_000);
        assert_eq!(pb.take_lost_block(), Some(2));
        pb.insert(frame(2, 0), 40_000);
        assert_eq!(pb.drain(40_000).map(|f| f.id), Some(2));
        assert_eq!(pb.drain(40_000).map(|f| f.id), Some(3));
        pb.insert(frame(6, 0), 96_000);
        assert_eq!(pb.take_lost_block(), Some(4));
    }

    #[test]
    fn reports_nothing_lost_when_frames_arrive_in_order() {
        let mut pb: PlayoutBuffer = PlayoutBuffer::new(PlayoutConfig::cloud_gaming());
        for id in 1..=5 {
            pb.insert(frame(id, 0), id as u64 * 16_000);
            assert_eq!(pb.drain(id as u64 * 16_000).map(|f| f.id), Some(id));
        }
        assert_eq!(pb.take_lost_block(), None);
    }

    #[test]
    fn every_gap_it_gives_up_on_is_flagged_once_even_without_a_keyframe_between() {
        let cfg: PlayoutConfig = PlayoutConfig::cloud_gaming();
        let ceiling: u64 = cfg.gap_timeout_us;
        let mut pb: PlayoutBuffer = PlayoutBuffer::new(cfg);
        pb.insert(frame(1, 0), 0);
        assert_eq!(pb.drain(0).map(|f| f.id), Some(1));
        assert!(!pb.take_gave_up());
        pb.insert(frame(3, 0), 100_000);
        assert_eq!(pb.drain(100_000 + ceiling).map(|f| f.id), Some(3));
        assert!(pb.take_gave_up());
        assert!(!pb.take_gave_up());
        pb.insert(frame(4, 0), 300_000);
        assert_eq!(pb.drain(300_000).map(|f| f.id), Some(4));
        assert!(!pb.take_gave_up());
        pb.insert(frame(6, 0), 400_000);
        assert_eq!(pb.drain(400_000 + ceiling).map(|f| f.id), Some(6));
        assert!(pb.take_gave_up());
    }

    #[test]
    fn a_keyframe_after_a_gap_reports_nothing_lost() {
        let mut pb: PlayoutBuffer = PlayoutBuffer::new(PlayoutConfig::cloud_gaming());
        pb.insert(frame(1, 0), 0);
        assert_eq!(pb.drain(0).map(|f| f.id), Some(1));
        pb.insert(
            PlayoutFrame {
                id: 3,
                capture_us: 0,
                is_idr: true,
                data: vec![],
            },
            1_000_000,
        );
        assert_eq!(pb.take_lost_block(), None);
    }

    #[test]
    fn gap_wait_never_exceeds_the_ceiling() {
        let cfg: PlayoutConfig = PlayoutConfig::cloud_gaming();
        let ceiling: u64 = cfg.gap_timeout_us;
        let mut pb: PlayoutBuffer = PlayoutBuffer::new(cfg);
        pb.set_round_trip_us(500_000);
        gap_released_at(&mut pb, 1_000_000, ceiling);
    }

    #[test]
    fn holds_inside_gap_window_then_emits_with_skip_armed() {
        let cfg: PlayoutConfig = PlayoutConfig::cloud_gaming();
        let g: u64 = cfg.gap_timeout_us;
        let mut pb: PlayoutBuffer = PlayoutBuffer::new(cfg);

        // establish the cursor by feeding+draining frame 1
        pb.insert(
            PlayoutFrame {
                id: 1,
                is_idr: false,
                capture_us: 0,
                data: vec![1],
            },
            0,
        );
        let f1: Option<PlayoutFrame> = pb.drain(u64::MAX);
        assert_eq!(f1.map(|f| f.id), Some(1));

        // frame 3 arrives but 2 has not — predecessor missing
        pb.insert(
            PlayoutFrame {
                id: 3,
                is_idr: false,
                capture_us: 0,
                data: vec![3],
            },
            0,
        );

        // inside the gap window: hold and wait
        assert!(
            pb.drain(g.saturating_sub(1)).is_none(),
            "should hold inside the gap window"
        );

        // past the gap window: emit the orphan P-frame so the decoder
        // can warm its DPB state on the intra-refresh wave (NVENC
        // emits no scheduled IDRs). Render side suppresses corrupt
        // output via `is_skipping_until_idr` until recovery.
        let f3: Option<PlayoutFrame> = pb.drain(g + 100_000);
        assert_eq!(
            f3.map(|f| f.id),
            Some(3),
            "should yield the orphan P-frame past gap window"
        );
        assert!(
            pb.is_skipping_until_idr(),
            "skip flag should be armed after emitting an orphan P-frame"
        );
    }
}

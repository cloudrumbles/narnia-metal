//! Adaptive bitrate + adaptive FEC.
//!
//! The server samples its QUIC connection a few times a second and feeds
//! the samples to a [`RateController`], which answers with the encoder
//! bitrate and the FEC redundancy to use next.
//!
//! ## Model
//!
//! The controller tracks one number for the link: the **wire budget**, the
//! bits per second the stream (video + FEC repair) may put on the wire.
//! FEC takes its share of that budget first, sized from the loss the link
//! shows; the encoder gets the rest:
//!
//! ```text
//! bitrate = wire_budget / (1 + fec)
//! ```
//!
//! So when the link gets lossier the picture gets a little softer and
//! better protected, without sending more; and when the link gets
//! congested the whole budget shrinks.
//!
//! ## Signals
//!
//! All taken on the server, so every client (browser or native) gets
//! adaptation without speaking anything new:
//!
//! - **loss**: packets quinn declared lost / packets sent, per tick. Loss
//!   alone is not taken as congestion (wifi drops packets on an idle
//!   link; that's FEC's job), unless it is heavy.
//! - **queueing delay**: smoothed RTT minus the lowest RTT of the last
//!   ~20 s. A standing queue means we send faster than the path drains.
//! - **send backlog**: datagrams handed to quinn minus datagrams quinn has
//!   put on the wire. When it grows, BBR's window is full and quinn is
//!   buffering (then dropping) our datagrams locally.
//! - **unrecovered blocks**: blocks the client reported short of K
//!   symbols (`deficit` lines). FEC was too thin for that loss burst.
//!
//! Pure logic, no clock reads: the caller stamps samples with its own
//! monotonic milliseconds, which keeps this testable and wasm-clean.

/// Limits and starting point for a [`RateController`].
#[derive(Clone, Debug)]
pub struct RateConfig {
    /// Lowest encoder bitrate the controller will pick.
    pub min_bitrate_bps: u32,
    /// Highest encoder bitrate the controller will pick.
    pub max_bitrate_bps: u32,
    /// Encoder bitrate at session start.
    pub start_bitrate_bps: u32,
    /// Lowest P-frame redundancy. Keeps a margin for loss that hasn't
    /// shown up in the stats yet.
    pub min_fec: f32,
    /// Highest P-frame redundancy.
    pub max_fec: f32,
    /// P-frame redundancy at session start.
    pub start_fec: f32,
    /// Off: the bitrate stays at `start_bitrate_bps`.
    pub adapt_bitrate: bool,
    /// Off: the redundancy stays at `start_fec`.
    pub adapt_fec: bool,
}

impl Default for RateConfig {
    fn default() -> Self {
        Self {
            min_bitrate_bps: 2_000_000,
            max_bitrate_bps: 20_000_000,
            start_bitrate_bps: 8_000_000,
            min_fec: 0.1,
            max_fec: 1.0,
            start_fec: 0.3,
            adapt_bitrate: true,
            adapt_fec: true,
        }
    }
}

/// Cumulative counters for one connection, read once per tick. Every
/// field but `at_ms` and `rtt_us` only ever grows; the controller works
/// on the difference between consecutive samples.
#[derive(Clone, Copy, Debug, Default)]
pub struct LinkSample {
    /// Caller's monotonic clock, milliseconds.
    pub at_ms: u64,
    /// Smoothed RTT, microseconds. 0 = not measured yet.
    pub rtt_us: u64,
    /// UDP packets sent.
    pub sent_packets: u64,
    /// Packets declared lost.
    pub lost_packets: u64,
    /// UDP bytes sent.
    pub sent_bytes: u64,
    /// Media datagrams handed to the connection.
    pub datagrams_queued: u64,
    /// Media datagrams the connection actually transmitted.
    pub datagrams_sent: u64,
    /// Distinct blocks the client reported it couldn't recover.
    pub unrecovered_blocks: u64,
}

/// Why the wire budget moved (or didn't) on the last tick.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RateEvent {
    /// Not enough data yet, or nothing to change.
    Hold,
    /// Budget raised.
    Increase,
    /// The send backlog grew: quinn is buffering or dropping our datagrams.
    DecreaseBacklog,
    /// RTT rose well above its baseline: a queue is building on the path.
    DecreaseDelay,
    /// Loss too heavy to be noise.
    DecreaseLoss,
}

/// What to use from now on.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct RateDecision {
    /// Encoder bitrate, bits per second.
    pub bitrate_bps: u32,
    /// Redundancy for P-frames (repair symbols per source symbol).
    pub fec: f32,
    /// Redundancy for IDR frames: a lost keyframe costs far more.
    pub keyframe_fec: f32,
    pub event: RateEvent,
}

/// Link measurements behind the last decision, for logs and the HUD.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct LinkView {
    /// Loss fraction this tick (0..1), None when too few packets.
    pub loss: Option<f64>,
    /// Smoothed loss estimate (fast up, slow down).
    pub loss_avg: f64,
    /// RTT above the baseline, ms.
    pub queue_delay_ms: f64,
    /// Baseline (lowest recent) RTT, ms.
    pub base_rtt_ms: f64,
    /// Measured send rate, bits per second.
    pub wire_bps: f64,
    /// Wire budget, bits per second.
    pub budget_bps: f64,
    /// Datagrams that piled up (or were dropped) in quinn this tick.
    pub backlog_growth: i64,
    /// Blocks the client couldn't recover this tick.
    pub unrecovered: u64,
}

/// Shortest tick the controller acts on; faster samples are ignored.
const MIN_TICK_MS: u64 = 100;
/// Fewer packets than this in a tick says nothing about loss.
const MIN_PACKETS_FOR_LOSS: u64 = 20;
/// Loss averaged above this is treated as congestion, not noise.
const CONGESTION_LOSS: f64 = 0.15;
/// After a decrease: wait this long before the next decrease (lets the
/// encoder's new rate reach the stats) ...
const DECREASE_HOLD_MS: u64 = 1_000;
/// ... and this long before increasing again.
const INCREASE_HOLD_MS: u64 = 3_000;
/// Growth per tick when well below the last congestion point.
const FAST_INCREASE: f64 = 1.05;
/// Growth per tick near (within 10% of) the last congestion point.
const SLOW_INCREASE: f64 = 1.01;
/// The last congestion point is forgotten after this long.
const CEILING_MEMORY_MS: u64 = 30_000;
/// Baseline RTT is the minimum over two windows of this length.
const RTT_WINDOW_MS: u64 = 10_000;
/// Bitrate changes smaller than this fraction aren't passed on: every
/// change reconfigures the encoder.
const MIN_BITRATE_STEP: f64 = 0.03;
/// Highest keyframe redundancy.
const MAX_KEYFRAME_FEC: f32 = 2.0;
/// Lowest keyframe redundancy.
const MIN_KEYFRAME_FEC: f32 = 0.5;

pub struct RateController {
    cfg: RateConfig,
    prev: Option<LinkSample>,
    budget_bps: f64,
    fec: f32,
    bitrate_bps: u32,
    loss_avg: f64,
    loss_peak: f64,
    /// Extra redundancy after unrecovered blocks; decays.
    fec_boost: f32,
    rtt_win_min_us: u64,
    rtt_prev_win_min_us: u64,
    rtt_win_start_ms: u64,
    last_decrease_ms: Option<u64>,
    /// Budget the last congestion was seen at, and when.
    ceiling: Option<(f64, u64)>,
    view: LinkView,
}

impl RateController {
    pub fn new(mut cfg: RateConfig) -> Self {
        cfg.min_bitrate_bps = cfg.min_bitrate_bps.max(100_000);
        cfg.max_bitrate_bps = cfg.max_bitrate_bps.max(cfg.min_bitrate_bps);
        cfg.start_bitrate_bps = cfg.start_bitrate_bps.clamp(cfg.min_bitrate_bps, cfg.max_bitrate_bps);
        cfg.min_fec = cfg.min_fec.max(0.0);
        cfg.max_fec = cfg.max_fec.max(cfg.min_fec);
        if cfg.adapt_fec {
            cfg.start_fec = cfg.start_fec.clamp(cfg.min_fec, cfg.max_fec);
        }
        let fec = cfg.start_fec.max(0.0);
        let bitrate = cfg.start_bitrate_bps;
        Self {
            budget_bps: bitrate as f64 * (1.0 + fec as f64),
            fec,
            bitrate_bps: bitrate,
            loss_avg: 0.0,
            loss_peak: 0.0,
            fec_boost: 0.0,
            rtt_win_min_us: u64::MAX,
            rtt_prev_win_min_us: u64::MAX,
            rtt_win_start_ms: 0,
            last_decrease_ms: None,
            ceiling: None,
            prev: None,
            view: LinkView::default(),
            cfg,
        }
    }

    pub fn config(&self) -> &RateConfig {
        &self.cfg
    }

    /// The decision in force, without feeding a sample.
    pub fn current(&self) -> RateDecision {
        self.decision(RateEvent::Hold)
    }

    /// Measurements behind the last decision.
    pub fn view(&self) -> LinkView {
        self.view
    }

    fn decision(&self, event: RateEvent) -> RateDecision {
        RateDecision {
            bitrate_bps: self.bitrate_bps,
            fec: self.fec,
            keyframe_fec: (self.fec * 2.0).clamp(MIN_KEYFRAME_FEC, MAX_KEYFRAME_FEC),
            event,
        }
    }

    /// Feed one sample; get the bitrate and redundancy to use next.
    pub fn on_sample(&mut self, s: LinkSample) -> RateDecision {
        let Some(prev) = self.prev else {
            self.prev = Some(s);
            self.rtt_win_start_ms = s.at_ms;
            self.observe_rtt(s);
            return self.current();
        };
        if s.at_ms < prev.at_ms + MIN_TICK_MS {
            return self.current();
        }
        self.prev = Some(s);
        let dt_ms = (s.at_ms - prev.at_ms) as f64;

        // ── measure ──────────────────────────────────────────────────
        let d_sent = s.sent_packets.saturating_sub(prev.sent_packets);
        let d_lost = s.lost_packets.saturating_sub(prev.lost_packets);
        let loss = (d_sent >= MIN_PACKETS_FOR_LOSS).then(|| (d_lost as f64 / d_sent as f64).min(1.0));
        if let Some(l) = loss {
            // Fast attack, slow release: a loss burst raises FEC now,
            // and a clean second doesn't drop it straight back.
            let a = if l > self.loss_avg { 0.5 } else { 0.15 };
            self.loss_avg += a * (l - self.loss_avg);
            self.loss_peak = self.loss_peak.max(l);
        }
        self.loss_peak *= 0.95;

        let backlog_prev = prev.datagrams_queued as i64 - prev.datagrams_sent as i64;
        let backlog_now = s.datagrams_queued as i64 - s.datagrams_sent as i64;
        let backlog_growth = backlog_now - backlog_prev;
        let d_queued = s.datagrams_queued.saturating_sub(prev.datagrams_queued);
        // A few datagrams sitting in quinn's buffer at sample time is
        // normal; growth beyond ~3% of what we queued this tick isn't.
        let backlogged = backlog_growth > (5i64).max((d_queued as f64 * 0.03) as i64);

        self.observe_rtt(s);
        let base_rtt_us = self.base_rtt_us();
        let queue_delay_us = match base_rtt_us {
            Some(b) if s.rtt_us > 0 => s.rtt_us.saturating_sub(b),
            _ => 0,
        };
        let base_us = base_rtt_us.unwrap_or(0);
        let delay_limit_us = 30_000 + base_us / 4;
        let delay_clear_us = 10_000 + base_us / 8;

        let unrecovered = s.unrecovered_blocks.saturating_sub(prev.unrecovered_blocks);
        let wire_bps = s.sent_bytes.saturating_sub(prev.sent_bytes) as f64 * 8_000.0 / dt_ms;

        // ── FEC ──────────────────────────────────────────────────────
        if self.cfg.adapt_fec {
            if unrecovered > 0 {
                self.fec_boost = (self.fec_boost + 0.05 * unrecovered.min(4) as f32).min(0.5);
            } else {
                self.fec_boost *= 0.9;
            }
            // Redundancy r survives loss p when (1+r)(1-p) >= 1, i.e.
            // r >= p/(1-p). Double that (losses come in bursts and small
            // blocks have no slack) plus a fixed margin.
            let p = self.loss_avg.max(0.5 * self.loss_peak).min(0.9);
            let target = (2.0 * p / (1.0 - p) + 0.05) as f32 + self.fec_boost;
            let target = target.clamp(self.cfg.min_fec, self.cfg.max_fec);
            self.fec = if target >= self.fec {
                target
            } else {
                (self.fec - 0.02).max(target)
            };
        }

        // ── wire budget ──────────────────────────────────────────────
        let now = s.at_ms;
        let mut event = RateEvent::Hold;
        if self.cfg.adapt_bitrate {
            if let Some((_, at)) = self.ceiling {
                if now.saturating_sub(at) > CEILING_MEMORY_MS {
                    self.ceiling = None;
                }
            }
            let cause = if backlogged {
                Some((RateEvent::DecreaseBacklog, 0.8))
            } else if base_rtt_us.is_some() && queue_delay_us > delay_limit_us {
                Some((RateEvent::DecreaseDelay, 0.9))
            } else if self.loss_avg > CONGESTION_LOSS {
                Some((RateEvent::DecreaseLoss, 0.85))
            } else {
                None
            };
            let since_decrease = self.last_decrease_ms.map(|t| now.saturating_sub(t));
            match cause {
                Some((why, factor)) => {
                    if since_decrease.is_none_or(|d| d >= DECREASE_HOLD_MS) {
                        // Cut from what actually went out if that's lower:
                        // the budget may be well above a quiet scene's rate.
                        let from = if wire_bps > 0.0 { self.budget_bps.min(wire_bps.max(self.budget_bps * 0.5)) } else { self.budget_bps };
                        self.ceiling = Some((self.budget_bps, now));
                        self.budget_bps = from * factor;
                        self.last_decrease_ms = Some(now);
                        event = why;
                    }
                }
                None => {
                    let rested = since_decrease.is_none_or(|d| d >= INCREASE_HOLD_MS);
                    // Only grow a budget we're using: a static scene under
                    // CBR sends far less, and growing then is unprobed.
                    let using = wire_bps >= 0.5 * self.budget_bps;
                    let clear = queue_delay_us <= delay_clear_us && self.loss_avg < CONGESTION_LOSS / 2.0;
                    let at_max = self.bitrate_bps >= self.cfg.max_bitrate_bps;
                    if rested && using && clear && !at_max {
                        let near_ceiling = self.ceiling.is_some_and(|(c, _)| self.budget_bps >= 0.9 * c);
                        self.budget_bps *= if near_ceiling { SLOW_INCREASE } else { FAST_INCREASE };
                        if self.ceiling.is_some_and(|(c, _)| self.budget_bps > 1.1 * c) {
                            self.ceiling = None;
                        }
                        event = RateEvent::Increase;
                    }
                }
            }
        }

        // ── split budget into bitrate + FEC ──────────────────────────
        let min = self.cfg.min_bitrate_bps as f64;
        let max = self.cfg.max_bitrate_bps as f64;
        let want = if self.cfg.adapt_bitrate {
            (self.budget_bps / (1.0 + self.fec as f64)).clamp(min, max)
        } else {
            self.cfg.start_bitrate_bps as f64
        };
        // Keep the budget consistent with what the encoder will do.
        self.budget_bps = want * (1.0 + self.fec as f64);
        let cur = self.bitrate_bps as f64;
        let step = (want - cur).abs() / cur;
        let pinned = (want == min || want == max) && want != cur;
        if step >= MIN_BITRATE_STEP || pinned {
            // Round to 100 kbps so logs stay readable.
            self.bitrate_bps = ((want / 100_000.0).round() * 100_000.0).clamp(min, max) as u32;
        }

        self.view = LinkView {
            loss,
            loss_avg: self.loss_avg,
            queue_delay_ms: queue_delay_us as f64 / 1000.0,
            base_rtt_ms: base_us as f64 / 1000.0,
            wire_bps,
            budget_bps: self.budget_bps,
            backlog_growth,
            unrecovered,
        };
        self.decision(event)
    }

    fn observe_rtt(&mut self, s: LinkSample) {
        if s.at_ms.saturating_sub(self.rtt_win_start_ms) >= RTT_WINDOW_MS {
            self.rtt_prev_win_min_us = self.rtt_win_min_us;
            self.rtt_win_min_us = u64::MAX;
            self.rtt_win_start_ms = s.at_ms;
        }
        if s.rtt_us > 0 {
            self.rtt_win_min_us = self.rtt_win_min_us.min(s.rtt_us);
        }
    }

    fn base_rtt_us(&self) -> Option<u64> {
        let b = self.rtt_win_min_us.min(self.rtt_prev_win_min_us);
        (b != u64::MAX).then_some(b)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A fake link: sends what the encoder asks (up to `capacity_bps`),
    /// loses `loss` of packets, and queues the excess (RTT grows).
    struct Link {
        t_ms: u64,
        capacity_bps: f64,
        loss: f64,
        base_rtt_us: u64,
        queue_bits: f64,
        acc: LinkSample,
        unrecovered_per_tick: u64,
    }

    impl Link {
        fn new(capacity_bps: f64, loss: f64) -> Self {
            Self {
                t_ms: 0,
                capacity_bps,
                loss,
                base_rtt_us: 40_000,
                queue_bits: 0.0,
                acc: LinkSample::default(),
                unrecovered_per_tick: 0,
            }
        }

        /// One 500 ms tick at the controller's current decision.
        fn tick(&mut self, d: RateDecision) -> LinkSample {
            let dt = 0.5;
            let offered = d.bitrate_bps as f64 * (1.0 + d.fec as f64) * dt;
            self.queue_bits += offered;
            let drained = self.queue_bits.min(self.capacity_bps * dt);
            self.queue_bits -= drained;
            // A bottleneck buffer of 200 ms; what overflows it is lost.
            let overflow = (self.queue_bits - self.capacity_bps * 0.2).max(0.0);
            self.queue_bits -= overflow;
            let pkt_bits = 8.0 * 1100.0;
            let packets = (offered / pkt_bits) as u64;
            self.t_ms += 500;
            self.acc.at_ms = self.t_ms;
            self.acc.sent_packets += packets;
            self.acc.lost_packets += (overflow / pkt_bits + drained / pkt_bits * self.loss) as u64;
            self.acc.sent_bytes += (offered / 8.0) as u64;
            self.acc.datagrams_queued += packets;
            self.acc.datagrams_sent += packets;
            self.acc.rtt_us = self.base_rtt_us + (self.queue_bits / self.capacity_bps * 1e6) as u64;
            self.acc.unrecovered_blocks += self.unrecovered_per_tick;
            self.acc
        }
    }

    fn run(c: &mut RateController, link: &mut Link, ticks: usize) -> RateDecision {
        let mut d = c.current();
        for _ in 0..ticks {
            let s = link.tick(d);
            d = c.on_sample(s);
        }
        d
    }

    #[test]
    fn a_clean_fat_link_ramps_to_the_max_with_minimum_fec() {
        let mut c = RateController::new(RateConfig::default());
        let mut link = Link::new(100e6, 0.0);
        let d = run(&mut c, &mut link, 120);
        assert_eq!(d.bitrate_bps, 20_000_000);
        assert!((d.fec - 0.1).abs() < 1e-6, "fec {}", d.fec);
    }

    #[test]
    fn a_thin_link_settles_under_its_capacity() {
        let mut c = RateController::new(RateConfig::default());
        let mut link = Link::new(10e6, 0.0);
        let mut wire = Vec::new();
        let mut d = c.current();
        for _ in 0..240 {
            d = c.on_sample(link.tick(d));
            wire.push(d.bitrate_bps as f64 * (1.0 + d.fec as f64));
        }
        // Over the last minute: never far above capacity, and using a
        // fair part of it.
        let tail = &wire[120..];
        let avg = tail.iter().sum::<f64>() / tail.len() as f64;
        assert!(tail.iter().all(|&w| w < 12e6), "{tail:?}");
        assert!(avg > 6e6, "avg {avg}");
    }

    #[test]
    fn random_loss_raises_fec_and_keeps_the_wire_budget() {
        // On a link-limited path, loss moves budget from picture to FEC
        // instead of sending more.
        fn settle(c: &mut RateController, link: &mut Link) -> (f64, f64, f32) {
            let mut d = c.current();
            let (mut br, mut wire) = (0.0, 0.0);
            for i in 0..240 {
                d = c.on_sample(link.tick(d));
                if i >= 160 {
                    br += d.bitrate_bps as f64 / 80.0;
                    wire += d.bitrate_bps as f64 * (1.0 + d.fec as f64) / 80.0;
                }
            }
            (br, wire, d.fec)
        }
        let mut c = RateController::new(RateConfig::default());
        let mut link = Link::new(12e6, 0.0);
        let (clean_br, clean_wire, clean_fec) = settle(&mut c, &mut link);
        link.loss = 0.08;
        let (lossy_br, lossy_wire, lossy_fec) = settle(&mut c, &mut link);
        assert!(lossy_fec > clean_fec + 0.1, "fec {clean_fec} -> {lossy_fec}");
        assert!(lossy_br < clean_br, "bitrate {clean_br} -> {lossy_br}");
        assert!(lossy_wire < clean_wire * 1.15, "wire {clean_wire} -> {lossy_wire}");
    }

    #[test]
    fn with_headroom_loss_only_adds_fec() {
        let mut c = RateController::new(RateConfig::default());
        let mut link = Link::new(100e6, 0.0);
        let clean = run(&mut c, &mut link, 60);
        link.loss = 0.08;
        let lossy = run(&mut c, &mut link, 10);
        assert!(lossy.fec > 0.2, "fec {}", lossy.fec);
        assert_eq!(lossy.bitrate_bps, clean.bitrate_bps);
        // 8% isn't congestion: no decrease.
        assert!(c.last_decrease_ms.is_none());
    }

    #[test]
    fn fec_falls_back_slowly_after_loss_ends() {
        let mut c = RateController::new(RateConfig::default());
        let mut link = Link::new(100e6, 0.2);
        let lossy = run(&mut c, &mut link, 10);
        link.loss = 0.0;
        let after_1s = run(&mut c, &mut link, 2);
        assert!(after_1s.fec > lossy.fec - 0.1);
        let later = run(&mut c, &mut link, 120);
        assert!((later.fec - 0.1).abs() < 1e-6, "fec {}", later.fec);
    }

    #[test]
    fn unrecovered_blocks_add_fec() {
        let mut c = RateController::new(RateConfig { start_fec: 0.1, ..Default::default() });
        let mut link = Link::new(100e6, 0.0);
        run(&mut c, &mut link, 4);
        link.unrecovered_per_tick = 3;
        let d = run(&mut c, &mut link, 3);
        assert!(d.fec >= 0.4, "fec {}", d.fec);
    }

    #[test]
    fn a_growing_send_backlog_cuts_the_budget() {
        let mut c = RateController::new(RateConfig::default());
        let mut link = Link::new(100e6, 0.0);
        let before = run(&mut c, &mut link, 20);
        let mut s = link.tick(before);
        s.datagrams_queued += 500; // quinn couldn't send these
        let d = c.on_sample(s);
        assert_eq!(d.event, RateEvent::DecreaseBacklog);
        assert!(d.bitrate_bps < before.bitrate_bps);
    }

    #[test]
    fn fixed_modes_leave_their_knob_alone() {
        let mut c = RateController::new(RateConfig {
            adapt_bitrate: false,
            adapt_fec: false,
            start_fec: 1.0,
            ..Default::default()
        });
        let mut link = Link::new(5e6, 0.1);
        let d = run(&mut c, &mut link, 60);
        assert_eq!(d.bitrate_bps, 8_000_000);
        assert_eq!(d.fec, 1.0);
    }

    #[test]
    fn heavy_loss_is_treated_as_congestion() {
        let mut c = RateController::new(RateConfig::default());
        let mut link = Link::new(100e6, 0.0);
        let before = run(&mut c, &mut link, 20);
        link.loss = 0.3;
        let d = run(&mut c, &mut link, 4);
        assert!(c.last_decrease_ms.is_some());
        assert!(d.bitrate_bps < before.bitrate_bps);
    }

    #[test]
    fn small_moves_dont_reconfigure_the_encoder() {
        let mut c = RateController::new(RateConfig::default());
        let start = c.current().bitrate_bps;
        // Increase of 1%: below the step.
        c.budget_bps *= 1.01;
        let mut s = LinkSample { at_ms: 0, ..Default::default() };
        c.on_sample(s);
        s.at_ms = 500;
        c.cfg.adapt_bitrate = true;
        let d = c.on_sample(s);
        assert_eq!(d.bitrate_bps, start);
    }
}

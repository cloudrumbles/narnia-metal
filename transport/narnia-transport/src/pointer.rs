#[derive(Debug, Default)]
pub struct MotionRemainder {
    x: f64,
    y: f64,
}

impl MotionRemainder {
    pub fn take(&mut self, dx: f64, dy: f64) -> (i16, i16) {
        if dx.is_finite() {
            self.x += dx;
        }
        if dy.is_finite() {
            self.y += dy;
        }
        (whole_pixels(&mut self.x), whole_pixels(&mut self.y))
    }
}

fn whole_pixels(carry: &mut f64) -> i16 {
    let whole = carry.trunc().clamp(f64::from(i16::MIN), f64::from(i16::MAX));
    *carry -= whole;
    whole as i16
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn half_pixels_add_up_to_a_whole_one() {
        let mut motion = MotionRemainder::default();
        assert_eq!(motion.take(0.5, 0.5), (0, 0));
        assert_eq!(motion.take(0.5, 0.5), (1, 1));
    }

    #[test]
    fn whole_pixels_pass_straight_through() {
        let mut motion = MotionRemainder::default();
        assert_eq!(motion.take(3.0, -2.0), (3, -2));
        assert_eq!(motion.take(0.0, 0.0), (0, 0));
    }

    #[test]
    fn the_fraction_of_each_move_carries_into_the_next() {
        let mut motion = MotionRemainder::default();
        assert_eq!(motion.take(1.6, -1.6), (1, -1));
        assert_eq!(motion.take(1.6, -1.6), (2, -2));
        assert_eq!(motion.take(0.8, -0.8), (1, -1));
    }

    #[test]
    fn small_moves_left_then_right_cancel_out() {
        let mut motion = MotionRemainder::default();
        assert_eq!(motion.take(-0.75, 0.0), (0, 0));
        assert_eq!(motion.take(-0.5, 0.0), (-1, 0));
        assert_eq!(motion.take(0.25, 0.0), (0, 0));
        assert_eq!(motion.take(1.0, 0.0), (1, 0));
    }

    #[test]
    fn a_move_past_the_x11_range_is_sent_over_the_next_events() {
        let mut motion = MotionRemainder::default();
        assert_eq!(motion.take(40_000.0, -40_000.0), (i16::MAX, i16::MIN));
        assert_eq!(motion.take(0.0, 0.0), (7_233, -7_232));
    }

    #[test]
    fn a_non_finite_delta_moves_nothing_and_leaves_no_trace() {
        let mut motion = MotionRemainder::default();
        assert_eq!(motion.take(f64::NAN, 2.0), (0, 2));
        assert_eq!(motion.take(f64::INFINITY, f64::NEG_INFINITY), (0, 0));
        assert_eq!(motion.take(1.0, 1.0), (1, 1));
    }
}

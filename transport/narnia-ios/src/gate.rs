use std::time::{Duration, Instant};

const IDR_REREQUEST_AFTER: Duration = Duration::from_secs(1);

#[derive(Debug)]
pub struct IdrGate {
    waiting_for_idr: bool,
    last_request: Option<Instant>,
}

impl IdrGate {
    pub fn new() -> IdrGate {
        IdrGate {
            waiting_for_idr: true,
            last_request: None,
        }
    }

    pub fn admit(&mut self, is_idr: bool) -> bool {
        if self.waiting_for_idr && is_idr {
            self.waiting_for_idr = false;
        }
        !self.waiting_for_idr
    }

    pub fn note_loss(&mut self, _now: Instant) {
        if !self.waiting_for_idr {
            self.waiting_for_idr = true;
            self.last_request = None;
        }
    }

    pub fn take_idr_request(&mut self, now: Instant) -> bool {
        if !self.waiting_for_idr {
            return false;
        }
        let due = self
            .last_request
            .is_none_or(|asked| now.saturating_duration_since(asked) >= IDR_REREQUEST_AFTER);
        if due {
            self.last_request = Some(now);
        }
        due
    }
}

impl Default for IdrGate {
    fn default() -> Self {
        IdrGate::new()
    }
}

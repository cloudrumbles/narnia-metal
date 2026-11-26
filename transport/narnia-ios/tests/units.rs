use std::time::{Duration, Instant};

use bytes::Bytes;
use narnia_ios::{parse_cert_hash, IdrGate, VideoFrame};

#[test]
fn cert_hash_parses_lowercase_and_uppercase_hex() {
    let lower = "ab".repeat(32);
    let upper = "AB".repeat(32);
    assert_eq!(parse_cert_hash(&lower).expect("lowercase"), [0xab; 32]);
    assert_eq!(parse_cert_hash(&upper).expect("uppercase"), [0xab; 32]);
}

#[test]
fn cert_hash_rejects_wrong_length_and_non_hex() {
    assert!(parse_cert_hash(&"ab".repeat(31)).is_err());
    assert!(parse_cert_hash(&"ab".repeat(33)).is_err());
    assert!(parse_cert_hash(&format!("zz{}", "ab".repeat(31))).is_err());
    assert!(parse_cert_hash("").is_err());
}

#[test]
fn video_frame_splits_capture_time_from_annexb() {
    let annexb = [0u8, 0, 0, 1, 0x02, 0x01, 0x99];
    let payload = [1_234_567u64.to_be_bytes().as_slice(), annexb.as_slice()].concat();
    let frame = VideoFrame::from_payload(Bytes::from(payload)).expect("frame");
    assert_eq!(frame.capture_us, 1_234_567);
    assert_eq!(frame.annexb.as_ref(), annexb.as_slice());
    assert!(!frame.is_idr);
}

#[test]
fn video_frame_spots_idr_slices_after_parameter_sets() {
    let w_radl = [0u8, 0, 0, 1, 0x40, 0x01, 0, 0, 1, 0x26, 0x01, 0x55];
    let n_lp = [0u8, 0, 0, 1, 0x28, 0x01, 0x55];
    for annexb in [w_radl.as_slice(), n_lp.as_slice()] {
        let payload = [7u64.to_be_bytes().as_slice(), annexb].concat();
        let frame = VideoFrame::from_payload(Bytes::from(payload)).expect("frame");
        assert!(frame.is_idr);
    }
}

#[test]
fn video_frame_rejects_payloads_shorter_than_the_capture_prefix() {
    assert!(VideoFrame::from_payload(Bytes::from_static(&[1, 2, 3])).is_none());
}

#[test]
fn fresh_gate_drops_p_frames_until_the_first_idr() {
    let mut gate = IdrGate::new();
    assert!(!gate.admit(false));
    assert!(gate.admit(true));
    assert!(gate.admit(false));
}

#[test]
fn fresh_gate_asks_for_an_idr_once() {
    let mut gate = IdrGate::new();
    let t0 = Instant::now();
    assert!(gate.take_idr_request(t0));
    assert!(!gate.take_idr_request(t0 + Duration::from_millis(100)));
}

#[test]
fn gate_asks_again_when_no_idr_arrives_within_a_second() {
    let mut gate = IdrGate::new();
    let t0 = Instant::now();
    assert!(gate.take_idr_request(t0));
    assert!(!gate.take_idr_request(t0 + Duration::from_millis(900)));
    assert!(gate.take_idr_request(t0 + Duration::from_millis(1100)));
}

#[test]
fn loss_drops_p_frames_and_asks_for_an_idr() {
    let mut gate = IdrGate::new();
    let t0 = Instant::now();
    assert!(gate.take_idr_request(t0));
    assert!(gate.admit(true));
    assert!(gate.admit(false));
    assert!(!gate.take_idr_request(t0 + Duration::from_secs(5)));

    let t1 = t0 + Duration::from_secs(6);
    gate.note_loss(t1);
    assert!(!gate.admit(false));
    assert!(gate.take_idr_request(t1));
    assert!(!gate.take_idr_request(t1 + Duration::from_millis(200)));
    assert!(gate.admit(true));
    assert!(gate.admit(false));
}

#[test]
fn second_loss_while_waiting_does_not_ask_twice_inside_a_second() {
    let mut gate = IdrGate::new();
    let t0 = Instant::now();
    assert!(gate.take_idr_request(t0));
    gate.note_loss(t0 + Duration::from_millis(300));
    assert!(!gate.take_idr_request(t0 + Duration::from_millis(300)));
}

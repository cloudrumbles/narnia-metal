mod support;

use std::time::{Duration, Instant};

use narnia_ios::{InputWireEvent, Session};
use support::{idr_annexb, p_annexb, payload, FakeBox};

const CONNECT: Duration = Duration::from_secs(5);

fn connect(fake: &FakeBox) -> Session {
    Session::connect(&fake.url, fake.hash(), CONNECT).expect("session connects")
}

#[test]
fn connect_reports_the_stream_size_from_hello() {
    let fake = FakeBox::start(960, 608);
    let session = connect(&fake);
    let info = session.stream_info();
    assert_eq!((info.width, info.height), (960, 608));
    assert!((info.fps - 30.0).abs() < f32::EPSILON);
}

#[test]
fn connect_refuses_a_box_whose_cert_hash_differs() {
    let fake = FakeBox::start(960, 608);
    let mut hash = fake.hash();
    hash[31] ^= 0x01;
    assert!(Session::connect(&fake.url, hash, CONNECT).is_err());
}

#[test]
fn first_video_frame_is_an_idr_with_its_capture_time() {
    let fake = FakeBox::start(960, 608);
    fake.repeat_video(vec![payload(111, &p_annexb()), payload(222, &idr_annexb())]);
    let session = connect(&fake);
    let frame = session
        .next_video(Duration::from_secs(3))
        .expect("session open")
        .expect("a frame arrived");
    assert!(frame.is_idr);
    assert_eq!(frame.capture_us, 222);
    assert_eq!(frame.annexb.as_ref(), idr_annexb().as_slice());
}

#[test]
fn session_asks_the_box_for_an_idr_while_only_p_frames_arrive() {
    let fake = FakeBox::start(960, 608);
    fake.repeat_video(vec![payload(1, &p_annexb())]);
    let session = connect(&fake);
    assert!(session.next_video(Duration::from_millis(300)).expect("session open").is_none());
    let deadline = Instant::now() + Duration::from_secs(3);
    let mut asked = false;
    while Instant::now() < deadline && !asked {
        let _ = session.next_video(Duration::from_millis(50));
        asked = matches!(fake.next_input(Duration::from_millis(50)), Some(InputWireEvent::RequestIdr));
    }
    assert!(asked);
}

#[test]
fn p_frames_flow_once_an_idr_has_arrived() {
    let fake = FakeBox::start(960, 608);
    fake.repeat_video(vec![payload(10, &idr_annexb()), payload(20, &p_annexb())]);
    let session = connect(&fake);
    let deadline = Instant::now() + Duration::from_secs(3);
    let mut seen_idr = false;
    let mut seen_p_after_idr = false;
    while Instant::now() < deadline && !seen_p_after_idr {
        if let Some(frame) = session.next_video(Duration::from_millis(100)).expect("session open") {
            if frame.is_idr {
                seen_idr = true;
            } else if seen_idr {
                seen_p_after_idr = frame.capture_us == 20;
            }
        }
    }
    assert!(seen_p_after_idr);
}

#[test]
fn audio_packets_arrive_unchanged() {
    let fake = FakeBox::start(960, 608);
    let opus = vec![0xfc, 0xff, 0xfe, 0x01, 0x02, 0x03];
    fake.repeat_audio(vec![opus.clone()]);
    let session = connect(&fake);
    let packet = session
        .next_audio(Duration::from_secs(3))
        .expect("session open")
        .expect("a packet arrived");
    assert_eq!(packet.as_ref(), opus.as_slice());
}

#[test]
fn input_events_reach_the_box_intact() {
    let fake = FakeBox::start(960, 608);
    let session = connect(&fake);
    let events = vec![
        InputWireEvent::Keyboard { scancode: 30, pressed: true, repeat: false },
        InputWireEvent::PointerRelative { dx: 3.5, dy: -2.0 },
        InputWireEvent::PointerAbsolute { x: 480.0, y: 304.0, width: 960, height: 608 },
        InputWireEvent::MouseButton { button: 0x110, pressed: true },
        InputWireEvent::MouseScroll { dx: 0.0, dy: 1.0 },
        InputWireEvent::Gamepad { buttons: 0b101, axes: [100, -100, 0, 32767, -32768, 0] },
    ];
    for event in events {
        session.send(event.clone()).expect("send");
        let got = fake.next_input_other_than_idr_request(Duration::from_secs(2));
        assert_eq!(got, Some(event));
    }
}

#[test]
fn next_video_reports_closed_after_the_box_hangs_up() {
    let fake = FakeBox::start(960, 608);
    fake.repeat_video(vec![payload(1, &idr_annexb())]);
    let session = connect(&fake);
    assert!(session.next_video(Duration::from_secs(3)).expect("open").is_some());
    fake.close();
    let deadline = Instant::now() + Duration::from_secs(5);
    let mut closed = false;
    while Instant::now() < deadline && !closed {
        closed = session.next_video(Duration::from_millis(100)).is_err();
    }
    assert!(closed);
}

mod support;

use std::ffi::{CStr, CString};
use std::os::raw::c_char;
use std::time::{Duration, Instant};

use narnia_ios::{
    narnia_close, narnia_connect, narnia_next_video, narnia_send_gamepad, narnia_send_key,
    narnia_stream_info, InputWireEvent, NarniaVideoFrame,
};
use support::{idr_annexb, payload, FakeBox};

fn connect(url: &str, hash: &str, error: &mut [u8]) -> *mut narnia_ios::NarniaSession {
    let url = CString::new(url).expect("url");
    let hash = CString::new(hash).expect("hash");
    unsafe {
        narnia_connect(
            url.as_ptr(),
            hash.as_ptr(),
            5000,
            error.as_mut_ptr().cast::<c_char>(),
            error.len(),
        )
    }
}

fn error_text(error: &[u8]) -> String {
    CStr::from_bytes_until_nul(error).expect("nul-terminated error").to_string_lossy().into_owned()
}

#[test]
fn ffi_connects_and_hands_out_an_idr_frame() {
    let fake = FakeBox::start(960, 608);
    fake.repeat_video(vec![payload(4242, &idr_annexb())]);
    let mut error = [0u8; 256];
    let session = connect(&fake.url, &fake.hash_hex, &mut error);
    assert!(!session.is_null(), "{}", error_text(&error));

    let info = unsafe { narnia_stream_info(session) };
    assert_eq!((info.width, info.height), (960, 608));

    let mut frame = NarniaVideoFrame { capture_us: 0, data: std::ptr::null(), len: 0, is_idr: false };
    let deadline = Instant::now() + Duration::from_secs(3);
    let mut got = 0;
    while Instant::now() < deadline && got != 1 {
        got = unsafe { narnia_next_video(session, 100, &mut frame) };
        assert!(got >= 0);
    }
    assert_eq!(got, 1);
    assert!(frame.is_idr);
    assert_eq!(frame.capture_us, 4242);
    let data = unsafe { std::slice::from_raw_parts(frame.data, frame.len) };
    assert_eq!(data, idr_annexb().as_slice());

    assert_eq!(unsafe { narnia_send_key(session, 57, true, false) }, 0);
    assert_eq!(
        fake.next_input_other_than_idr_request(Duration::from_secs(2)),
        Some(InputWireEvent::Keyboard { scancode: 57, pressed: true, repeat: false })
    );
    let axes: [i16; 6] = [1, 2, 3, 4, 5, 6];
    assert_eq!(unsafe { narnia_send_gamepad(session, 7, axes.as_ptr()) }, 0);
    assert_eq!(
        fake.next_input_other_than_idr_request(Duration::from_secs(2)),
        Some(InputWireEvent::Gamepad { buttons: 7, axes })
    );

    unsafe { narnia_close(session) };
}

#[test]
fn ffi_connect_with_a_wrong_hash_returns_null_and_says_why() {
    let fake = FakeBox::start(960, 608);
    let wrong = "00".repeat(32);
    let mut error = [0u8; 256];
    let session = connect(&fake.url, &wrong, &mut error);
    assert!(session.is_null());
    assert!(!error_text(&error).is_empty());
}

#[test]
fn ffi_connect_with_a_malformed_hash_mentions_the_hash() {
    let mut error = [0u8; 256];
    let session = connect("https://127.0.0.1:9", "xyz", &mut error);
    assert!(session.is_null());
    assert!(error_text(&error).to_lowercase().contains("hash"));
}

#![allow(clippy::missing_safety_doc)]

use std::ffi::{c_char, CStr};
use std::sync::{Mutex, PoisonError};
use std::time::Duration;

use bytes::Bytes;

use crate::cert::parse_cert_hash;
use crate::session::{ConnectError, Session};
use narnia_transport::protocol::InputWireEvent;

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct NarniaStreamInfo {
    pub width: u32,
    pub height: u32,
    pub fps: f32,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct NarniaVideoFrame {
    pub capture_us: u64,
    pub data: *const u8,
    pub len: usize,
    pub is_idr: bool,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct NarniaAudioPacket {
    pub data: *const u8,
    pub len: usize,
}

pub struct NarniaSession {
    session: Session,
    last_video: Mutex<Bytes>,
    last_audio: Mutex<Bytes>,
}

const CLOSED: i32 = -1;
const TIMED_OUT: i32 = 0;
const DELIVERED: i32 = 1;
const SENT: i32 = 0;
const FAILED: i32 = -1;

unsafe fn session_ref<'a>(session: *const NarniaSession) -> Option<&'a NarniaSession> {
    unsafe { session.as_ref() }
}

unsafe fn text_arg<'a>(arg: *const c_char, name: &str) -> Result<&'a str, String> {
    if arg.is_null() {
        return Err(format!("{name} is null"));
    }
    unsafe { CStr::from_ptr(arg) }
        .to_str()
        .map_err(|_| format!("{name} is not valid utf-8"))
}

unsafe fn write_error(error: *mut c_char, error_len: usize, message: &str) {
    if error.is_null() || error_len == 0 {
        return;
    }
    let mut kept = message.len().min(error_len - 1);
    while !message.is_char_boundary(kept) {
        kept -= 1;
    }
    let buffer = unsafe { std::slice::from_raw_parts_mut(error.cast::<u8>(), error_len) };
    buffer[..kept].copy_from_slice(&message.as_bytes()[..kept]);
    buffer[kept] = 0;
}

unsafe fn connect(
    url: *const c_char,
    cert_sha256_hex: *const c_char,
    timeout_ms: u32,
) -> Result<NarniaSession, String> {
    let url = unsafe { text_arg(url, "url") }?;
    let hex = unsafe { text_arg(cert_sha256_hex, "certificate hash") }?;
    let cert_sha256 = parse_cert_hash(hex).map_err(|error| error.to_string())?;
    let timeout = Duration::from_millis(timeout_ms.into());
    let session = Session::connect(url, cert_sha256, timeout)
        .map_err(|error: ConnectError| error.to_string())?;
    Ok(NarniaSession {
        session,
        last_video: Mutex::default(),
        last_audio: Mutex::default(),
    })
}

#[no_mangle]
pub unsafe extern "C" fn narnia_connect(
    url: *const c_char,
    cert_sha256_hex: *const c_char,
    timeout_ms: u32,
    error: *mut c_char,
    error_len: usize,
) -> *mut NarniaSession {
    match unsafe { connect(url, cert_sha256_hex, timeout_ms) } {
        Ok(session) => Box::into_raw(Box::new(session)),
        Err(message) => {
            unsafe { write_error(error, error_len, &message) };
            std::ptr::null_mut()
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn narnia_stream_info(session: *const NarniaSession) -> NarniaStreamInfo {
    match unsafe { session_ref(session) } {
        Some(state) => {
            let info = state.session.stream_info();
            NarniaStreamInfo {
                width: info.width,
                height: info.height,
                fps: info.fps,
            }
        }
        None => NarniaStreamInfo {
            width: 0,
            height: 0,
            fps: 0.0,
        },
    }
}

#[no_mangle]
pub unsafe extern "C" fn narnia_next_video(
    session: *mut NarniaSession,
    timeout_ms: u32,
    out: *mut NarniaVideoFrame,
) -> i32 {
    let Some(state) = (unsafe { session_ref(session) }) else {
        return CLOSED;
    };
    if out.is_null() {
        return CLOSED;
    }
    match state
        .session
        .next_video(Duration::from_millis(timeout_ms.into()))
    {
        Err(_) => CLOSED,
        Ok(None) => TIMED_OUT,
        Ok(Some(frame)) => {
            let mut last = state
                .last_video
                .lock()
                .unwrap_or_else(PoisonError::into_inner);
            *last = frame.annexb;
            let filled = NarniaVideoFrame {
                capture_us: frame.capture_us,
                data: last.as_ptr(),
                len: last.len(),
                is_idr: frame.is_idr,
            };
            unsafe { out.write(filled) };
            DELIVERED
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn narnia_next_audio(
    session: *mut NarniaSession,
    timeout_ms: u32,
    out: *mut NarniaAudioPacket,
) -> i32 {
    let Some(state) = (unsafe { session_ref(session) }) else {
        return CLOSED;
    };
    if out.is_null() {
        return CLOSED;
    }
    match state
        .session
        .next_audio(Duration::from_millis(timeout_ms.into()))
    {
        Err(_) => CLOSED,
        Ok(None) => TIMED_OUT,
        Ok(Some(packet)) => {
            let mut last = state
                .last_audio
                .lock()
                .unwrap_or_else(PoisonError::into_inner);
            *last = packet;
            let filled = NarniaAudioPacket {
                data: last.as_ptr(),
                len: last.len(),
            };
            unsafe { out.write(filled) };
            DELIVERED
        }
    }
}

unsafe fn send(session: *mut NarniaSession, event: InputWireEvent) -> i32 {
    match unsafe { session_ref(session) } {
        Some(state) => match state.session.send(event) {
            Ok(()) => SENT,
            Err(_) => FAILED,
        },
        None => FAILED,
    }
}

#[no_mangle]
pub unsafe extern "C" fn narnia_send_key(
    session: *mut NarniaSession,
    evdev_code: u32,
    pressed: bool,
    repeat: bool,
) -> i32 {
    unsafe {
        send(
            session,
            InputWireEvent::Keyboard {
                scancode: evdev_code,
                pressed,
                repeat,
            },
        )
    }
}

#[no_mangle]
pub unsafe extern "C" fn narnia_send_pointer_relative(
    session: *mut NarniaSession,
    dx: f64,
    dy: f64,
) -> i32 {
    unsafe { send(session, InputWireEvent::PointerRelative { dx, dy }) }
}

#[no_mangle]
pub unsafe extern "C" fn narnia_send_pointer_absolute(
    session: *mut NarniaSession,
    x: f64,
    y: f64,
    width: u32,
    height: u32,
) -> i32 {
    unsafe {
        send(
            session,
            InputWireEvent::PointerAbsolute {
                x,
                y,
                width,
                height,
            },
        )
    }
}

#[no_mangle]
pub unsafe extern "C" fn narnia_send_mouse_button(
    session: *mut NarniaSession,
    button: u16,
    pressed: bool,
) -> i32 {
    unsafe { send(session, InputWireEvent::MouseButton { button, pressed }) }
}

#[no_mangle]
pub unsafe extern "C" fn narnia_send_scroll(session: *mut NarniaSession, dx: f64, dy: f64) -> i32 {
    unsafe { send(session, InputWireEvent::MouseScroll { dx, dy }) }
}

#[no_mangle]
pub unsafe extern "C" fn narnia_send_gamepad(
    session: *mut NarniaSession,
    buttons: u32,
    axes: *const i16,
) -> i32 {
    if axes.is_null() {
        return FAILED;
    }
    let axes = unsafe { axes.cast::<[i16; 6]>().read() };
    unsafe { send(session, InputWireEvent::Gamepad { buttons, axes }) }
}

#[no_mangle]
pub unsafe extern "C" fn narnia_close(session: *mut NarniaSession) {
    if !session.is_null() {
        drop(unsafe { Box::from_raw(session) });
    }
}

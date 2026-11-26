mod cert;
mod ffi;
mod frame;
mod gate;
mod session;

pub use cert::{parse_cert_hash, CertHashError};
pub use ffi::{
    narnia_close, narnia_connect, narnia_next_audio, narnia_next_video, narnia_send_gamepad,
    narnia_send_key, narnia_send_mouse_button, narnia_send_pointer_absolute,
    narnia_send_pointer_relative, narnia_send_scroll, narnia_stream_info, NarniaAudioPacket,
    NarniaSession, NarniaStreamInfo, NarniaVideoFrame,
};
pub use frame::VideoFrame;
pub use gate::IdrGate;
pub use narnia_transport::protocol::InputWireEvent;
pub use session::{Closed, ConnectError, Session, StreamInfo};

use std::sync::{Mutex, MutexGuard, PoisonError};
use std::time::{Duration, Instant};

use bytes::Bytes;
use narnia_transport::client::{Client, InputPublisher, TrackReceiver};
use narnia_transport::protocol::InputWireEvent;
use tokio::runtime::Runtime;

use crate::frame::VideoFrame;
use crate::gate::IdrGate;

const IDLE_POLL: Duration = Duration::from_millis(50);

#[derive(Debug, thiserror::Error)]
#[error("session closed")]
pub struct Closed;

#[derive(Debug, thiserror::Error)]
pub enum ConnectError {
    #[error("could not start the network runtime: {0}")]
    Runtime(std::io::Error),
    #[error("could not build the pinned client: {0}")]
    Client(String),
    #[error("could not connect: {0}")]
    Connect(String),
    #[error("could not open the input channel: {0}")]
    Input(String),
    #[error("no connection after {0:?}")]
    Timeout(Duration),
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct StreamInfo {
    pub width: u32,
    pub height: u32,
    pub fps: f32,
}

struct VideoState {
    receiver: TrackReceiver,
    seen_losses: u64,
    gate: IdrGate,
}

impl VideoState {
    fn observe_loss(&mut self, now: Instant) {
        let lost = self.receiver.lost_count();
        if lost > self.seen_losses {
            self.seen_losses = lost;
            self.gate.note_loss(now);
        }
    }
}

struct Link {
    info: StreamInfo,
    video: VideoState,
    audio: TrackReceiver,
    input: InputPublisher,
}

pub struct Session {
    runtime: Runtime,
    info: StreamInfo,
    video: Mutex<VideoState>,
    audio: Mutex<TrackReceiver>,
    input: tokio::sync::Mutex<InputPublisher>,
}

const _: fn() = || {
    fn assert_send_sync<T: Send + Sync>() {}
    assert_send_sync::<Session>();
};

fn locked<T>(mutex: &Mutex<T>) -> MutexGuard<'_, T> {
    mutex.lock().unwrap_or_else(PoisonError::into_inner)
}

async fn establish(url: String, cert_sha256: [u8; 32]) -> Result<Link, ConnectError> {
    let client =
        Client::pinned(cert_sha256).map_err(|error| ConnectError::Client(format!("{error:#}")))?;
    let subscription = client
        .connect(&url)
        .await
        .map_err(|error| ConnectError::Connect(format!("{error:#}")))?;
    let hello = subscription.hello();
    let info = StreamInfo {
        width: hello.width,
        height: hello.height,
        fps: hello.fps,
    };
    let input = subscription
        .clone_for_input()
        .open_input_publisher()
        .await
        .map_err(|error| ConnectError::Input(format!("{error:#}")))?;
    let (video, audio) = subscription.split();
    Ok(Link {
        info,
        video: VideoState {
            receiver: video,
            seen_losses: 0,
            gate: IdrGate::new(),
        },
        audio,
        input,
    })
}

impl Session {
    pub fn connect(
        url: &str,
        cert_sha256: [u8; 32],
        timeout: Duration,
    ) -> Result<Session, ConnectError> {
        let runtime = tokio::runtime::Builder::new_multi_thread()
            .worker_threads(2)
            .enable_all()
            .build()
            .map_err(ConnectError::Runtime)?;
        let link = runtime
            .block_on(async {
                tokio::time::timeout(timeout, establish(url.to_owned(), cert_sha256)).await
            })
            .map_err(|_| ConnectError::Timeout(timeout))??;
        Ok(Session {
            runtime,
            info: link.info,
            video: Mutex::new(link.video),
            audio: Mutex::new(link.audio),
            input: tokio::sync::Mutex::new(link.input),
        })
    }

    pub fn stream_info(&self) -> StreamInfo {
        self.info
    }

    pub fn next_video(&self, timeout: Duration) -> Result<Option<VideoFrame>, Closed> {
        let mut video = locked(&self.video);
        self.runtime
            .block_on(self.receive_video(&mut video, timeout))
    }

    pub fn next_audio(&self, timeout: Duration) -> Result<Option<Bytes>, Closed> {
        let mut audio = locked(&self.audio);
        self.runtime.block_on(async {
            match tokio::time::timeout(timeout, audio.recv()).await {
                Err(_) => Ok(None),
                Ok(None) => Err(Closed),
                Ok(Some(packet)) => Ok(Some(packet.payload)),
            }
        })
    }

    pub fn send(&self, event: InputWireEvent) -> Result<(), Closed> {
        self.runtime.block_on(self.send_event(event))
    }

    async fn send_event(&self, event: InputWireEvent) -> Result<(), Closed> {
        self.input
            .lock()
            .await
            .send(event)
            .await
            .map_err(|_| Closed)
    }

    async fn receive_video(
        &self,
        video: &mut VideoState,
        timeout: Duration,
    ) -> Result<Option<VideoFrame>, Closed> {
        let deadline = Instant::now() + timeout;
        loop {
            let now = Instant::now();
            video.observe_loss(now);
            if video.gate.take_idr_request(now) {
                self.send_event(InputWireEvent::RequestIdr).await?;
            }
            let Some(left) = deadline.checked_duration_since(now) else {
                return Ok(None);
            };
            match tokio::time::timeout(left.min(IDLE_POLL), video.receiver.recv()).await {
                Err(_) => {}
                Ok(None) => return Err(Closed),
                Ok(Some(recovered)) => {
                    video.observe_loss(Instant::now());
                    let admitted = VideoFrame::from_payload(recovered.payload)
                        .filter(|frame| video.gate.admit(frame.is_idr));
                    if admitted.is_some() {
                        return Ok(admitted);
                    }
                }
            }
        }
    }
}

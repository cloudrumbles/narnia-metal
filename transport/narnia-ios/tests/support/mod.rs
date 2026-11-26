#![allow(dead_code)]

use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{mpsc, Arc, Mutex};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

use narnia_transport::protocol::InputWireEvent;
use narnia_transport::server::Server;

pub const VPS: [u8; 6] = [0, 0, 0, 1, 0x40, 0x01];
pub const IDR_SLICE: [u8; 8] = [0, 0, 0, 1, 0x26, 0x01, 0xaf, 0x11];
pub const P_SLICE: [u8; 8] = [0, 0, 0, 1, 0x02, 0x01, 0xd0, 0x22];

pub fn idr_annexb() -> Vec<u8> {
    [VPS.as_slice(), IDR_SLICE.as_slice()].concat()
}

pub fn p_annexb() -> Vec<u8> {
    P_SLICE.to_vec()
}

pub fn payload(capture_us: u64, annexb: &[u8]) -> Vec<u8> {
    [capture_us.to_be_bytes().as_slice(), annexb].concat()
}

pub fn hash_bytes(hex: &str) -> [u8; 32] {
    let mut out = [0u8; 32];
    for (i, byte) in out.iter_mut().enumerate() {
        *byte = u8::from_str_radix(&hex[2 * i..2 * i + 2], 16).expect("server hash is hex");
    }
    out
}

pub struct FakeBox {
    pub url: String,
    pub hash_hex: String,
    video: Arc<Mutex<Vec<Vec<u8>>>>,
    audio: Arc<Mutex<Vec<Vec<u8>>>>,
    inputs: mpsc::Receiver<InputWireEvent>,
    stop: Arc<AtomicBool>,
    thread: Option<JoinHandle<()>>,
}

async fn wait_for(flag: Arc<AtomicBool>) {
    while !flag.load(Ordering::Relaxed) {
        tokio::time::sleep(Duration::from_millis(20)).await;
    }
}

impl FakeBox {
    pub fn start(width: u32, height: u32) -> FakeBox {
        let (ready_tx, ready_rx) = mpsc::channel::<(String, String)>();
        let (input_tx, input_rx) = mpsc::channel::<InputWireEvent>();
        let video = Arc::new(Mutex::new(Vec::<Vec<u8>>::new()));
        let audio = Arc::new(Mutex::new(Vec::<Vec<u8>>::new()));
        let stop = Arc::new(AtomicBool::new(false));
        let (video_loop, audio_loop, stop_loop) = (video.clone(), audio.clone(), stop.clone());
        let thread = std::thread::spawn(move || {
            let runtime = tokio::runtime::Builder::new_multi_thread()
                .worker_threads(2)
                .enable_all()
                .build()
                .expect("fake box runtime");
            runtime.block_on(async move {
                let mut server =
                    Server::bind("127.0.0.1:0".parse().expect("addr"), vec![]).expect("bind");
                let port = server.local_addr().expect("local addr").port();
                ready_tx
                    .send((format!("https://127.0.0.1:{port}"), server.cert_sha256_hex()))
                    .expect("announce fake box");
                let handle = tokio::select! {
                    accepted = server.accept(width, height, 30.0) => match accepted {
                        Ok(handle) => handle,
                        Err(_) => return,
                    },
                    _ = wait_for(stop_loop.clone()) => return,
                };
                if let Some(mut events) = handle.take_input_receiver().await {
                    tokio::spawn(async move {
                        while let Some(event) = events.recv().await {
                            if input_tx.send(event).is_err() {
                                break;
                            }
                        }
                    });
                }
                let video_publisher = handle.video_publisher();
                let audio_publisher = handle.audio_publisher();
                while !stop_loop.load(Ordering::Relaxed) {
                    let frames = video_loop.lock().expect("video list").clone();
                    for frame in frames {
                        let _ = video_publisher.send(&frame);
                    }
                    let packets = audio_loop.lock().expect("audio list").clone();
                    for packet in packets {
                        let _ = audio_publisher.send(&packet);
                    }
                    tokio::time::sleep(Duration::from_millis(20)).await;
                }
                handle.close(0, "fake box closed");
                tokio::time::sleep(Duration::from_millis(200)).await;
            });
        });
        let (url, hash_hex) = ready_rx
            .recv_timeout(Duration::from_secs(5))
            .expect("fake box came up");
        FakeBox { url, hash_hex, video, audio, inputs: input_rx, stop, thread: Some(thread) }
    }

    pub fn hash(&self) -> [u8; 32] {
        hash_bytes(&self.hash_hex)
    }

    pub fn repeat_video(&self, payloads: Vec<Vec<u8>>) {
        *self.video.lock().expect("video list") = payloads;
    }

    pub fn repeat_audio(&self, packets: Vec<Vec<u8>>) {
        *self.audio.lock().expect("audio list") = packets;
    }

    pub fn next_input(&self, timeout: Duration) -> Option<InputWireEvent> {
        self.inputs.recv_timeout(timeout).ok()
    }

    pub fn next_input_other_than_idr_request(&self, timeout: Duration) -> Option<InputWireEvent> {
        let deadline = Instant::now() + timeout;
        loop {
            let left = deadline.checked_duration_since(Instant::now())?;
            match self.inputs.recv_timeout(left).ok()? {
                InputWireEvent::RequestIdr => continue,
                event => return Some(event),
            }
        }
    }

    pub fn close(&self) {
        self.stop.store(true, Ordering::Relaxed);
    }
}

impl Drop for FakeBox {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::Relaxed);
        if let Some(thread) = self.thread.take() {
            let _ = thread.join();
        }
    }
}

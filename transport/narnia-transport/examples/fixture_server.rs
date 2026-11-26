//! `fixture_server` — a narnia server that needs no GPU.
//!
//! Streams a recorded Annex-B HEVC file (and optionally an Ogg/Opus file)
//! through the real `narnia_transport::Server`, with the same framing
//! narnia-server uses: an 8-byte big-endian capture timestamp in front of
//! every access unit, keyframes at higher FEC redundancy, on-demand
//! repair for client `deficit` NACKs, and `RequestIdr` handled by
//! restarting from the first keyframe. Every input event the client sends
//! is logged, which makes it a test bench for clients (narnia-web in
//! particular) without renting a GPU box.
//!
//! ```text
//! cargo run -p narnia-transport --example fixture_server -- \
//!     --video narnia-client/fixtures/mock_1080p60_hevc_5s_x265nob.h265 \
//!     [--audio tone.opus] [--bind [::]:4443] [--fps 60] [--cert-cache DIR] [--san HOST]
//! ```

use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use anyhow::{bail, Context, Result};
use narnia_transport::{InputWireEvent, Server};

struct Args {
    bind: std::net::SocketAddr,
    video: PathBuf,
    audio: Option<PathBuf>,
    fps: f64,
    sans: Vec<String>,
    cert_cache: Option<PathBuf>,
    redundancy: f32,
    keyframe_redundancy: f32,
}

fn parse_args() -> Result<Args> {
    let mut a = Args {
        bind: "[::]:4443".parse()?,
        video: PathBuf::new(),
        audio: None,
        fps: 60.0,
        sans: Vec::new(),
        cert_cache: None,
        redundancy: 1.0,
        keyframe_redundancy: 2.0,
    };
    let mut it = std::env::args().skip(1);
    while let Some(flag) = it.next() {
        let mut val = || it.next().with_context(|| format!("{flag} needs a value"));
        match flag.as_str() {
            "--bind" => a.bind = val()?.parse()?,
            "--video" => a.video = val()?.into(),
            "--audio" => a.audio = Some(val()?.into()),
            "--fps" => a.fps = val()?.parse()?,
            "--san" => a.sans.push(val()?),
            "--cert-cache" => a.cert_cache = Some(val()?.into()),
            "--redundancy" => a.redundancy = val()?.parse()?,
            "--keyframe-redundancy" => a.keyframe_redundancy = val()?.parse()?,
            other => bail!("unknown flag {other}"),
        }
    }
    if a.video.as_os_str().is_empty() {
        bail!("--video <file.h265> is required");
    }
    Ok(a)
}

/// Split an Annex-B HEVC stream into access units: a new AU starts at a
/// VPS/SPS/PPS/AUD/prefix-SEI, or at a slice with
/// first_slice_segment_in_pic_flag set, once the current AU has a slice.
fn split_access_units(data: &[u8]) -> Vec<(Vec<u8>, bool)> {
    let mut starts = Vec::new();
    let mut i = 0;
    while i + 3 <= data.len() {
        if data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1 {
            let begin = if i > 0 && data[i - 1] == 0 { i - 1 } else { i };
            starts.push((begin, i + 3));
            i += 3;
        } else {
            i += 1;
        }
    }
    let mut aus = Vec::new();
    let mut cur_start: Option<usize> = None;
    let mut cur_has_vcl = false;
    let mut cur_idr = false;
    for (n, &(begin, hdr)) in starts.iter().enumerate() {
        let Some(&b0) = data.get(hdr) else { break };
        let t = (b0 >> 1) & 0x3f;
        let is_vcl = t < 32;
        let first_slice = is_vcl && data.get(hdr + 2).map(|b| b & 0x80 != 0).unwrap_or(false);
        let starts_au = (matches!(t, 32..=35 | 39) || first_slice) && cur_has_vcl;
        if starts_au {
            if let Some(s) = cur_start {
                aus.push((data[s..begin].to_vec(), cur_idr));
            }
            cur_start = Some(begin);
            cur_has_vcl = false;
            cur_idr = false;
        }
        if cur_start.is_none() {
            cur_start = Some(begin);
        }
        cur_has_vcl |= is_vcl;
        cur_idr |= t == 19 || t == 20;
        if n + 1 == starts.len() {
            aus.push((data[cur_start.unwrap()..].to_vec(), cur_idr));
        }
    }
    aus
}

/// Extract Opus packets from an Ogg file, skipping OpusHead / OpusTags.
fn ogg_opus_packets(data: &[u8]) -> Result<Vec<Vec<u8>>> {
    let mut packets = Vec::new();
    let mut partial = Vec::new();
    let mut pos = 0;
    while pos + 27 <= data.len() {
        if &data[pos..pos + 4] != b"OggS" {
            bail!("bad ogg page at {pos}");
        }
        let nseg = data[pos + 26] as usize;
        let table = &data[pos + 27..pos + 27 + nseg];
        let mut body = pos + 27 + nseg;
        for &lace in table {
            partial.extend_from_slice(&data[body..body + lace as usize]);
            body += lace as usize;
            if lace < 255 {
                packets.push(std::mem::take(&mut partial));
            }
        }
        pos = body;
    }
    packets.retain(|p| !p.starts_with(b"OpusHead") && !p.starts_with(b"OpusTags"));
    Ok(packets)
}

fn now_us() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_micros() as u64)
        .unwrap_or(0)
}

#[tokio::main]
async fn main() -> Result<()> {
    let args = parse_args()?;
    let video = std::fs::read(&args.video).with_context(|| format!("reading {:?}", args.video))?;
    let aus = split_access_units(&video);
    let idrs = aus.iter().filter(|(_, idr)| *idr).count();
    if aus.is_empty() || !aus[0].1 {
        bail!("video must start with an IDR access unit");
    }
    let audio = match &args.audio {
        Some(p) => ogg_opus_packets(&std::fs::read(p)?)?,
        None => Vec::new(),
    };
    eprintln!(
        "[fixture] {} access units ({} IDR), {} opus packets",
        aus.len(),
        idrs,
        audio.len()
    );

    let mut server =
        Server::bind_with_cert_cache(args.bind, args.sans.clone(), args.cert_cache.clone())?;
    let hash = server.cert_sha256_hex();
    eprintln!("[fixture] listening on {} — cert sha256 {hash}", args.bind);
    eprintln!(
        "[fixture] browser: <client page>#server=https://HOST:{}&hash={hash}",
        args.bind.port()
    );

    // Reading the size from the SPS needs a full SPS parser; the repo's
    // fixtures are all 1080p.
    let (width, height) = (1920, 1080);
    let aus = Arc::new(aus);
    let audio = Arc::new(audio);
    let cfg = Arc::new(args);
    // One client at a time, but a NEW client always wins: accept keeps
    // running while a session streams, and a fresh connection replaces
    // the old session immediately. Otherwise a browser that vanished
    // without closing (killed tab, refresh) would block reconnects until
    // QUIC's idle timeout noticed.
    let mut current: Option<tokio::task::JoinHandle<()>> = None;
    loop {
        let session = server.accept(width, height, cfg.fps as f32).await?;
        if let Some(old) = current.take() {
            eprintln!("[fixture] new client — dropping the previous session");
            old.abort();
        }
        eprintln!("[fixture] client connected");
        let (aus, audio, cfg) = (aus.clone(), audio.clone(), cfg.clone());
        current = Some(tokio::spawn(async move {
            if let Err(e) = run_session(session, aus, audio, cfg).await {
                eprintln!("[fixture] session error: {e:?}");
            }
        }));
    }
}

async fn run_session(
    session: narnia_transport::SessionHandle,
    aus: Arc<Vec<(Vec<u8>, bool)>>,
    audio: Arc<Vec<Vec<u8>>>,
    cfg: Arc<Args>,
) -> Result<()> {
    let video_pub = session.video_publisher();
    let audio_pub = session.audio_publisher();
    let restart = Arc::new(AtomicBool::new(false));

    // Input: log everything, honour RequestIdr.
    let mut input_rx = session.take_input_receiver().await.context("input rx")?;
    let restart_in = restart.clone();
    let input_task = tokio::spawn(async move {
        while let Some(ev) = input_rx.recv().await {
            match ev {
                InputWireEvent::RequestIdr => {
                    eprintln!("[fixture] input: RequestIdr → restarting at keyframe");
                    restart_in.store(true, Ordering::Relaxed);
                }
                InputWireEvent::FrameLost { block } => {
                    eprintln!("[fixture] input: FrameLost({block}) → restarting at keyframe");
                    restart_in.store(true, Ordering::Relaxed);
                }
                InputWireEvent::PointerAbsolute { .. } | InputWireEvent::PointerRelative { .. } => {
                    tracing_debug(&format!("[fixture] input: {ev:?}"));
                }
                other => eprintln!("[fixture] input: {other:?}"),
            }
        }
    });

    // On-demand repair for NACKed blocks (what narnia-relay does).
    let mut deficits = session.take_deficit_events().await.context("deficit rx")?;
    let repair_pub = video_pub.clone();
    let deficit_task = tokio::spawn(async move {
        let mut n = 0u64;
        while let Some(d) = deficits.recv().await {
            n += 1;
            let sent = repair_pub
                .emit_extra_repair(d.block_id, d.missing() + 2)
                .unwrap_or(0);
            if n <= 20 || n % 100 == 0 {
                eprintln!(
                    "[fixture] deficit block={} {}/{} → {} repair symbols (total {n})",
                    d.block_id, d.k_received, d.k_total, sent
                );
            }
        }
    });

    // Audio pump: one 20 ms opus packet per tick, looping.
    let audio_task = {
        let audio = audio.clone();
        tokio::spawn(async move {
            if audio.is_empty() {
                return;
            }
            let mut tick = tokio::time::interval(Duration::from_millis(20));
            for i in 0.. {
                tick.tick().await;
                if audio_pub.send(&audio[i % audio.len()]).is_err() {
                    return;
                }
            }
        })
    };

    let _children = AbortOnDrop(vec![
        input_task.abort_handle(),
        deficit_task.abort_handle(),
        audio_task.abort_handle(),
    ]);

    // Video pump.
    let mut tick = tokio::time::interval(Duration::from_secs_f64(1.0 / cfg.fps));
    tick.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);
    let mut idx = 0usize;
    let mut sent = 0u64;
    loop {
        tick.tick().await;
        if restart.swap(false, Ordering::Relaxed) {
            idx = 0;
        }
        let (au, is_idr) = &aus[idx % aus.len()];
        idx = (idx + 1) % aus.len();
        let mut framed = Vec::with_capacity(8 + au.len());
        framed.extend_from_slice(&now_us().to_be_bytes());
        framed.extend_from_slice(au);
        let r = if *is_idr {
            cfg.keyframe_redundancy
        } else {
            cfg.redundancy
        };
        if video_pub.send_with_redundancy(&framed, r).is_err() {
            break;
        }
        sent += 1;
        if sent % (cfg.fps as u64 * 5) == 0 {
            eprintln!(
                "[fixture] sent {sent} frames, rtt={:?}",
                video_pub.cc_stats().smoothed_rtt_us
            );
        }
    }
    eprintln!("[fixture] client gone after {sent} frames");
    drop(_children);
    Ok(())
}

/// Aborts the per-session helper tasks when the session task ends —
/// including when it is itself aborted because a new client took over.
/// Without this the audio pump would keep the old session alive.
struct AbortOnDrop(Vec<tokio::task::AbortHandle>);

impl Drop for AbortOnDrop {
    fn drop(&mut self) {
        for h in &self.0 {
            h.abort();
        }
    }
}

fn tracing_debug(msg: &str) {
    if std::env::var_os("FIXTURE_VERBOSE").is_some() {
        eprintln!("{msg}");
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn splits_on_first_slice_flag() {
        // VPS SPS PPS IDR(first) | TRAIL(first) | TRAIL(first)
        let mut s = Vec::new();
        for (t, first) in [
            (32u8, false),
            (33, false),
            (34, false),
            (20, true),
            (1, true),
            (1, true),
        ] {
            s.extend_from_slice(&[0, 0, 0, 1, t << 1, 1, if first { 0x80 } else { 0 }, 0xaa]);
        }
        let aus = split_access_units(&s);
        assert_eq!(aus.len(), 3);
        assert!(aus[0].1 && !aus[1].1 && !aus[2].1);
        assert!(aus[0].0.starts_with(&[0, 0, 0, 1, 64]));
    }
}

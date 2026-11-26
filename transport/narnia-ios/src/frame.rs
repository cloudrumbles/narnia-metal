use bytes::Bytes;

const CAPTURE_PREFIX_LEN: usize = 8;
const NAL_IDR_W_RADL: u8 = 19;
const NAL_IDR_N_LP: u8 = 20;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct VideoFrame {
    pub capture_us: u64,
    pub annexb: Bytes,
    pub is_idr: bool,
}

impl VideoFrame {
    pub fn from_payload(payload: Bytes) -> Option<VideoFrame> {
        let (prefix, _) = payload.split_first_chunk::<CAPTURE_PREFIX_LEN>()?;
        let capture_us = u64::from_be_bytes(*prefix);
        let annexb = payload.slice(CAPTURE_PREFIX_LEN..);
        let is_idr = contains_idr_slice(&annexb);
        Some(VideoFrame {
            capture_us,
            annexb,
            is_idr,
        })
    }
}

fn contains_idr_slice(annexb: &[u8]) -> bool {
    annexb.windows(4).any(|window| {
        window[..3] == [0, 0, 1] && matches!((window[3] >> 1) & 0x3f, NAL_IDR_W_RADL | NAL_IDR_N_LP)
    })
}

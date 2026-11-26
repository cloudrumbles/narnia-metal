//! Self-signed certificate generation for tailnet-resident servers.
//!
//! Tailnet identity replaces CA trust here — a node listing in `tailscale
//! status` IS the proof that we're talking to the right machine. So our
//! TLS layer just needs SOME cert to make QUIC happy; the native client
//! passes `--insecure` (`with_no_certificate_verification`) to skip CA
//! validation.
//!
//! ## Browser constraints
//!
//! Browsers can't skip verification, but WebTransport lets a page pin a
//! cert by hash (`serverCertificateHashes`). Chrome only accepts a pinned
//! cert when it is:
//!   - ECDSA P-256 (rcgen's default is fine, but we ask for it explicitly),
//!   - X.509v3,
//!   - valid for **at most 14 days** in total.
//!
//! rcgen's default validity runs to the year 4096, which browsers reject,
//! so we mint with a 13-day window ([`VALIDITY`]) starting one hour in the
//! past (clock-skew slack). [`GeneratedCert::sha256`] is the value the
//! browser client pins.
//!
//! Because the hash changes every time we mint, [`load_or_mint`] can keep
//! the cert in a cache directory so a narnia-server restart (the image's
//! entrypoint respawns it) doesn't invalidate the link you already opened.
//! The cached cert is reused until it has less than [`REMINT_BEFORE`] left.

use std::path::Path;
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use anyhow::{Context, Result};

/// Total validity window. Chrome's cap for hash-pinned certs is 14 days;
/// we stay a day under so a skewed client clock can't push us over.
pub const VALIDITY: Duration = Duration::from_secs(13 * 24 * 3600);

/// Backdate `not_before` by this much so a client whose clock runs a little
/// slow still considers the cert valid.
const BACKDATE: Duration = Duration::from_secs(3600);

/// Re-mint a cached cert once it has less than this long left, so a
/// session started just before expiry doesn't get cut off mid-game.
pub const REMINT_BEFORE: Duration = Duration::from_secs(2 * 24 * 3600);

/// Generated cert + private key pair, in DER form ready for rustls.
#[derive(Clone)]
pub struct GeneratedCert {
    pub cert_der: Vec<u8>,
    pub key_der: Vec<u8>,
    /// Subject names that the cert claims authority for.
    pub subject_alt_names: Vec<String>,
    /// Expiry, seconds since the UNIX epoch.
    pub not_after_unix: u64,
}

impl GeneratedCert {
    /// SHA-256 of the DER cert — the value a browser passes in
    /// `serverCertificateHashes`.
    pub fn sha256(&self) -> [u8; 32] {
        let digest = ring::digest::digest(&ring::digest::SHA256, &self.cert_der);
        let mut out = [0u8; 32];
        out.copy_from_slice(digest.as_ref());
        out
    }

    /// Lowercase hex form of [`Self::sha256`], for logs and URLs.
    pub fn sha256_hex(&self) -> String {
        self.sha256().iter().map(|b| format!("{b:02x}")).collect()
    }

    /// How long until the cert expires (zero if it already has).
    pub fn remaining(&self) -> Duration {
        let now = unix_now();
        Duration::from_secs(self.not_after_unix.saturating_sub(now))
    }

    /// True once the cert is inside the [`REMINT_BEFORE`] window.
    pub fn needs_remint(&self) -> bool {
        self.remaining() < REMINT_BEFORE
    }
}

fn unix_now() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0)
}

fn default_sans(subject_alt_names: Vec<String>) -> Vec<String> {
    if subject_alt_names.is_empty() {
        vec![
            "localhost".to_string(),
            "127.0.0.1".to_string(),
            "::1".to_string(),
        ]
    } else {
        subject_alt_names
    }
}

/// Mint a fresh ECDSA P-256 self-signed cert, valid for [`VALIDITY`].
/// Subject Alternative Names cover `localhost`, `127.0.0.1`, `::1` when
/// the caller passes none, otherwise exactly the caller's list (tailnet /
/// MagicDNS hostnames).
pub fn self_signed(subject_alt_names: Vec<String>) -> Result<GeneratedCert> {
    let sans = default_sans(subject_alt_names);

    let key_pair = rcgen::KeyPair::generate_for(&rcgen::PKCS_ECDSA_P256_SHA256)
        .context("generating P-256 key")?;
    let mut params = rcgen::CertificateParams::new(sans.clone()).context("building cert params")?;

    let now = SystemTime::now();
    let not_before = now - BACKDATE;
    let not_after = not_before + VALIDITY;
    params.not_before = time::OffsetDateTime::from(not_before);
    params.not_after = time::OffsetDateTime::from(not_after);

    let cert = params
        .self_signed(&key_pair)
        .context("rcgen failed to self-sign cert")?;

    Ok(GeneratedCert {
        cert_der: cert.der().to_vec(),
        key_der: key_pair.serialize_der(),
        subject_alt_names: sans,
        not_after_unix: not_after
            .duration_since(UNIX_EPOCH)
            .map(|d| d.as_secs())
            .unwrap_or(0),
    })
}

const CERT_FILE: &str = "narnia-cert.der";
const KEY_FILE: &str = "narnia-key.der";
const META_FILE: &str = "narnia-cert.meta";

/// Reuse the cert cached in `dir` if it is still comfortably valid and was
/// minted for the same SANs; otherwise mint a new one and write it there.
/// Cache I/O failures are logged and fall back to an in-memory cert —
/// a read-only or missing cache dir must never stop the server starting.
pub fn load_or_mint(dir: &Path, subject_alt_names: Vec<String>) -> Result<GeneratedCert> {
    let sans = default_sans(subject_alt_names);
    match load_cached(dir) {
        Ok(Some(cached)) if cached.subject_alt_names == sans && !cached.needs_remint() => {
            tracing::info!(
                dir = %dir.display(),
                remaining_h = cached.remaining().as_secs() / 3600,
                "reusing cached server cert"
            );
            return Ok(cached);
        }
        Ok(Some(_)) => tracing::info!("cached cert is stale or for other SANs; minting new one"),
        Ok(None) => {}
        Err(e) => tracing::warn!(error = ?e, "could not read cert cache; minting new one"),
    }
    let fresh = self_signed(sans)?;
    if let Err(e) = store_cached(dir, &fresh) {
        tracing::warn!(error = ?e, dir = %dir.display(), "could not write cert cache");
    }
    Ok(fresh)
}

fn load_cached(dir: &Path) -> Result<Option<GeneratedCert>> {
    let meta_path = dir.join(META_FILE);
    if !meta_path.exists() {
        return Ok(None);
    }
    let meta = std::fs::read_to_string(&meta_path).context("reading cert meta")?;
    let mut not_after_unix = None;
    let mut sans = Vec::new();
    for line in meta.lines() {
        if let Some(v) = line.strip_prefix("not_after=") {
            not_after_unix = v.trim().parse().ok();
        } else if let Some(v) = line.strip_prefix("san=") {
            sans.push(v.trim().to_string());
        }
    }
    let Some(not_after_unix) = not_after_unix else {
        return Ok(None);
    };
    Ok(Some(GeneratedCert {
        cert_der: std::fs::read(dir.join(CERT_FILE)).context("reading cached cert")?,
        key_der: std::fs::read(dir.join(KEY_FILE)).context("reading cached key")?,
        subject_alt_names: sans,
        not_after_unix,
    }))
}

fn store_cached(dir: &Path, cert: &GeneratedCert) -> Result<()> {
    std::fs::create_dir_all(dir).context("creating cert cache dir")?;
    let mut meta = format!("not_after={}\n", cert.not_after_unix);
    for san in &cert.subject_alt_names {
        meta.push_str(&format!("san={san}\n"));
    }
    std::fs::write(dir.join(CERT_FILE), &cert.cert_der).context("writing cert")?;
    write_private(&dir.join(KEY_FILE), &cert.key_der).context("writing key")?;
    // Meta last: a crash mid-write leaves no meta, so the next start
    // mints fresh instead of pairing a new cert with an old key.
    std::fs::write(dir.join(META_FILE), meta).context("writing cert meta")?;
    Ok(())
}

#[cfg(unix)]
fn write_private(path: &Path, bytes: &[u8]) -> std::io::Result<()> {
    use std::io::Write;
    use std::os::unix::fs::OpenOptionsExt;
    let mut f = std::fs::OpenOptions::new()
        .write(true)
        .create(true)
        .truncate(true)
        .mode(0o600)
        .open(path)?;
    f.write_all(bytes)
}

#[cfg(not(unix))]
fn write_private(path: &Path, bytes: &[u8]) -> std::io::Result<()> {
    std::fs::write(path, bytes)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn validity_fits_browser_hash_pinning_limit() {
        let cert = self_signed(vec![]).unwrap();
        // ≤ 14 days total, as Chrome requires for serverCertificateHashes.
        let remaining = cert.remaining();
        assert!(remaining <= Duration::from_secs(14 * 24 * 3600));
        assert!(remaining > Duration::from_secs(12 * 24 * 3600));
        assert!(!cert.needs_remint());
    }

    #[test]
    fn hash_is_stable_hex() {
        let cert = self_signed(vec!["narnia".into()]).unwrap();
        let hex = cert.sha256_hex();
        assert_eq!(hex.len(), 64);
        assert_eq!(hex, cert.sha256_hex());
        assert_eq!(cert.subject_alt_names, vec!["narnia".to_string()]);
    }

    #[test]
    fn cache_roundtrip_reuses_same_cert() {
        let dir = std::env::temp_dir().join(format!("narnia-cert-test-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        let a = load_or_mint(&dir, vec!["host-a".into()]).unwrap();
        let b = load_or_mint(&dir, vec!["host-a".into()]).unwrap();
        assert_eq!(
            a.sha256(),
            b.sha256(),
            "second start should reuse the cache"
        );
        // Different SANs → fresh cert.
        let c = load_or_mint(&dir, vec!["host-b".into()]).unwrap();
        assert_ne!(a.sha256(), c.sha256());
        let _ = std::fs::remove_dir_all(&dir);
    }
}

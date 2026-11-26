#[derive(Debug, thiserror::Error, PartialEq, Eq)]
pub enum CertHashError {
    #[error("certificate hash must be 64 hex characters, got {0}")]
    WrongLength(usize),
    #[error("certificate hash contains a character that is not hex")]
    NotHex,
}

pub fn parse_cert_hash(hex: &str) -> Result<[u8; 32], CertHashError> {
    let digits = hex.as_bytes();
    if digits.len() != 64 {
        return Err(CertHashError::WrongLength(digits.len()));
    }
    let mut hash = [0u8; 32];
    for (byte, pair) in hash.iter_mut().zip(digits.chunks_exact(2)) {
        *byte = (hex_value(pair[0])? << 4) | hex_value(pair[1])?;
    }
    Ok(hash)
}

fn hex_value(digit: u8) -> Result<u8, CertHashError> {
    char::from(digit)
        .to_digit(16)
        .and_then(|value| u8::try_from(value).ok())
        .ok_or(CertHashError::NotHex)
}

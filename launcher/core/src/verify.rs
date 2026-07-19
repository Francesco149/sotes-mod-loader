//! Integrity: every file the launcher downloads is checked against the `sha256` pinned in the
//! (trusted) registry, so the file host — a GitHub release, any CDN — needn't be trusted. A
//! mismatch MUST abort the install (see `docs/REGISTRY.md` "Trust model"). This isn't a secret
//! comparison, so plain (case-insensitive) hex equality is fine.

use anyhow::{bail, Result};
use sha2::{Digest, Sha256};

/// Lowercase-hex sha256 of `bytes`.
pub fn sha256_hex(bytes: &[u8]) -> String {
    let mut h = Sha256::new();
    h.update(bytes);
    hex::encode(h.finalize())
}

/// Lowercase-hex sha256 of a reader, streamed (for large files — no full buffering).
pub fn sha256_hex_reader<R: std::io::Read>(mut r: R) -> std::io::Result<String> {
    let mut h = Sha256::new();
    let mut buf = [0u8; 64 * 1024];
    loop {
        let n = r.read(&mut buf)?;
        if n == 0 {
            break;
        }
        h.update(&buf[..n]);
    }
    Ok(hex::encode(h.finalize()))
}

/// sha256 of a file on disk, streamed.
pub fn sha256_hex_file(path: impl AsRef<std::path::Path>) -> std::io::Result<String> {
    sha256_hex_reader(std::fs::File::open(path)?)
}

/// True iff `bytes` hashes to `expected` (hex; case + surrounding whitespace insensitive).
pub fn matches(bytes: &[u8], expected_hex: &str) -> bool {
    sha256_hex(bytes).eq_ignore_ascii_case(expected_hex.trim())
}

/// Assert `bytes` matches `expected_hex`, or fail with the mismatch (the install-abort path).
pub fn ensure(bytes: &[u8], expected_hex: &str) -> Result<()> {
    let actual = sha256_hex(bytes);
    if actual.eq_ignore_ascii_case(expected_hex.trim()) {
        Ok(())
    } else {
        bail!(
            "sha256 mismatch: expected {}, got {}",
            expected_hex.trim().to_ascii_lowercase(),
            actual
        );
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // NIST-known vectors.
    const EMPTY: &str = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    const ABC: &str = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

    #[test]
    fn known_vectors() {
        assert_eq!(sha256_hex(b""), EMPTY);
        assert_eq!(sha256_hex(b"abc"), ABC);
    }

    #[test]
    fn matches_is_case_and_whitespace_insensitive() {
        assert!(matches(b"abc", ABC));
        assert!(matches(b"abc", &ABC.to_ascii_uppercase()));
        assert!(matches(b"abc", &format!("  {ABC}  ")));
        assert!(!matches(b"abd", ABC));
    }

    #[test]
    fn ensure_reports_mismatch() {
        assert!(ensure(b"abc", ABC).is_ok());
        let err = ensure(b"abc", EMPTY).unwrap_err().to_string();
        assert!(err.contains("sha256 mismatch"), "{err}");
        assert!(err.contains(ABC), "error should show the actual hash: {err}");
    }

    #[test]
    fn reader_matches_oneshot() {
        let data = b"the quick brown fox";
        assert_eq!(sha256_hex_reader(&data[..]).unwrap(), sha256_hex(data));
    }
}

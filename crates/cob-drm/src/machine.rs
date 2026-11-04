//! The machine identity a license can be bound to.
//!
//! We read the operating system's own machine UUID and never store or print it
//! raw: what goes into a license file is `sha256("cob-machine-id-v1\0" + uuid)`,
//! so a license that is passed around doesn't leak the hardware identifier.
//!
//! Sources (the first that yields a value):
//!   Linux    /etc/machine-id, /var/lib/dbus/machine-id, then
//!            /sys/class/dmi/id/product_uuid (root-only on modern kernels, so it
//!            comes last -- the id must not change with the user running Cob)
//!   macOS    IOPlatformUUID from `ioreg`
//!   Windows  HKLM\SOFTWARE\Microsoft\Cryptography\MachineGuid

use sha2::{Digest, Sha256};
#[cfg(any(target_os = "macos", windows))]
use std::process::Command;

const SALT: &[u8] = b"cob-machine-id-v1\0";

fn clean(s: &str) -> Option<String> {
    let t = s.trim().trim_matches('"').trim().to_ascii_lowercase();
    // refuse obviously empty / placeholder identifiers
    if t.len() < 8 || t.chars().all(|c| c == '0' || c == '-' || c == 'f') {
        None
    } else {
        Some(t)
    }
}

#[cfg(target_os = "linux")]
fn raw_uuid() -> Option<String> {
    for p in ["/etc/machine-id", "/var/lib/dbus/machine-id", "/sys/class/dmi/id/product_uuid"] {
        if let Ok(s) = std::fs::read_to_string(p) {
            if let Some(c) = clean(&s) {
                return Some(c);
            }
        }
    }
    None
}

#[cfg(target_os = "macos")]
fn raw_uuid() -> Option<String> {
    let out = Command::new("ioreg").args(["-rd1", "-c", "IOPlatformExpertDevice"]).output().ok()?;
    let text = String::from_utf8_lossy(&out.stdout).into_owned();
    let line = text.lines().find(|l| l.contains("IOPlatformUUID"))?;
    // ..."IOPlatformUUID" = "XXXXXXXX-XXXX-..."
    clean(line.rsplit('=').next()?)
}

#[cfg(windows)]
fn raw_uuid() -> Option<String> {
    let out = Command::new("reg")
        .args(["query", r"HKLM\SOFTWARE\Microsoft\Cryptography", "/v", "MachineGuid"])
        .output()
        .ok()?;
    let text = String::from_utf8_lossy(&out.stdout).into_owned();
    let line = text.lines().find(|l| l.contains("MachineGuid"))?;
    clean(line.split_whitespace().last()?)
}

#[cfg(not(any(target_os = "linux", target_os = "macos", windows)))]
fn raw_uuid() -> Option<String> {
    None
}

/// Hex digest identifying this machine, or `None` if the OS offers no id.
pub fn id_hash() -> Option<String> {
    Some(hash_of(&raw_uuid()?))
}

/// The salted digest of a given raw UUID (exposed so it can be tested).
pub fn hash_of(raw_uuid: &str) -> String {
    let mut h = Sha256::new();
    h.update(SALT);
    h.update(raw_uuid.trim().to_ascii_lowercase().as_bytes());
    h.finalize().iter().map(|b| format!("{:02x}", b)).collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn hash_is_stable_and_salted() {
        let a = hash_of("ABCDEF12-3456-7890-ABCD-EF1234567890");
        assert_eq!(a, hash_of("abcdef12-3456-7890-abcd-ef1234567890 \n"));
        assert_eq!(a.len(), 64);
        assert_ne!(a, hash_of("something-else-entirely"));
    }

    #[test]
    fn placeholders_rejected() {
        assert!(clean("00000000-0000-0000-0000-000000000000").is_none());
        assert!(clean("ffffffff-ffff-ffff-ffff-ffffffffffff").is_none());
        assert!(clean("short").is_none());
        assert!(clean("4c4c4544-0042-3510-8052-b4c04f4d3032").is_some());
    }
}

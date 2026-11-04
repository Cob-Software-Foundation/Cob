//! Cob Enterprise license files.
//!
//! A license is a small text file (`cob.lic`) signed with an EdDSA private key
//! that lives only on the maintainer's machine; this code holds the PUBLIC
//! key(s) and can verify licenses but never create them (that is `cob_sign`).
//!
//! ```text
//! COBLIC1
//! customer=Acme Corp
//! issued=2026-10-22
//! expires=2027-10-22          (or: never)
//! features=audit_log,sso      (or: *)
//! seats=50
//! key_id=1
//! bind=machine                (optional -- see below)
//! machine=<64 hex>            (optional -- see below)
//! ---
//! <base64 signature>
//! bound_machine=<64 hex>      (unsigned; written by the interpreter)
//! bound_on=2026-10-30
//! ```
//!
//! The signature covers every byte up to and including the newline before the
//! `---` line. Everything after the signature line is NOT signed.
//!
//! MACHINE BINDING
//!   * `machine=<hash>` in the signed part: the vendor pre-bound the license to
//!     one machine (get the hash from `cob_sign machine-id`). It simply must
//!     match; this cannot be edited without breaking the signature.
//!   * `bind=machine` in the signed part: bind on first run. The first time a
//!     Cob Enterprise tool starts with this license it reads the machine's UUID
//!     and writes `bound_machine=` into the license file; every later run must
//!     match it. The write goes below the signature precisely because the
//!     signature cannot be redone without the private key.
//!
//! HONEST LIMIT: that written line is unsigned, so someone who edits or deletes
//! it can re-bind a license to another machine. First-run binding stops casual
//! copying, not a determined user. Only `machine=` (pre-bound, signed) or an
//! online activation service can enforce it.

use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::sync::Mutex;

use crate::eddsa;
use crate::machine;

include!(concat!(env!("OUT_DIR"), "/keys_gen.rs"));

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum LicError {
    NotFound,
    Malformed,
    UnknownKey,
    BadSignature,
    Expired,
    /// Bound (by `machine=` or a prior first run) to a different machine.
    WrongMachine,
    /// First-run binding could not be written back into the file.
    BindFailed(String),
    /// The license is machine-bound but this system exposes no machine id.
    NoMachineId,
}

impl LicError {
    pub fn text(&self) -> String {
        match self {
            LicError::NotFound => "no license file found".into(),
            LicError::Malformed => "license file is malformed".into(),
            LicError::UnknownKey => "license was signed with a key this build does not know".into(),
            LicError::BadSignature => "license signature is invalid (file edited or not issued by Pixel Pulse Labs)".into(),
            LicError::Expired => "license has expired".into(),
            LicError::WrongMachine => "this license is bound to a different machine".into(),
            LicError::BindFailed(why) => format!("could not record this machine in the license file ({})", why),
            LicError::NoMachineId => "this license is bound to a machine, but this system exposes no machine id".into(),
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum Bind {
    #[default]
    None,
    /// `bind=machine`: bind to the first machine that runs it.
    FirstRun,
}

#[derive(Debug, Clone, Default)]
pub struct License {
    pub customer: String,
    pub issued: String,
    /// "YYYY-MM-DD" or "never"
    pub expires: String,
    /// comma list, or "*"
    pub features: String,
    pub seats: i32,
    pub key_id: i32,
    /// 0 = never
    pub expires_at: i64,
    pub bind: Bind,
    /// Vendor pre-bound machine hash (signed).
    pub machine: Option<String>,
}

/// A signature-checked license plus the unsigned trailer.
#[derive(Debug, Clone)]
pub struct Verified {
    pub lic: License,
    pub expired: bool,
    /// `bound_machine=` from the unsigned trailer, if present.
    pub bound_machine: Option<String>,
    pub bound_on: Option<String>,
}

// ---- text handling ------------------------------------------------------------

/// Strip every '\r' so a license mangled by CRLF conversion (email, Windows
/// editors) still verifies: the signer only ever writes LF.
fn normalize_lf(t: &[u8]) -> Vec<u8> {
    t.iter().copied().filter(|&b| b != b'\r').collect()
}

/// (payload length incl. its trailing '\n', offset where the part after `---` starts)
fn split_license(t: &[u8]) -> Option<(usize, usize)> {
    let n = t.len();
    let mut i = 0;
    while i + 4 <= n {
        if (i == 0 || t[i - 1] == b'\n') && &t[i..i + 3] == b"---" && (t[i + 3] == b'\n' || i + 3 == n) {
            if i == 0 {
                return None;
            }
            return Some((i, (i + 4).min(n)));
        }
        i += 1;
    }
    None
}

/// A trailer line is `lowercase_key=` followed by a non-'=' character. Base64
/// only ever has '=' as trailing padding, so it can never look like this.
fn is_trailer_line(line: &str) -> bool {
    let Some((k, v)) = line.split_once('=') else { return false };
    !k.is_empty() && k.bytes().all(|b| b.is_ascii_lowercase() || b == b'_') && v.bytes().next().map_or(false, |c| c != b'=')
}

/// Splits the part after `---` into (base64 signature, trailer key/values).
fn split_sig_and_trailer(rest: &[u8]) -> (String, Vec<(String, String)>) {
    let text = String::from_utf8_lossy(rest);
    let mut sig = String::new();
    let mut trailer = Vec::new();
    for line in text.lines() {
        let line = line.trim();
        if line.is_empty() {
            continue;
        }
        if is_trailer_line(line) {
            if let Some((k, v)) = line.split_once('=') {
                trailer.push((k.to_string(), v.trim().to_string()));
            }
        } else if trailer.is_empty() {
            sig.push_str(line);
        }
    }
    (sig, trailer)
}

fn b64val(c: u8) -> Option<u32> {
    Some(match c {
        b'A'..=b'Z' => (c - b'A') as u32,
        b'a'..=b'z' => (c - b'a') as u32 + 26,
        b'0'..=b'9' => (c - b'0') as u32 + 52,
        b'+' => 62,
        b'/' => 63,
        _ => return None,
    })
}

/// Standard base64, whitespace and '=' ignored.
pub fn b64decode(s: &str) -> Option<Vec<u8>> {
    let (mut acc, mut bits) = (0u32, 0);
    let mut out = Vec::new();
    for c in s.bytes() {
        if matches!(c, b'=' | b'\n' | b'\r' | b' ' | b'\t') {
            continue;
        }
        acc = (acc << 6) | b64val(c)?;
        bits += 6;
        if bits >= 8 {
            bits -= 8;
            out.push(((acc >> bits) & 0xff) as u8);
        }
    }
    Some(out)
}

pub fn b64encode(data: &[u8]) -> String {
    const T: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut out = String::new();
    for chunk in data.chunks(3) {
        let v = (chunk[0] as u32) << 16 | (*chunk.get(1).unwrap_or(&0) as u32) << 8 | *chunk.get(2).unwrap_or(&0) as u32;
        out.push(T[(v >> 18) as usize & 63] as char);
        out.push(T[(v >> 12) as usize & 63] as char);
        out.push(if chunk.len() > 1 { T[(v >> 6) as usize & 63] as char } else { '=' });
        out.push(if chunk.len() > 2 { T[v as usize & 63] as char } else { '=' });
    }
    out
}

// ---- dates ----------------------------------------------------------------------

/// Days since 1970-01-01 (proleptic Gregorian).
fn days_from_civil(y: i64, m: i64, d: i64) -> i64 {
    let y = if m <= 2 { y - 1 } else { y };
    let era = if y >= 0 { y } else { y - 399 } / 400;
    let yoe = y - era * 400;
    let doy = (153 * (m + if m > 2 { -3 } else { 9 }) + 2) / 5 + d - 1;
    let doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    era * 146097 + doe - 719468
}

fn civil_from_days(z: i64) -> (i64, i64, i64) {
    let z = z + 719468;
    let era = if z >= 0 { z } else { z - 146096 } / 146097;
    let doe = z - era * 146097;
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    let y = yoe + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    (if m <= 2 { y + 1 } else { y }, m, d)
}

/// "YYYY-MM-DD" -> the last second of that day (UTC); `None` if malformed.
fn parse_date_end_of_day(s: &str) -> Option<i64> {
    let mut it = s.split('-');
    let (y, m, d) = (it.next()?, it.next()?, it.next()?);
    if it.next().is_some() {
        return None;
    }
    let num = |t: &str, max_len: usize| -> Option<i64> {
        if t.is_empty() || t.len() > max_len || !t.bytes().all(|b| b.is_ascii_digit()) {
            None
        } else {
            t.parse().ok()
        }
    };
    let (y, m, d) = (num(y, 4)?, num(m, 2)?, num(d, 2)?);
    if !(1970..=9999).contains(&y) || !(1..=12).contains(&m) || !(1..=31).contains(&d) {
        return None;
    }
    Some(days_from_civil(y, m, d) * 86400 + 86399)
}

fn today_utc() -> String {
    let secs = now();
    let (y, m, d) = civil_from_days(secs.div_euclid(86400));
    format!("{:04}-{:02}-{:02}", y, m, d)
}

fn now() -> i64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs() as i64)
        .unwrap_or(0)
}

// ---- parsing & verification ---------------------------------------------------------

/// C `atoi`: leading integer, junk ignored.
fn atoi(s: &str) -> i32 {
    cob_atoi(s)
}
fn cob_atoi(s: &str) -> i32 {
    let t = s.trim_start();
    let (neg, t) = match t.strip_prefix('-') {
        Some(r) => (true, r),
        None => (false, t.strip_prefix('+').unwrap_or(t)),
    };
    let digits: String = t.chars().take_while(|c| c.is_ascii_digit()).collect();
    let v: i64 = digits.parse().unwrap_or(0);
    (if neg { -v } else { v }).clamp(i32::MIN as i64, i32::MAX as i64) as i32
}

fn is_hash(s: &str) -> bool {
    s.len() == 64 && s.bytes().all(|b| b.is_ascii_hexdigit())
}

/// Parses the key=value lines of a payload (signed, or key_id-only probing).
fn parse_payload(p: &[u8]) -> Result<License, LicError> {
    let text = String::from_utf8_lossy(p);
    let mut lic = License { key_id: -1, ..License::default() };
    let mut first = true;
    for line in text.split('\n') {
        if first {
            first = false;
            if line != "COBLIC1" {
                return Err(LicError::Malformed);
            }
            continue;
        }
        if line.is_empty() {
            continue;
        }
        let Some((k, v)) = line.split_once('=') else { return Err(LicError::Malformed) };
        match k {
            "customer" => lic.customer = v.chars().take(127).collect(),
            "issued" => lic.issued = v.chars().take(15).collect(),
            "expires" => lic.expires = v.chars().take(15).collect(),
            "features" => lic.features = v.chars().take(255).collect(),
            "seats" => lic.seats = atoi(v),
            "key_id" => lic.key_id = atoi(v),
            "bind" => {
                lic.bind = match v {
                    "machine" => Bind::FirstRun,
                    "none" => Bind::None,
                    // an unknown mode must not silently turn binding off
                    _ => return Err(LicError::Malformed),
                }
            }
            "machine" => {
                if !is_hash(v) {
                    return Err(LicError::Malformed);
                }
                lic.machine = Some(v.to_ascii_lowercase());
            }
            _ => {} // unknown keys are ignored (they are covered by the signature anyway)
        }
    }
    Ok(lic)
}

/// Checks the signature with an explicit public key and parses the license.
/// Expiry is reported in `Verified::expired`, not as an error, so tools can
/// still show an expired license.
pub fn verify_text(text: &[u8], pubkey: &[u8; 32]) -> Result<Verified, LicError> {
    let t = normalize_lf(text);
    verify_normalized(&t, pubkey)
}

fn verify_normalized(t: &[u8], pubkey: &[u8; 32]) -> Result<Verified, LicError> {
    let (payload_len, rest_at) = split_license(t).ok_or(LicError::Malformed)?;
    let (sig_b64, trailer) = split_sig_and_trailer(&t[rest_at..]);
    let sig = b64decode(&sig_b64).ok_or(LicError::Malformed)?;
    let sig: [u8; 64] = sig.try_into().map_err(|_| LicError::Malformed)?;
    if !eddsa::verify(&sig, pubkey, &t[..payload_len]) {
        return Err(LicError::BadSignature);
    }
    let mut lic = parse_payload(&t[..payload_len])?;
    if lic.expires == "never" || lic.expires.is_empty() {
        lic.expires_at = 0;
    } else {
        lic.expires_at = parse_date_end_of_day(&lic.expires).ok_or(LicError::Malformed)?;
    }
    let expired = lic.expires_at != 0 && now() > lic.expires_at;
    let get = |k: &str| trailer.iter().find(|(key, _)| key == k).map(|(_, v)| v.to_ascii_lowercase());
    Ok(Verified {
        lic,
        expired,
        bound_machine: get("bound_machine").filter(|v| is_hash(v)),
        bound_on: get("bound_on"),
    })
}

// ---- machine binding ------------------------------------------------------------------

#[derive(Debug, PartialEq, Eq)]
pub enum BindDecision {
    /// Nothing to do (not machine-bound, or already bound to this machine).
    Allowed,
    /// First run of a `bind=machine` license: record this machine.
    BindNow,
    Refused(LicError),
}

/// The binding policy, as a pure function of the verified license and this
/// machine's id hash (`None` when the OS exposes no id).
pub fn decide_binding(v: &Verified, this_machine: Option<&str>) -> BindDecision {
    // 1. vendor pre-bound (signed): must match
    if let Some(m) = &v.lic.machine {
        return match this_machine {
            None => BindDecision::Refused(LicError::NoMachineId),
            Some(me) if me.eq_ignore_ascii_case(m) => BindDecision::Allowed,
            Some(_) => BindDecision::Refused(LicError::WrongMachine),
        };
    }
    // 2. bind on first run
    if v.lic.bind == Bind::FirstRun {
        let Some(me) = this_machine else { return BindDecision::Refused(LicError::NoMachineId) };
        return match &v.bound_machine {
            Some(b) if b.eq_ignore_ascii_case(me) => BindDecision::Allowed,
            Some(_) => BindDecision::Refused(LicError::WrongMachine),
            None => BindDecision::BindNow,
        };
    }
    // 3. an ordinary, unbound license
    BindDecision::Allowed
}

/// Appends the (unsigned) binding record below the signature, atomically where
/// possible. The original bytes are left exactly as they were.
pub fn write_binding(path: &Path, raw: &[u8], machine_hash: &str) -> std::io::Result<()> {
    let crlf = raw.windows(2).any(|w| w == b"\r\n");
    let nl = if crlf { "\r\n" } else { "\n" };
    let mut out = raw.to_vec();
    if !out.ends_with(b"\n") {
        out.extend_from_slice(nl.as_bytes());
    }
    out.extend_from_slice(format!("bound_machine={}{nl}bound_on={}{nl}", machine_hash, today_utc()).as_bytes());

    // temp file + rename, so a crash never leaves a half-written license
    let tmp = path.with_extension(format!("lic.tmp{}", std::process::id()));
    let atomic = (|| -> std::io::Result<()> {
        let mut f = fs::File::create(&tmp)?;
        f.write_all(&out)?;
        f.sync_all()?;
        if let Ok(meta) = fs::metadata(path) {
            let _ = fs::set_permissions(&tmp, meta.permissions());
        }
        fs::rename(&tmp, path)
    })();
    if atomic.is_ok() {
        return Ok(());
    }
    let _ = fs::remove_file(&tmp);
    // the directory may be read-only while the file itself is writable
    let mut f = fs::OpenOptions::new().append(true).open(path)?;
    f.write_all(&out[raw.len()..])
}

// ---- finding and loading the license ------------------------------------------------------

const MAX_LICENSE_BYTES: u64 = 65536;

/// A successfully loaded license: what it says, and where it lives.
#[derive(Debug, Clone)]
pub struct Loaded {
    pub lic: License,
    pub path: PathBuf,
    /// Set when this very load bound the license to this machine.
    pub newly_bound: bool,
}

fn read_license_file(path: &Path) -> Option<Vec<u8>> {
    let meta = fs::metadata(path).ok()?;
    if !meta.is_file() || meta.len() > MAX_LICENSE_BYTES {
        return None;
    }
    fs::read(path).ok()
}

/// Verifies `raw` against the embedded key whose id the (still untrusted)
/// payload names.
fn verify_embedded(raw: &[u8]) -> Result<Verified, LicError> {
    let t = normalize_lf(raw);
    // pick the key by key_id (untrusted until the signature checks out)
    let (payload_len, _) = split_license(&t).ok_or(LicError::Malformed)?;
    let probe = parse_payload(&t[..payload_len])?;
    for (id, key) in EMBEDDED_KEYS {
        if *id == probe.key_id {
            return verify_normalized(&t, key);
        }
    }
    Err(LicError::UnknownKey)
}

/// Full check of one license file, including binding (and the first-run write).
/// `Err((error, license-if-known))`.
fn check_file(path: &Path, raw: &[u8], machine_id: Option<&str>) -> Result<Loaded, (LicError, Option<License>)> {
    let v = verify_embedded(raw).map_err(|e| (e, None))?;
    if v.expired {
        return Err((LicError::Expired, Some(v.lic)));
    }
    match decide_binding(&v, machine_id) {
        BindDecision::Allowed => Ok(Loaded { lic: v.lic, path: path.to_path_buf(), newly_bound: false }),
        BindDecision::Refused(e) => Err((e, Some(v.lic))),
        BindDecision::BindNow => {
            let me = machine_id.expect("BindNow implies a machine id");
            write_binding(path, raw, me).map_err(|e| (LicError::BindFailed(e.to_string()), Some(v.lic.clone())))?;
            // prove it stuck: re-read and confirm the record is really in the file
            let again = read_license_file(path).and_then(|b| verify_embedded(&b).ok());
            match again {
                Some(v2) if v2.bound_machine.as_deref() == Some(me) => {
                    Ok(Loaded { lic: v.lic, path: path.to_path_buf(), newly_bound: true })
                }
                _ => Err((LicError::BindFailed("the file does not keep the change".into()), Some(v.lic))),
            }
        }
    }
}

/// Candidate paths, in order: explicit, $COB_LICENSE, ./cob.lic, the per-user
/// config dir, /etc/cob/cob.lic.
pub fn search_paths(explicit: Option<&Path>) -> Vec<PathBuf> {
    if let Some(p) = explicit {
        return vec![p.to_path_buf()];
    }
    let mut v = Vec::new();
    if let Some(e) = std::env::var_os("COB_LICENSE") {
        if !e.is_empty() {
            v.push(PathBuf::from(e));
        }
    }
    v.push(PathBuf::from("cob.lic"));
    if cfg!(windows) {
        if let Some(a) = std::env::var_os("APPDATA") {
            if !a.is_empty() {
                v.push(PathBuf::from(a).join("cob").join("cob.lic"));
            }
        }
    } else {
        if let Some(h) = std::env::var_os("HOME") {
            if !h.is_empty() {
                v.push(PathBuf::from(h).join(".config/cob/cob.lic"));
            }
        }
        v.push(PathBuf::from("/etc/cob/cob.lic"));
    }
    v
}

/// Finds, verifies and (on first run of a `bind=machine` license) binds a license.
pub fn load(explicit: Option<&Path>) -> Result<Loaded, (LicError, Option<License>)> {
    let machine = machine::id_hash();
    for p in search_paths(explicit) {
        // $COB_LICENSE / explicit paths that exist but are bad are errors, not "keep looking"
        if let Some(raw) = read_license_file(&p) {
            return check_file(&p, &raw, machine.as_deref());
        }
    }
    Err((LicError::NotFound, None))
}

// ---- the startup gate ------------------------------------------------------------------------

static CURRENT: Mutex<Option<License>> = Mutex::new(None);

/// Startup gate for enterprise tools: loads + verifies the license. Prints a
/// clear error and returns nonzero on failure; silent on success (no banners,
/// no nagging) -- except for the one-line note the first time a license binds
/// itself to this machine, because that edits a file the user owns.
pub fn require(tool: &str) -> i32 {
    match load(None) {
        Ok(loaded) => {
            if loaded.newly_bound {
                eprintln!(
                    "[{}] note: this license is now bound to this machine (recorded in {})",
                    tool,
                    loaded.path.display()
                );
            }
            *CURRENT.lock().unwrap_or_else(|e| e.into_inner()) = Some(loaded.lic);
            0
        }
        Err((e, lic)) => {
            eprintln!(
                "[{}] error: this is a Cob Enterprise build and it needs a valid license.\n  reason: {}",
                tool,
                e.text()
            );
            match (&e, &lic) {
                (LicError::Expired, Some(l)) => eprintln!("  license for \"{}\" expired on {}", l.customer, l.expires),
                (LicError::BindFailed(_), _) => eprintln!(
                    "  A license that binds on first run must be writable the first time it is used:\n  make the file writable, or point $COB_LICENSE at a writable copy, then run again."
                ),
                (LicError::WrongMachine, _) => eprintln!(
                    "  It was first used on another computer. Ask for a new license, or for one issued for this\n  machine (run `cob_sign machine-id` here and send the result)."
                ),
                (LicError::NoMachineId, _) => eprintln!(
                    "  Cob reads /etc/machine-id (Linux), IOPlatformUUID (macOS) or MachineGuid (Windows).\n  In a container, mount the host's /etc/machine-id read-only."
                ),
                _ => {}
            }
            eprintln!(
                "  Looked for: $COB_LICENSE, ./cob.lic, per-user config dir, /etc/cob/cob.lic\n  The free build needs no license: https://cob.pixel-pulse.dev/download.html"
            );
            1
        }
    }
}

pub fn current() -> Option<License> {
    CURRENT.lock().unwrap_or_else(|e| e.into_inner()).clone()
}

/// After a successful `require()`: is this feature unlocked?
pub fn feature_enabled(feature: &str) -> bool {
    let Some(l) = current() else { return false };
    if feature.is_empty() {
        return false;
    }
    l.features == "*" || l.features.split(',').any(|f| f == feature)
}

// ---- tests ------------------------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;

    const SEED: [u8; 32] = [42u8; 32];

    /// Builds a signed license text the way `cob_sign sign` does.
    fn make(extra: &str, expires: &str) -> Vec<u8> {
        let payload = format!(
            "COBLIC1\ncustomer=Acme\nissued=2026-01-01\nexpires={}\nfeatures=*\nseats=5\nkey_id=1\n{}",
            expires, extra
        );
        let sig = eddsa::sign(&SEED, payload.as_bytes());
        format!("{}---\n{}\n", payload, b64encode(&sig)).into_bytes()
    }
    fn pk() -> [u8; 32] {
        eddsa::public_key(&SEED)
    }
    const ME: &str = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    const OTHER: &str = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

    #[test]
    fn valid_and_tampered() {
        let lic = make("", "never");
        let v = verify_text(&lic, &pk()).unwrap();
        assert_eq!(v.lic.customer, "Acme");
        assert_eq!(v.lic.seats, 5);
        assert!(!v.expired);
        let tampered = String::from_utf8(lic).unwrap().replace("seats=5", "seats=500").into_bytes();
        assert_eq!(verify_text(&tampered, &pk()).unwrap_err(), LicError::BadSignature);
    }

    #[test]
    fn crlf_still_verifies() {
        let lic = String::from_utf8(make("", "never")).unwrap().replace('\n', "\r\n").into_bytes();
        assert!(verify_text(&lic, &pk()).is_ok());
    }

    #[test]
    fn expiry() {
        assert!(verify_text(&make("", "2020-01-01"), &pk()).unwrap().expired);
        assert!(!verify_text(&make("", "9999-12-31"), &pk()).unwrap().expired);
        assert_eq!(verify_text(&make("", "soon"), &pk()).unwrap_err(), LicError::Malformed);
    }

    #[test]
    fn trailer_is_unsigned_and_harmless() {
        let mut lic = make("bind=machine\n", "never");
        lic.extend_from_slice(format!("bound_machine={}\nbound_on=2026-10-30\n", ME).as_bytes());
        let v = verify_text(&lic, &pk()).unwrap();
        assert_eq!(v.bound_machine.as_deref(), Some(ME));
        assert_eq!(v.lic.bind, Bind::FirstRun);
    }

    #[test]
    fn unsigned_machine_cannot_be_smuggled_into_the_payload() {
        // adding `machine=` to the signed part breaks the signature
        let lic = String::from_utf8(make("", "never")).unwrap().replace("key_id=1\n", &format!("key_id=1\nmachine={}\n", OTHER));
        assert_eq!(verify_text(lic.as_bytes(), &pk()).unwrap_err(), LicError::BadSignature);
    }

    #[test]
    fn binding_decisions() {
        let unbound = verify_text(&make("", "never"), &pk()).unwrap();
        assert_eq!(decide_binding(&unbound, Some(ME)), BindDecision::Allowed);
        assert_eq!(decide_binding(&unbound, None), BindDecision::Allowed);

        let first = verify_text(&make("bind=machine\n", "never"), &pk()).unwrap();
        assert_eq!(decide_binding(&first, Some(ME)), BindDecision::BindNow);
        assert_eq!(decide_binding(&first, None), BindDecision::Refused(LicError::NoMachineId));

        let mut bound = first.clone();
        bound.bound_machine = Some(ME.into());
        assert_eq!(decide_binding(&bound, Some(ME)), BindDecision::Allowed);
        assert_eq!(decide_binding(&bound, Some(OTHER)), BindDecision::Refused(LicError::WrongMachine));

        let pre = verify_text(&make(&format!("machine={}\n", ME), "never"), &pk()).unwrap();
        assert_eq!(decide_binding(&pre, Some(ME)), BindDecision::Allowed);
        assert_eq!(decide_binding(&pre, Some(OTHER)), BindDecision::Refused(LicError::WrongMachine));
        // a signed pre-binding can't be undone by writing a trailer
        let mut pre2 = pre.clone();
        pre2.bound_machine = Some(OTHER.into());
        assert_eq!(decide_binding(&pre2, Some(OTHER)), BindDecision::Refused(LicError::WrongMachine));
    }

    #[test]
    fn unknown_bind_mode_fails_closed() {
        assert_eq!(verify_text(&make("bind=whenever\n", "never"), &pk()).unwrap_err(), LicError::Malformed);
    }

    #[test]
    fn write_binding_round_trip() {
        let dir = std::env::temp_dir().join(format!("cob-drm-test-{}", std::process::id()));
        fs::create_dir_all(&dir).unwrap();
        let path = dir.join("cob.lic");
        let raw = make("bind=machine\n", "never");
        fs::write(&path, &raw).unwrap();

        write_binding(&path, &raw, ME).unwrap();
        let after = fs::read(&path).unwrap();
        assert!(after.starts_with(&raw), "original bytes must be preserved");
        let v = verify_text(&after, &pk()).unwrap();
        assert_eq!(v.bound_machine.as_deref(), Some(ME));
        assert_eq!(decide_binding(&v, Some(ME)), BindDecision::Allowed);
        assert_eq!(decide_binding(&v, Some(OTHER)), BindDecision::Refused(LicError::WrongMachine));
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn dates() {
        assert_eq!(parse_date_end_of_day("1970-01-01"), Some(86399));
        assert_eq!(parse_date_end_of_day("2027-13-01"), None);
        assert_eq!(parse_date_end_of_day("2027-10-22x"), None);
        assert_eq!(civil_from_days(days_from_civil(2026, 10, 30)), (2026, 10, 30));
    }
}

//! farmer -- the Cob package manager: `farmer harvest <package>`
//!
//! PROTOCOL: farmer talks to a completely static JSON API -- plain files hosted
//! on GitHub Pages (or any static host, via $COB_FARMER_BASE_URL). For
//! `farmer harvest <package>`:
//!
//!   1. GET {base}/api/v1/packages/<package>.json
//!        -> { "name": "...", "version": "1.0.0", "zip_url": "https://.../x.zip",
//!             "sha256": "<hex digest, optional>" }
//!      "name" is checked against what was requested, so a misconfigured or
//!      spoofed metadata file is caught before anything is downloaded.
//!   2. GET <zip_url> -> <package>.zip.tmp; if "sha256" was present, the zip is
//!      verified against it before extraction.
//!   3. Unzip into cob_modules/<package>/ -- the layout `shuck <package>` looks
//!      for (cob_modules/<package>/<package>.cob). Every entry's path is checked
//!      against zip-slip traversal before anything is written.
//!
//! The network fetch still goes through curl (Unix) or PowerShell (Windows),
//! keeping the toolchain free of an HTTP/TLS library -- but the tool is spawned
//! with an argument vector, never a shell string, so nothing from argv or from
//! fetched JSON can ever be interpreted by a shell. Checksumming and unzipping
//! happen in-process.

use std::fs;
use std::io;
use std::path::{Path, PathBuf};
use std::process::Command;

use cob_core::consts::*;
use sha2::{Digest, Sha256};

/// Package names become part of URLs and file paths, so they are restricted to
/// letters, digits, '-' and '_' (1..=128 characters).
fn validate_package_name(name: &str) -> bool {
    !name.is_empty() && name.len() <= 128 && name.bytes().all(|c| c.is_ascii_alphanumeric() || c == b'-' || c == b'_')
}

/// Finds `"<field>"  :  "<value>"` anywhere in the text and unescapes
/// `\" \\ \n \t \/` in the value. Metadata files are small flat objects, so a
/// full JSON parser isn't worth vendoring.
fn json_extract_string_field(json: &str, field: &str) -> Option<String> {
    let needle = format!("\"{}\"", field);
    let b = json.as_bytes();
    let mut p = json.find(&needle)? + needle.len();
    let ws = |b: &[u8], mut p: usize| {
        while p < b.len() && matches!(b[p], b' ' | b'\t' | b'\n' | b'\r') {
            p += 1;
        }
        p
    };
    p = ws(b, p);
    if b.get(p) != Some(&b':') {
        return None;
    }
    p = ws(b, p + 1);
    if b.get(p) != Some(&b'"') {
        return None;
    }
    p += 1;
    let mut out: Vec<u8> = Vec::new();
    while p < b.len() && b[p] != b'"' {
        let mut c = b[p];
        if c == b'\\' && p + 1 < b.len() {
            c = match b[p + 1] {
                b'n' => b'\n',
                b't' => b'\t',
                other => other, // covers \" \\ \/ and unknown escapes
            };
            p += 2;
        } else {
            p += 1;
        }
        out.push(c);
    }
    if b.get(p) != Some(&b'"') {
        return None; // unterminated
    }
    String::from_utf8(out).ok()
}

/// Downloads `url` to `out_path` with curl / PowerShell. Success is verified by
/// the tool's exit status AND the output file existing.
fn download(url: &str, out_path: &str) -> bool {
    if !(url.starts_with("https://") || url.starts_with("http://")) {
        return false;
    }
    let status = if cfg!(windows) {
        let q = |s: &str| format!("'{}'", s.replace('\'', "''"));
        Command::new("powershell")
            .args(["-NoProfile", "-NonInteractive", "-Command"])
            .arg(format!(
                "$ProgressPreference='SilentlyContinue'; Invoke-WebRequest -Uri {} -OutFile {} -UseBasicParsing",
                q(url),
                q(out_path)
            ))
            .status()
    } else {
        Command::new("curl").args(["-fsSL", "-o", out_path, "--", url]).status()
    };
    matches!(status, Ok(s) if s.success()) && Path::new(out_path).exists()
}

/// Is this archive entry name (ZIP always stores forward slashes) safe to join
/// onto the extraction directory? Rejects absolute paths, drive letters and any
/// `..` component -- a malicious "../../etc/cron.d/evil" must never escape.
fn is_safe_zip_entry_name(name: &str) -> bool {
    if name.is_empty() || name.starts_with('/') || name.starts_with('\\') || name.contains(':') {
        return false;
    }
    !name.split(['/', '\\']).any(|part| part == "..")
}

fn unzip(zip_path: &str, dest_dir: &Path) -> bool {
    let _ = fs::create_dir(dest_dir);
    let Ok(file) = fs::File::open(zip_path) else {
        eprintln!("farmer: error: '{}' is not a valid zip archive", zip_path);
        return false;
    };
    let Ok(mut archive) = zip::ZipArchive::new(file) else {
        eprintln!("farmer: error: '{}' is not a valid zip archive", zip_path);
        return false;
    };
    for i in 0..archive.len() {
        let mut entry = match archive.by_index(i) {
            Ok(e) => e,
            Err(_) => {
                eprintln!("farmer: error: could not read entry {} of '{}'", i, zip_path);
                return false;
            }
        };
        let name = entry.name().to_string();
        if !is_safe_zip_entry_name(&name) {
            eprintln!("farmer: error: refusing to extract unsafe archive entry '{}'", name);
            return false;
        }
        let full: PathBuf = dest_dir.join(&name);
        if entry.is_dir() {
            let _ = fs::create_dir_all(&full);
            continue;
        }
        // zip files aren't required to list directories before the files in them
        if let Some(parent) = full.parent() {
            let _ = fs::create_dir_all(parent);
        }
        let ok = fs::File::create(&full).and_then(|mut out| io::copy(&mut entry, &mut out)).is_ok();
        if !ok {
            eprintln!("farmer: error: failed to extract '{}' from '{}'", name, zip_path);
            return false;
        }
    }
    true
}

fn verify_sha256(path: &str, expected_hex: &str) -> bool {
    let Ok(mut f) = fs::File::open(path) else { return false };
    let mut h = Sha256::new();
    if io::copy(&mut f, &mut h).is_err() {
        return false;
    }
    let actual: String = h.finalize().iter().map(|b| format!("{:02x}", b)).collect();
    actual.eq_ignore_ascii_case(expected_hex.trim())
}

fn cmd_harvest(package: &str) -> i32 {
    if !validate_package_name(package) {
        eprintln!("farmer: error: '{}' is not a valid package name (letters, digits, '-', '_' only)", package);
        return 1;
    }
    let base_url = match std::env::var(FARMER_BASE_URL_ENV) {
        Ok(v) if !v.is_empty() => v,
        _ => FARMER_DEFAULT_BASE_URL.to_string(),
    };
    let meta_url = format!("{}/api/v1/packages/{}.json", base_url, package);
    let meta_path = format!("{}.meta.json.tmp", package);

    println!("farmer: fetching metadata for '{}'...", package);
    if !download(&meta_url, &meta_path) {
        eprintln!("farmer: error: could not fetch {}", meta_url);
        return 1;
    }
    let meta = fs::read(&meta_path);
    let _ = fs::remove_file(&meta_path);
    let Ok(meta) = meta else {
        eprintln!("farmer: error: could not read downloaded metadata");
        return 1;
    };
    let meta = String::from_utf8_lossy(&meta).into_owned();

    let name = json_extract_string_field(&meta, "name");
    let version = json_extract_string_field(&meta, "version");
    let zip_url = json_extract_string_field(&meta, "zip_url");
    let sha256 = json_extract_string_field(&meta, "sha256"); // optional
    let (Some(name), Some(version), Some(zip_url)) = (name, version, zip_url) else {
        eprintln!("farmer: error: metadata for '{}' is missing required fields (need name, version, zip_url)", package);
        return 1;
    };

    // The metadata's own "name" must match what was requested.
    if name != package {
        eprintln!("farmer: error: metadata name mismatch -- requested '{}' but server returned metadata for '{}'", package, name);
        return 1;
    }
    println!("farmer: found {} v{}", name, version);

    let zip_path = format!("{}.zip.tmp", package);
    println!("farmer: downloading {}...", zip_url);
    if !download(&zip_url, &zip_path) {
        eprintln!("farmer: error: could not fetch {}", zip_url);
        return 1;
    }
    match &sha256 {
        Some(expected) => {
            println!("farmer: verifying sha256...");
            if !verify_sha256(&zip_path, expected) {
                eprintln!("farmer: error: sha256 mismatch for '{}' -- refusing to install", package);
                let _ = fs::remove_file(&zip_path);
                return 1;
            }
            println!("farmer: checksum OK");
        }
        None => println!("farmer: note: no sha256 provided in metadata, skipping checksum verification"),
    }

    let _ = fs::create_dir(FARMER_MODULES_DIR);
    let dest_dir = format!("{}{}{}", FARMER_MODULES_DIR, PATH_SEP, package);
    println!("farmer: unzipping into {}...", dest_dir);
    if !unzip(&zip_path, Path::new(&dest_dir)) {
        eprintln!("farmer: error: failed to unzip {}", zip_path);
        let _ = fs::remove_file(&zip_path);
        return 1;
    }
    let _ = fs::remove_file(&zip_path);

    println!("farmer: harvested {} v{} -> {}", name, version, dest_dir);
    println!("farmer: use `shuck {}` in your .cob file to import it", package);
    0
}

fn print_usage(argv0: &str) {
    eprint!(
        "Usage: {0} harvest <package>\n\
         \x20      {0} --license\n\
         \x20      {0} --version\n\
         \n\
         \x20 farmer harvest <package>   download and install a Cob package\n\
         \x20                            into {1}/<package>/\n\
         \x20 farmer --license           print the short license notice and exit\n\
         \n\
         Environment:\n\
         \x20 {2}   override the registry base URL (default: {3})\n",
        argv0, FARMER_MODULES_DIR, FARMER_BASE_URL_ENV, FARMER_DEFAULT_BASE_URL
    );
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if check_hidden_version_flag(&args) || check_hidden_license_flag(&args) {
        return;
    }
    let argv0 = args.first().cloned().unwrap_or_else(|| "farmer".into());
    if args.len() < 2 {
        print_usage(&argv0);
        std::process::exit(1);
    }
    if args[1] == "harvest" {
        if args.len() != 3 {
            eprintln!("farmer: error: 'harvest' needs exactly one package name");
            print_usage(&argv0);
            std::process::exit(1);
        }
        std::process::exit(cmd_harvest(&args[2]));
    }
    eprintln!("farmer: error: unknown command '{}'", args[1]);
    print_usage(&argv0);
    std::process::exit(1);
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn package_names() {
        assert!(validate_package_name("my-pkg_2"));
        assert!(!validate_package_name(""));
        assert!(!validate_package_name("a b"));
        assert!(!validate_package_name("; rm -rf ~"));
        assert!(!validate_package_name("../x"));
    }

    #[test]
    fn zip_slip() {
        assert!(is_safe_zip_entry_name("pkg/pkg.cob"));
        assert!(!is_safe_zip_entry_name("../evil"));
        assert!(!is_safe_zip_entry_name("a/../../evil"));
        assert!(!is_safe_zip_entry_name("/etc/passwd"));
        assert!(!is_safe_zip_entry_name("C:/x"));
        assert!(!is_safe_zip_entry_name("a/.."));
        assert!(is_safe_zip_entry_name("a/..b/c"));
    }

    #[test]
    fn json_fields() {
        let j = r#"{ "name" : "p", "version":"1.0", "zip_url": "https:\/\/x\/y.zip" }"#;
        assert_eq!(json_extract_string_field(j, "name").as_deref(), Some("p"));
        assert_eq!(json_extract_string_field(j, "zip_url").as_deref(), Some("https://x/y.zip"));
        assert_eq!(json_extract_string_field(j, "sha256"), None);
    }
}

//! Unpacks the embedded payload. No UI in here, so it can be tested headlessly.

use std::fs;
use std::io::{self, Cursor};
use std::path::{Path, PathBuf};

/// The archive baked into this executable at build time (see build.rs).
pub const PAYLOAD_ZIP: &[u8] = include_bytes!(concat!(env!("OUT_DIR"), "/payload.zip"));

#[derive(Debug, PartialEq)]
pub struct Summary {
    /// files written
    pub written: usize,
    /// files left alone because they already existed and overwrite was off
    pub skipped: usize,
    pub dir: PathBuf,
}

impl Summary {
    pub fn message(&self) -> String {
        let mut m = format!("Extracted {} file{} to {}", self.written, plural(self.written), self.dir.display());
        if self.skipped > 0 {
            m.push_str(&format!(" ({} existing file{} kept)", self.skipped, plural(self.skipped)));
        }
        m
    }
}

fn plural(n: usize) -> &'static str {
    if n == 1 {
        ""
    } else {
        "s"
    }
}

/// When every entry of the archive lives under ONE top-level folder -- as in the
/// release archives (`cob-windows-amd64/cob.exe`, ...) -- that folder's name, so
/// the files can be unpacked straight into the chosen destination instead of
/// into a redundant `Cob/cob-windows-amd64/`. An archive with anything at its
/// top level (or no entries) has no such folder.
fn common_root(archive: &zip::ZipArchive<Cursor<&[u8]>>) -> Option<String> {
    let mut root: Option<&str> = None;
    for name in archive.file_names() {
        let (first, _rest) = name.split_once('/')?; // a file at the top level: nothing to strip
        if first.is_empty() || first == "." || first == ".." {
            return None;
        }
        match root {
            None => root = Some(first),
            Some(r) if r == first => {}
            Some(_) => return None,
        }
    }
    root.map(str::to_owned)
}

/// Unpacks `payload` into `dest`, calling `progress(done, total)` after every entry.
///
/// A single top-level folder wrapping the whole archive is stripped (see `common_root`).
///
/// Entries whose paths would escape `dest` (`../x`, absolute paths) abort the
/// whole extraction -- a setup program must never write outside its folder.
pub fn extract(
    payload: &[u8],
    dest: &Path,
    overwrite: bool,
    mut progress: impl FnMut(usize, usize),
) -> Result<Summary, String> {
    let mut archive =
        zip::ZipArchive::new(Cursor::new(payload)).map_err(|e| format!("Failed to read embedded archive: {e}"))?;
    let total = archive.len();
    let root = common_root(&archive);

    fs::create_dir_all(dest).map_err(|e| format!("Failed to create destination folder: {e}"))?;

    let (mut written, mut skipped) = (0, 0);
    for i in 0..total {
        let mut entry = archive.by_index(i).map_err(|e| format!("Failed to read archive entry {i}: {e}"))?;
        let Some(rel) = entry.enclosed_name() else {
            return Err(format!("Refusing to extract unsafe path from archive: {}", entry.name()));
        };
        let rel = match &root {
            Some(r) => rel.strip_prefix(r).map(Path::to_path_buf).unwrap_or(rel),
            None => rel,
        };
        if rel.as_os_str().is_empty() {
            // the wrapper folder's own entry
            progress(i + 1, total);
            continue;
        }
        let out = dest.join(rel);

        if entry.is_dir() {
            fs::create_dir_all(&out).map_err(|e| format!("Failed to create {}: {e}", out.display()))?;
        } else if out.is_file() && !overwrite {
            skipped += 1;
        } else {
            if let Some(parent) = out.parent() {
                fs::create_dir_all(parent).map_err(|e| format!("Failed to create {}: {e}", parent.display()))?;
            }
            let mut file =
                fs::File::create(&out).map_err(|e| format!("Failed to write {}: {e}", out.display()))?;
            io::copy(&mut entry, &mut file).map_err(|e| format!("Failed to write {}: {e}", out.display()))?;
            #[cfg(unix)]
            if let Some(mode) = entry.unix_mode() {
                use std::os::unix::fs::PermissionsExt;
                // keep the executable bits (cob must stay runnable)
                let _ = fs::set_permissions(&out, fs::Permissions::from_mode(mode & 0o777));
            }
            written += 1;
        }
        progress(i + 1, total);
    }

    Ok(Summary { written, skipped, dir: dest.to_path_buf() })
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;
    use zip::write::SimpleFileOptions;

    fn zip_of(entries: &[(&str, &[u8])]) -> Vec<u8> {
        let mut w = zip::ZipWriter::new(Cursor::new(Vec::new()));
        for (name, data) in entries {
            if name.ends_with('/') {
                w.add_directory(*name, SimpleFileOptions::default()).unwrap();
            } else {
                w.start_file(*name, SimpleFileOptions::default().unix_permissions(0o755)).unwrap();
                w.write_all(data).unwrap();
            }
        }
        w.finish().unwrap().into_inner()
    }

    fn scratch(name: &str) -> PathBuf {
        let p = std::env::temp_dir().join(format!("cob_installer_test_{}_{}", name, std::process::id()));
        let _ = fs::remove_dir_all(&p);
        p
    }

    #[test]
    fn extracts_files_and_folders() {
        let dir = scratch("basic");
        let z = zip_of(&[("lib/", b""), ("cob_interp", b"bin"), ("lib/bushel.cob", b"extern")]);
        let s = extract(&z, &dir, true, |_, _| {}).unwrap();
        assert_eq!((s.written, s.skipped), (2, 0));
        assert_eq!(fs::read(dir.join("cob_interp")).unwrap(), b"bin");
        assert_eq!(fs::read(dir.join("lib/bushel.cob")).unwrap(), b"extern");
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn overwrite_on_replaces_and_off_keeps() {
        let dir = scratch("overwrite");
        fs::create_dir_all(&dir).unwrap();
        fs::write(dir.join("VERSION"), "old").unwrap();
        let z = zip_of(&[("VERSION", b"0.0.10")]);

        let s = extract(&z, &dir, false, |_, _| {}).unwrap();
        assert_eq!((s.written, s.skipped), (0, 1));
        assert_eq!(fs::read_to_string(dir.join("VERSION")).unwrap(), "old");
        assert!(s.message().contains("1 existing file kept"));

        let s = extract(&z, &dir, true, |_, _| {}).unwrap();
        assert_eq!((s.written, s.skipped), (1, 0));
        assert_eq!(fs::read_to_string(dir.join("VERSION")).unwrap(), "0.0.10");
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn refuses_paths_that_escape_the_folder() {
        let dir = scratch("slip");
        let outside = dir.parent().unwrap().join(format!("cob_evil_{}.txt", std::process::id()));
        let _ = fs::remove_file(&outside);
        let evil = format!("../cob_evil_{}.txt", std::process::id());
        let z = zip_of(&[(evil.as_str(), b"pwned")]);
        let err = extract(&z, &dir, true, |_, _| {}).unwrap_err();
        assert!(err.contains("unsafe path"), "{err}");
        assert!(!outside.exists(), "wrote outside the destination folder");
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn progress_counts_every_entry() {
        let dir = scratch("progress");
        let z = zip_of(&[("a", b"1"), ("b", b"2"), ("c/", b""), ("c/d", b"3")]);
        let mut seen = Vec::new();
        extract(&z, &dir, true, |d, t| seen.push((d, t))).unwrap();
        assert_eq!(seen, vec![(1, 4), (2, 4), (3, 4), (4, 4)]);
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn strips_a_single_wrapper_folder() {
        // the shape of the release archives: everything under cob-windows-amd64/
        let dir = scratch("wrapper");
        let z = zip_of(&[
            ("cob-windows-amd64/", b""),
            ("cob-windows-amd64/cob.exe", b"bin"),
            ("cob-windows-amd64/lib/", b""),
            ("cob-windows-amd64/lib/bushel.cob", b"extern"),
        ]);
        let mut last = (0, 0);
        let s = extract(&z, &dir, true, |d, t| last = (d, t)).unwrap();
        assert_eq!((s.written, s.skipped), (2, 0));
        assert_eq!(last, (4, 4)); // the wrapper's own entry still counts as progress
        assert_eq!(fs::read(dir.join("cob.exe")).unwrap(), b"bin");
        assert_eq!(fs::read(dir.join("lib/bushel.cob")).unwrap(), b"extern");
        assert!(!dir.join("cob-windows-amd64").exists());
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn keeps_folders_when_there_is_more_than_one_top_level_entry() {
        let dir = scratch("nostrip");
        let z = zip_of(&[("a/x", b"1"), ("b/y", b"2")]);
        extract(&z, &dir, true, |_, _| {}).unwrap();
        assert!(dir.join("a/x").is_file() && dir.join("b/y").is_file());
        let _ = fs::remove_dir_all(&dir);

        // one folder plus a loose file at the top: nothing is stripped either
        let dir = scratch("nostrip2");
        let z = zip_of(&[("pkg/x", b"1"), ("VERSION", b"0.0.10")]);
        extract(&z, &dir, true, |_, _| {}).unwrap();
        assert!(dir.join("pkg/x").is_file() && dir.join("VERSION").is_file());
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn a_lone_top_level_file_is_not_a_wrapper() {
        let dir = scratch("lonefile");
        let z = zip_of(&[("cob", b"bin")]);
        extract(&z, &dir, true, |_, _| {}).unwrap();
        assert_eq!(fs::read(dir.join("cob")).unwrap(), b"bin");
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn unsafe_paths_are_still_refused_inside_a_wrapper() {
        let dir = scratch("wrapslip");
        let z = zip_of(&[("pkg/", b""), ("pkg/../../evil.txt", b"x")]);
        assert!(extract(&z, &dir, true, |_, _| {}).is_err());
        let _ = fs::remove_dir_all(&dir);
    }

    #[cfg(unix)]
    #[test]
    fn keeps_executable_bit() {
        use std::os::unix::fs::PermissionsExt;
        let dir = scratch("perm");
        let z = zip_of(&[("cob_interp", b"x")]);
        extract(&z, &dir, true, |_, _| {}).unwrap();
        assert_eq!(fs::metadata(dir.join("cob_interp")).unwrap().permissions().mode() & 0o111, 0o111);
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn garbage_payload_is_an_error_not_a_panic() {
        let dir = scratch("garbage");
        assert!(extract(b"not a zip", &dir, true, |_, _| {}).is_err());
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn the_embedded_payload_is_readable() {
        // the empty placeholder (debug) or the real archive (release) both parse
        assert!(zip::ZipArchive::new(Cursor::new(PAYLOAD_ZIP)).is_ok());
    }
}

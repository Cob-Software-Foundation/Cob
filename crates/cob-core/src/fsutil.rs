//! Whole-file helpers (twin of file_io.c).

use std::fs;
use std::path::Path;

/// Reads a file fully. For text sources the data stops at the first NUL byte,
/// because the language has always treated source as a C string.
pub fn read_all(path: impl AsRef<Path>) -> Option<Vec<u8>> {
    fs::read(path).ok()
}

/// Source text as the parser sees it: cut at the first NUL.
pub fn as_c_text(mut data: Vec<u8>) -> Vec<u8> {
    if let Some(z) = data.iter().position(|&b| b == 0) {
        data.truncate(z);
    }
    data
}

pub fn write_all(path: impl AsRef<Path>, data: &[u8]) -> bool {
    fs::write(path, data).is_ok()
}

pub fn exists(path: impl AsRef<Path>) -> bool {
    fs::File::open(path).is_ok()
}

/// Bytes -> path, byte-exact on Unix.
pub fn path_from_bytes(b: &[u8]) -> std::path::PathBuf {
    #[cfg(unix)]
    {
        use std::os::unix::ffi::OsStrExt;
        std::path::PathBuf::from(std::ffi::OsStr::from_bytes(b))
    }
    #[cfg(not(unix))]
    {
        std::path::PathBuf::from(String::from_utf8_lossy(b).into_owned())
    }
}

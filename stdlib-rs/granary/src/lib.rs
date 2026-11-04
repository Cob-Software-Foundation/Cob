//! granary -- files, stored like grain             (shuck granary)
//!
//!   granary_read(path)           the whole file as a string ("" if it can't be read)
//!   granary_write(path, text)    replace the file's contents; 0 on success, 1 on failure
//!   granary_append(path, text)   add to the end of the file; 0 on success, 1 on failure
//!   granary_exists(path)         1 if the file can be opened, else 0
//!   granary_remove(path)         delete the file; 0 on success, 1 on failure
//! Text only: a file's contents are read up to its first zero byte.

use std::ffi::CString;
use std::fs::{File, OpenOptions};
use std::io::{Read, Write};
use std::os::raw::{c_char, c_int};

use cob_abi::*;

const MOD: &str = "granary";
const GRANARY_MAX_READ: u64 = 256 * 1024 * 1024;
static READ: Slot = Slot::new();

fn to_path(b: &[u8]) -> std::path::PathBuf {
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

#[no_mangle]
pub unsafe extern "C" fn granary_read(argv: *mut CobValue, argc: c_int) -> CobValue {
    if !arity(MOD, "granary_read", argc, 1) {
        return cob_empty_str();
    }
    let path = arg_str(MOD, "granary_read", argv, argc, 0);
    let Ok(mut fp) = File::open(to_path(path)) else {
        ewrite(&format!("[{}] warning: granary_read() could not open '{}'\n", MOD, lossy(path)));
        return READ.ret(b"");
    };
    let len = match fp.metadata() {
        Ok(m) if m.is_file() => m.len(),
        _ => {
            ewrite(&format!("[{}] warning: granary_read() could not size '{}'\n", MOD, lossy(path)));
            return READ.ret(b"");
        }
    };
    if len > GRANARY_MAX_READ {
        ewrite(&format!("[{}] warning: granary_read() '{}' is too large\n", MOD, lossy(path)));
        return READ.ret(b"");
    }
    let mut buf = Vec::with_capacity(len as usize);
    let _ = (&mut fp).take(len).read_to_end(&mut buf);
    // text only: stop at the first zero byte
    if let Some(z) = buf.iter().position(|&b| b == 0) {
        buf.truncate(z);
    }
    READ.ret(&buf)
}

unsafe fn granary_put(func: &str, append: bool, argv: *mut CobValue, argc: c_int) -> CobValue {
    if !arity(MOD, func, argc, 2) {
        return cob_int(1);
    }
    let path = arg_str(MOD, func, argv, argc, 0);
    let text = arg_str(MOD, func, argv, argc, 1);
    let opened = if append {
        OpenOptions::new().append(true).create(true).open(to_path(path))
    } else {
        File::create(to_path(path))
    };
    let Ok(mut fp) = opened else {
        ewrite(&format!("[{}] warning: {}() could not open '{}'\n", MOD, func, lossy(path)));
        return cob_int(1);
    };
    if fp.write_all(text).is_err() {
        ewrite(&format!("[{}] warning: {}() could not write '{}'\n", MOD, func, lossy(path)));
        return cob_int(1);
    }
    cob_int(if fp.flush().is_ok() { 0 } else { 1 })
}

#[no_mangle]
pub unsafe extern "C" fn granary_write(argv: *mut CobValue, argc: c_int) -> CobValue {
    granary_put("granary_write", false, argv, argc)
}

#[no_mangle]
pub unsafe extern "C" fn granary_append(argv: *mut CobValue, argc: c_int) -> CobValue {
    granary_put("granary_append", true, argv, argc)
}

#[no_mangle]
pub unsafe extern "C" fn granary_exists(argv: *mut CobValue, argc: c_int) -> CobValue {
    if !arity(MOD, "granary_exists", argc, 1) {
        return cob_int(0);
    }
    let path = arg_str(MOD, "granary_exists", argv, argc, 0);
    cob_int(File::open(to_path(path)).is_ok() as i64)
}

extern "C" {
    fn remove(path: *const c_char) -> c_int;
}

#[no_mangle]
pub unsafe extern "C" fn granary_remove(argv: *mut CobValue, argc: c_int) -> CobValue {
    if !arity(MOD, "granary_remove", argc, 1) {
        return cob_int(1);
    }
    let path = arg_str(MOD, "granary_remove", argv, argc, 0);
    // C's remove(): deletes a file, or an empty directory
    let Ok(c) = CString::new(path) else { return cob_int(1) };
    cob_int(if remove(c.as_ptr()) == 0 { 0 } else { 1 })
}

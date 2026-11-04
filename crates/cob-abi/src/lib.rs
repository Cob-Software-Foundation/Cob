//! cob-abi -- the native-extension ABI (CEP-2) and the helpers every Cob
//! standard-library module shares. This is the Rust twin of `include/cob.h`
//! (which stays, for people writing extensions in C) and `stdlib/stdhelp.h`.
//!
//! A native function has exactly one signature, whatever its arity:
//!
//! ```c
//! CobValue f(CobValue *argv, int argc);
//! ```
//!
//! Values are an integer (C `long`) or a NUL-terminated string. Argument
//! strings are owned by the interpreter and valid only during the call; a
//! returned string is copied immediately and never freed by Cob.

#[cfg(not(target_arch = "wasm32"))]
use std::io::Write;
use std::os::raw::{c_char, c_int};
#[cfg(not(target_arch = "wasm32"))]
use std::os::raw::c_long;
use std::sync::Mutex;

pub mod cstdio;
#[cfg(target_arch = "wasm32")]
pub mod host;
pub mod num;

/// The ABI's integer: C `long` (see include/cob.h) -- except in the browser
/// build, where C's `long` would be 32 bits. There the ABI never leaves the
/// module (nothing is dlopen()ed), so numbers stay 64-bit, as everywhere else.
#[cfg(not(target_arch = "wasm32"))]
pub type CobLong = c_long;
#[cfg(target_arch = "wasm32")]
pub type CobLong = i64;

pub const COB_INT: c_int = 0;
pub const COB_STR: c_int = 1;

#[repr(C)]
#[derive(Copy, Clone)]
pub union CobUnion {
    pub i: CobLong,
    pub s: *const c_char,
}

/// Layout-identical to `CobValue` in include/cob.h.
#[repr(C)]
#[derive(Copy, Clone)]
pub struct CobValue {
    pub kind: c_int,
    pub as_: CobUnion,
}

/// `CobFFIFunc` from cob.h.
pub type CobFFIFunc = unsafe extern "C" fn(argv: *mut CobValue, argc: c_int) -> CobValue;

pub fn cob_int(v: i64) -> CobValue {
    CobValue { kind: COB_INT, as_: CobUnion { i: v as CobLong } }
}
/// `s` must be NUL-terminated and outlive the interpreter's copy of it.
pub fn cob_str(s: *const c_char) -> CobValue {
    CobValue { kind: COB_STR, as_: CobUnion { s } }
}
pub fn cob_empty_str() -> CobValue {
    cob_str(b"\0".as_ptr() as *const c_char)
}

/// Writes diagnostics to stderr (never panics if stderr is closed).
#[cfg(not(target_arch = "wasm32"))]
pub fn ewrite(msg: &str) {
    let mut e = std::io::stderr();
    let _ = e.write_all(msg.as_bytes());
}

/// In the browser build stderr is a host function (std's own stderr is a no-op there).
#[cfg(target_arch = "wasm32")]
pub fn ewrite(msg: &str) {
    cstdio::write_stderr(msg.as_bytes());
}

/// Lossy text of a byte string, for messages.
pub fn lossy(b: &[u8]) -> String {
    String::from_utf8_lossy(b).into_owned()
}

/// Checks the argument count; prints a warning and returns false on mismatch.
pub fn arity(module: &str, func: &str, argc: c_int, want: c_int) -> bool {
    if argc == want {
        return true;
    }
    ewrite(&format!(
        "[{}] warning: {}() takes {} argument{}, got {}\n",
        module,
        func,
        want,
        if want == 1 { "" } else { "s" },
        argc
    ));
    false
}

/// i-th argument as a number; a wrong kind warns and gives 0.
///
/// # Safety
/// `argv` must point to `argc` valid values.
pub unsafe fn arg_int(module: &str, func: &str, argv: *mut CobValue, argc: c_int, i: c_int) -> i64 {
    if i < argc && (*argv.add(i as usize)).kind == COB_INT {
        return (*argv.add(i as usize)).as_.i as i64;
    }
    ewrite(&format!(
        "[{}] warning: argument {} of {}() must be a number; treated as 0\n",
        module,
        i + 1,
        func
    ));
    0
}

/// i-th argument as a string (without its NUL); a wrong kind warns and gives "".
///
/// # Safety
/// `argv` must point to `argc` valid values whose strings are NUL-terminated.
pub unsafe fn arg_str<'a>(module: &str, func: &str, argv: *mut CobValue, argc: c_int, i: c_int) -> &'a [u8] {
    if i < argc {
        let v = *argv.add(i as usize);
        if v.kind == COB_STR && !v.as_.s.is_null() {
            return std::ffi::CStr::from_ptr(v.as_.s).to_bytes();
        }
    }
    ewrite(&format!(
        "[{}] warning: argument {} of {}() must be a string; treated as \"\"\n",
        module,
        i + 1,
        func
    ));
    b""
}

/// Where a function's string result lives until its next call -- Cob copies
/// the result right after the call returns (cob.h ownership rules).
pub struct Slot(Mutex<Vec<u8>>);

impl Slot {
    pub const fn new() -> Slot {
        Slot(Mutex::new(Vec::new()))
    }
    pub fn ret(&self, text: &[u8]) -> CobValue {
        let mut g = self.0.lock().unwrap_or_else(|e| e.into_inner());
        g.clear();
        g.extend_from_slice(text);
        g.push(0);
        cob_str(g.as_ptr() as *const c_char)
    }
}

impl Default for Slot {
    fn default() -> Self {
        Slot::new()
    }
}

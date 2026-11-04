//! Access to the C library's own `stdin` / `stdout`.
//!
//! Cob deliberately shares ONE set of stdio buffers between the interpreter,
//! the standard-library modules and any C extension: a `pop()` followed by a
//! module's prompt followed by a C `printf()` must come out in that order, and
//! a program compiled by popcorn_comp (plain C stdio) must behave the same as
//! the interpreter. Rust's `std::io::stdout()` has its own buffer, so it would
//! reorder against all of those -- hence this thin layer over libc.

#[cfg(not(target_arch = "wasm32"))]
pub use self::native::*;
#[cfg(target_arch = "wasm32")]
pub use self::browser::*;

/// Shared by both backends: prompt = print without newline, then flush.
pub fn prompt(text: &[u8]) {
    write_stdout(text);
    flush_stdout();
}

#[cfg(not(target_arch = "wasm32"))]
mod native {
    use std::os::raw::{c_int, c_void};

    #[repr(C)]
    pub struct FILE {
        _private: [u8; 0],
    }

    /// The C library's `fwrite`/`fflush`/`fgetc` and its `stdin`/`stdout`,
    /// always taken from ONE place so they can never belong to different
    /// runtimes.
    ///
    /// Unix: plain imports. Windows: resolved at run time. There is more than
    /// one C runtime a Windows process can have (the UCRT that Zig-built programs
    /// and most C extensions use, or the old msvcrt that Rust's `-gnu` targets
    /// link), and `stdout` lives in a different place in each -- a link-time
    /// reference would fail to link against the wrong one. We prefer the UCRT, so
    /// our buffer is the SAME one a compiled Cob program and C extensions print
    /// through, and fall back to msvcrt.
    #[cfg(not(windows))]
    mod crt {
        use super::FILE;
        use std::os::raw::{c_int, c_void};

        extern "C" {
            fn fwrite(ptr: *const c_void, size: usize, n: usize, f: *mut FILE) -> usize;
            fn fflush(f: *mut FILE) -> c_int;
            fn fgetc(f: *mut FILE) -> c_int;
        }

        #[cfg(not(any(target_os = "macos", target_os = "ios")))]
        extern "C" {
            #[link_name = "stdin"]
            static C_STDIN: *mut FILE;
            #[link_name = "stdout"]
            static C_STDOUT: *mut FILE;
        }
        #[cfg(any(target_os = "macos", target_os = "ios"))]
        extern "C" {
            #[link_name = "__stdinp"]
            static C_STDIN: *mut FILE;
            #[link_name = "__stdoutp"]
            static C_STDOUT: *mut FILE;
        }

        pub unsafe fn c_fwrite(p: *const c_void, size: usize, n: usize) -> usize {
            fwrite(p, size, n, C_STDOUT)
        }
        pub unsafe fn c_fflush_stdout() {
            fflush(C_STDOUT);
        }
        pub unsafe fn c_fgetc_stdin() -> c_int {
            fgetc(C_STDIN)
        }
    }

    #[cfg(windows)]
    mod crt {
        use super::FILE;
        use std::os::raw::{c_char, c_int, c_void};
        use std::sync::OnceLock;

        extern "system" {
            fn LoadLibraryA(name: *const c_char) -> *mut c_void;
            fn GetProcAddress(module: *mut c_void, name: *const c_char) -> *mut c_void;
        }

        type FwriteFn = unsafe extern "C" fn(*const c_void, usize, usize, *mut FILE) -> usize;
        type FflushFn = unsafe extern "C" fn(*mut FILE) -> c_int;
        type FgetcFn = unsafe extern "C" fn(*mut FILE) -> c_int;

        struct Crt {
            fwrite: FwriteFn,
            fflush: FflushFn,
            fgetc: FgetcFn,
            stdin: *mut FILE,
            stdout: *mut FILE,
        }
        // The CRT's own globals; the CRT locks its streams itself.
        unsafe impl Send for Crt {}
        unsafe impl Sync for Crt {}

        static CRT: OnceLock<Crt> = OnceLock::new();

        unsafe fn sym(module: *mut c_void, name: &'static [u8]) -> *mut c_void {
            GetProcAddress(module, name.as_ptr() as *const c_char)
        }

        unsafe fn load() -> Crt {
            // UCRT first
            let ucrt = LoadLibraryA(b"ucrtbase.dll\0".as_ptr() as *const c_char);
            let (module, stdin, stdout);
            let iob = if ucrt.is_null() { std::ptr::null_mut() } else { sym(ucrt, b"__acrt_iob_func\0") };
            if !iob.is_null() {
                let f: unsafe extern "C" fn(u32) -> *mut FILE = std::mem::transmute(iob);
                module = ucrt;
                stdin = f(0);
                stdout = f(1);
            } else {
                // the old msvcrt: stdin/stdout are the first two entries of __iob_func()
                let m = LoadLibraryA(b"msvcrt.dll\0".as_ptr() as *const c_char);
                let iob_func = if m.is_null() { std::ptr::null_mut() } else { sym(m, b"__iob_func\0") };
                assert!(!iob_func.is_null(), "cob: no usable C runtime (neither ucrtbase.dll nor msvcrt.dll)");
                let f: unsafe extern "C" fn() -> *mut u8 = std::mem::transmute(iob_func);
                let base = f();
                // sizeof(FILE) in msvcrt.dll
                let size = if cfg!(target_pointer_width = "64") { 48 } else { 32 };
                module = m;
                stdin = base as *mut FILE;
                stdout = base.add(size) as *mut FILE;
            }
            let get = |name: &'static [u8]| {
                let p = sym(module, name);
                assert!(!p.is_null(), "cob: the C runtime lacks a stdio function");
                p
            };
            Crt {
                fwrite: std::mem::transmute(get(b"fwrite\0")),
                fflush: std::mem::transmute(get(b"fflush\0")),
                fgetc: std::mem::transmute(get(b"fgetc\0")),
                stdin,
                stdout,
            }
        }

        fn crt() -> &'static Crt {
            CRT.get_or_init(|| unsafe { load() })
        }

        pub unsafe fn c_fwrite(p: *const c_void, size: usize, n: usize) -> usize {
            let c = crt();
            (c.fwrite)(p, size, n, c.stdout)
        }
        pub unsafe fn c_fflush_stdout() {
            let c = crt();
            (c.fflush)(c.stdout);
        }
        pub unsafe fn c_fgetc_stdin() -> c_int {
            let c = crt();
            (c.fgetc)(c.stdin)
        }
    }

    /// Writes bytes to C `stdout` (buffered exactly like C's `fwrite`).
    pub fn write_stdout(bytes: &[u8]) {
        if bytes.is_empty() {
            return;
        }
        unsafe {
            crt::c_fwrite(bytes.as_ptr() as *const c_void, 1, bytes.len());
        }
    }

    pub fn flush_stdout() {
        unsafe {
            crt::c_fflush_stdout();
        }
    }

    /// Reads one line from C `stdin`. Returns `None` at end of input (nothing was
    /// read); otherwise the line without its `\n` / `\r\n`, cut at a NUL byte the
    /// way the C string functions always cut it.
    pub fn read_line() -> Option<Vec<u8>> {
        let mut buf: Vec<u8> = Vec::new();
        let mut got = false;
        loop {
            let c = unsafe { crt::c_fgetc_stdin() };
            if c < 0 {
                break;
            }
            got = true;
            buf.push(c as u8);
            if c == b'\n' as c_int {
                break;
            }
        }
        if !got {
            return None;
        }
        if let Some(z) = buf.iter().position(|&b| b == 0) {
            buf.truncate(z);
        }
        while matches!(buf.last(), Some(b'\n') | Some(b'\r')) {
            buf.pop();
        }
        Some(buf)
    }


}

/// WebAssembly backend (the browser playground). A `wasm32-unknown-unknown`
/// module has no libc, so console I/O goes through functions the host page
/// provides (see docs/play/cob_interp.js).
#[cfg(target_arch = "wasm32")]
mod browser {
    extern "C" {
        /// fd 1 = stdout, fd 2 = stderr
        fn cob_host_write(fd: i32, ptr: *const u8, len: usize);
        fn cob_host_flush();
        /// next input byte, or -1 at end of input
        fn cob_host_read_byte() -> i32;
    }

    pub fn write_stdout(bytes: &[u8]) {
        if !bytes.is_empty() {
            unsafe { cob_host_write(1, bytes.as_ptr(), bytes.len()) }
        }
    }

    pub fn write_stderr(bytes: &[u8]) {
        if !bytes.is_empty() {
            unsafe { cob_host_write(2, bytes.as_ptr(), bytes.len()) }
        }
    }

    pub fn flush_stdout() {
        unsafe { cob_host_flush() }
    }

    /// Same contract as the native `read_line`.
    pub fn read_line() -> Option<Vec<u8>> {
        let mut buf: Vec<u8> = Vec::new();
        let mut got = false;
        loop {
            let c = unsafe { cob_host_read_byte() };
            if c < 0 {
                break;
            }
            got = true;
            buf.push(c as u8);
            if c == b'\n' as i32 {
                break;
            }
        }
        if !got {
            return None;
        }
        if let Some(z) = buf.iter().position(|&b| b == 0) {
            buf.truncate(z);
        }
        while matches!(buf.last(), Some(b'\n') | Some(b'\r')) {
            buf.pop();
        }
        Some(buf)
    }
}

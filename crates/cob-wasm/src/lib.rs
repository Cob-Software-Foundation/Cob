//! cob-wasm -- the Cob interpreter compiled to WebAssembly, for the browser
//! playground (docs/play/). Same engine as `cob_interp` (it IS cob-core), with
//! the sandbox's differences:
//!
//!   * no files: the program text is handed in, and every standard-library
//!     `.cob` wrapper is compiled in so `shuck porch` just works;
//!   * no dlopen(): porch, cornmath, kernels and almanac are linked in and bound
//!     by name; any other `extern` (pantry, granary, barn, your own C) is a
//!     no-op stub that prints one notice per library;
//!   * console, clock and sleep come from the page (see docs/play/cob_interp.js).
//!
//! One module instance is one run: the page instantiates a fresh module per
//! Run click, so no state ever leaks between runs.

use std::os::raw::c_int;

use cob_abi::CobFFIFunc;
use cob_core::ast::program_uses_harvest_or_trash;
use cob_core::fsutil;
use cob_core::Session;

/// Every standard-library wrapper, so `shuck <module>` needs no files.
static WRAPPERS: &[(&str, &str)] = &[
    ("almanac", include_str!("../../../stdlib/almanac.cob")),
    ("barn", include_str!("../../../stdlib/barn.cob")),
    ("cornmath", include_str!("../../../stdlib/cornmath.cob")),
    ("granary", include_str!("../../../stdlib/granary.cob")),
    ("kernels", include_str!("../../../stdlib/kernels.cob")),
    ("pantry", include_str!("../../../stdlib/pantry.cob")),
    ("porch", include_str!("../../../stdlib/porch.cob")),
    ("silo", include_str!("../../../stdlib/silo.cob")),
];

/// The linked-in modules' functions, as one table.
fn static_table() -> &'static [(&'static str, CobFFIFunc)] {
    let mut all: Vec<(&'static str, CobFFIFunc)> = Vec::new();
    for t in [porch::EXPORTS, cornmath::EXPORTS, kernels::EXPORTS, almanac::EXPORTS] {
        all.extend_from_slice(t);
    }
    // one table per run (the instance is thrown away afterwards)
    Box::leak(all.into_boxed_slice())
}

/// Flag bits for `cob_run`.
const FLAG_NO_GC: c_int = 1;

/// Memory the host writes the program text into.
#[no_mangle]
pub extern "C" fn cob_alloc(len: usize) -> *mut u8 {
    let mut v: Vec<u8> = Vec::with_capacity(len.max(1));
    let p = v.as_mut_ptr();
    std::mem::forget(v);
    p
}

/// # Safety
/// `ptr`/`len` must be exactly what `cob_alloc(len)` returned.
#[no_mangle]
pub unsafe extern "C" fn cob_free(ptr: *mut u8, len: usize) {
    drop(Vec::from_raw_parts(ptr, 0, len.max(1)));
}

/// Runs one program. Returns the process-style exit code: 0, or 1 after a
/// parse/check error (already reported through the host's stderr).
///
/// The sequence is exactly `cob_interp <file> --no-cache`: parse, validate and
/// register every recipe, check every call, then execute.
///
/// # Safety
/// `src` must point to `len` readable bytes.
#[no_mangle]
pub unsafe extern "C" fn cob_run(src: *const u8, len: usize, flags: c_int) -> c_int {
    let source = fsutil::as_c_text(std::slice::from_raw_parts(src, len).to_vec());
    let no_gc = flags & FLAG_NO_GC != 0;

    let mut sess = Session::new(no_gc);
    sess.static_externs = static_table();
    sess.embedded_wrappers = WRAPPERS;

    let Ok((prog, _)) = sess.parse_source(&source) else { return 1 };
    if sess.recipes_validate(&prog, false).is_err() || sess.check_calls(&prog, false).is_err() {
        return 1;
    }
    sess.recipes_register(&prog);
    if !no_gc && program_uses_harvest_or_trash(&prog) {
        cob_abi::ewrite("[cob_interp] error: this program uses harvest()/trash() -- rerun with --no-gc\n");
        return 1;
    }
    sess.execute(&prog);
    cob_abi::cstdio::flush_stdout();
    0
}

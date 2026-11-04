//! porch -- sit on the porch and talk to whoever is at the keyboard
//!                                                  (shuck porch)
//!
//!   porch_ask(prompt)      print the prompt (no newline), read one line,
//!                          return it as a string without the line ending.
//!                          "" at end of input. The prompt is optional.
//!   porch_ask_int(prompt)  the same, but read the line as a whole number.
//!                          Not a number (or end of input) gives 0, with a
//!                          warning for the first case.

use std::os::raw::c_int;

use cob_abi::cstdio::{prompt, read_line};
use cob_abi::num::parse_whole_number;
use cob_abi::*;

const MOD: &str = "porch";
static LINE: Slot = Slot::new();

unsafe fn prompt_text(argv: *mut CobValue, argc: c_int) -> Vec<u8> {
    if argc < 1 {
        return Vec::new();
    }
    let v = *argv;
    if v.kind == COB_STR {
        if v.as_.s.is_null() {
            Vec::new()
        } else {
            std::ffi::CStr::from_ptr(v.as_.s).to_bytes().to_vec()
        }
    } else {
        v.as_.i.to_string().into_bytes()
    }
}

/// Prints the prompt, flushes, reads a line. `None` at end of input.
fn ask(p: &[u8]) -> Option<Vec<u8>> {
    prompt(p);
    read_line()
}

#[no_mangle]
pub unsafe extern "C" fn porch_ask(argv: *mut CobValue, argc: c_int) -> CobValue {
    let p = prompt_text(argv, argc);
    let line = ask(&p).unwrap_or_default();
    LINE.ret(&line)
}

#[no_mangle]
pub unsafe extern "C" fn porch_ask_int(argv: *mut CobValue, argc: c_int) -> CobValue {
    let p = prompt_text(argv, argc);
    let got = ask(&p);
    let (line, had_line) = match got {
        Some(l) => (l, true),
        None => (Vec::new(), false),
    };
    match parse_whole_number(&line) {
        Some(n) => cob_int(n),
        None => {
            if had_line {
                ewrite(&format!(
                    "[{}] warning: porch_ask_int() got non-numeric input \"{}\"; treated as 0\n",
                    MOD,
                    lossy(&line)
                ));
            }
            cob_int(0)
        }
    }
}

/// Every function this module exports, by name -- for builds that cannot
/// `dlopen()` (the browser playground) and bind the module in statically.
pub static EXPORTS: &[(&str, cob_abi::CobFFIFunc)] = &[
    ("porch_ask", porch_ask),
    ("porch_ask_int", porch_ask_int),
];

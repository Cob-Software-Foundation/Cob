//! kernels -- text, kernel by kernel            (shuck kernels)
//!
//!   kernel_len(s)                  number of characters (bytes)
//!   kernel_upper(s) / kernel_lower(s)   ASCII case change
//!   kernel_slice(s, start, count)  `count` characters from `start` (0-based)
//!   kernel_find(hay, needle)       index of the first match, or -1
//!   kernel_repeat(s, n)            s written n times
//!   kernel_trim(s)                 without leading/trailing blanks
//!   kernel_replace(s, from, to)    every `from` swapped for `to`
//!   kernel_char(s, i)              the one-character string at index i ("" if none)
//!   kernel_code(s)                 byte value of the first character (0 if empty)
//!   kernel_from_code(n)            the one-character string with byte value n (1..255)
//!   kernel_to_int(s)               s read as a whole number (0 if it isn't one)
//!   kernel_starts(s, prefix) / kernel_ends(s, suffix)   1 or 0

use std::os::raw::c_int;

use cob_abi::num::parse_whole_number;
use cob_abi::*;

const MOD: &str = "kernels";
const KERNEL_MAX_RESULT: usize = 64 * 1024 * 1024;

static UPPER: Slot = Slot::new();
static LOWER: Slot = Slot::new();
static SLICE: Slot = Slot::new();
static REPEAT: Slot = Slot::new();
static TRIM: Slot = Slot::new();
static REPLACE: Slot = Slot::new();
static CHAR: Slot = Slot::new();
static CHR: Slot = Slot::new();

/// C `isspace` in the "C" locale.
fn is_space(b: u8) -> bool {
    matches!(b, b' ' | b'\t' | b'\n' | 0x0b | 0x0c | b'\r')
}

/// `strstr`: first index of `needle` in `hay` (an empty needle matches at 0).
fn find(hay: &[u8], needle: &[u8]) -> Option<usize> {
    if needle.is_empty() {
        return Some(0);
    }
    if needle.len() > hay.len() {
        return None;
    }
    hay.windows(needle.len()).position(|w| w == needle)
}

#[no_mangle]
pub unsafe extern "C" fn kernel_len(argv: *mut CobValue, argc: c_int) -> CobValue {
    if !arity(MOD, "kernel_len", argc, 1) {
        return cob_int(0);
    }
    cob_int(arg_str(MOD, "kernel_len", argv, argc, 0).len() as i64)
}

#[no_mangle]
pub unsafe extern "C" fn kernel_upper(argv: *mut CobValue, argc: c_int) -> CobValue {
    if !arity(MOD, "kernel_upper", argc, 1) {
        return cob_empty_str();
    }
    let s = arg_str(MOD, "kernel_upper", argv, argc, 0);
    UPPER.ret(&s.to_ascii_uppercase())
}

#[no_mangle]
pub unsafe extern "C" fn kernel_lower(argv: *mut CobValue, argc: c_int) -> CobValue {
    if !arity(MOD, "kernel_lower", argc, 1) {
        return cob_empty_str();
    }
    let s = arg_str(MOD, "kernel_lower", argv, argc, 0);
    LOWER.ret(&s.to_ascii_lowercase())
}

#[no_mangle]
pub unsafe extern "C" fn kernel_slice(argv: *mut CobValue, argc: c_int) -> CobValue {
    if !arity(MOD, "kernel_slice", argc, 3) {
        return cob_empty_str();
    }
    let s = arg_str(MOD, "kernel_slice", argv, argc, 0);
    let mut start = arg_int(MOD, "kernel_slice", argv, argc, 1);
    let mut count = arg_int(MOD, "kernel_slice", argv, argc, 2);
    let n = s.len();
    if start < 0 {
        start = 0;
    }
    if count < 0 {
        count = 0;
    }
    if start as u64 >= n as u64 {
        return SLICE.ret(b"");
    }
    let start = start as usize;
    let count = (count as u64).min((n - start) as u64) as usize;
    SLICE.ret(&s[start..start + count])
}

#[no_mangle]
pub unsafe extern "C" fn kernel_find(argv: *mut CobValue, argc: c_int) -> CobValue {
    if !arity(MOD, "kernel_find", argc, 2) {
        return cob_int(-1);
    }
    let h = arg_str(MOD, "kernel_find", argv, argc, 0);
    let nd = arg_str(MOD, "kernel_find", argv, argc, 1);
    cob_int(find(h, nd).map_or(-1, |i| i as i64))
}

#[no_mangle]
pub unsafe extern "C" fn kernel_repeat(argv: *mut CobValue, argc: c_int) -> CobValue {
    if !arity(MOD, "kernel_repeat", argc, 2) {
        return cob_empty_str();
    }
    let s = arg_str(MOD, "kernel_repeat", argv, argc, 0);
    let times = arg_int(MOD, "kernel_repeat", argv, argc, 1);
    let n = s.len();
    if times <= 0 || n == 0 {
        return REPEAT.ret(b"");
    }
    if times as u64 > (KERNEL_MAX_RESULT / n) as u64 {
        ewrite("[kernels] warning: kernel_repeat() result would be too large; gives \"\"\n");
        return REPEAT.ret(b"");
    }
    REPEAT.ret(&s.repeat(times as usize))
}

#[no_mangle]
pub unsafe extern "C" fn kernel_trim(argv: *mut CobValue, argc: c_int) -> CobValue {
    if !arity(MOD, "kernel_trim", argc, 1) {
        return cob_empty_str();
    }
    let mut s = arg_str(MOD, "kernel_trim", argv, argc, 0);
    while let Some((&last, head)) = s.split_last() {
        if is_space(last) {
            s = head;
        } else {
            break;
        }
    }
    while let Some((&first, tail)) = s.split_first() {
        if is_space(first) {
            s = tail;
        } else {
            break;
        }
    }
    TRIM.ret(s)
}

#[no_mangle]
pub unsafe extern "C" fn kernel_replace(argv: *mut CobValue, argc: c_int) -> CobValue {
    if !arity(MOD, "kernel_replace", argc, 3) {
        return cob_empty_str();
    }
    let s = arg_str(MOD, "kernel_replace", argv, argc, 0);
    let from = arg_str(MOD, "kernel_replace", argv, argc, 1);
    let to = arg_str(MOD, "kernel_replace", argv, argc, 2);
    let (sl, fl, tl) = (s.len(), from.len(), to.len());
    if fl == 0 {
        return REPLACE.ret(s);
    }
    let mut count: usize = 0;
    let mut p = 0usize;
    while let Some(i) = find(&s[p..], from) {
        count += 1;
        p += i + fl;
    }
    let outn = sl.wrapping_add(count.wrapping_mul(tl)).wrapping_sub(count.wrapping_mul(fl));
    if outn > KERNEL_MAX_RESULT {
        ewrite("[kernels] warning: kernel_replace() result would be too large; gives \"\"\n");
        return REPLACE.ret(b"");
    }
    let mut out = Vec::with_capacity(outn);
    let mut p = 0usize;
    loop {
        match find(&s[p..], from) {
            None => {
                out.extend_from_slice(&s[p..]);
                break;
            }
            Some(i) => {
                out.extend_from_slice(&s[p..p + i]);
                out.extend_from_slice(to);
                p += i + fl;
            }
        }
    }
    REPLACE.ret(&out)
}

#[no_mangle]
pub unsafe extern "C" fn kernel_char(argv: *mut CobValue, argc: c_int) -> CobValue {
    if !arity(MOD, "kernel_char", argc, 2) {
        return cob_empty_str();
    }
    let s = arg_str(MOD, "kernel_char", argv, argc, 0);
    let i = arg_int(MOD, "kernel_char", argv, argc, 1);
    if i < 0 || i as u64 >= s.len() as u64 {
        return CHAR.ret(b"");
    }
    CHAR.ret(&s[i as usize..i as usize + 1])
}

#[no_mangle]
pub unsafe extern "C" fn kernel_code(argv: *mut CobValue, argc: c_int) -> CobValue {
    if !arity(MOD, "kernel_code", argc, 1) {
        return cob_int(0);
    }
    let s = arg_str(MOD, "kernel_code", argv, argc, 0);
    cob_int(s.first().copied().unwrap_or(0) as i64)
}

#[no_mangle]
pub unsafe extern "C" fn kernel_from_code(argv: *mut CobValue, argc: c_int) -> CobValue {
    if !arity(MOD, "kernel_from_code", argc, 1) {
        return cob_empty_str();
    }
    let n = arg_int(MOD, "kernel_from_code", argv, argc, 0);
    if !(1..=255).contains(&n) {
        return CHR.ret(b"");
    }
    CHR.ret(&[n as u8])
}

#[no_mangle]
pub unsafe extern "C" fn kernel_to_int(argv: *mut CobValue, argc: c_int) -> CobValue {
    if !arity(MOD, "kernel_to_int", argc, 1) {
        return cob_int(0);
    }
    let s = arg_str(MOD, "kernel_to_int", argv, argc, 0);
    cob_int(parse_whole_number(s).unwrap_or(0))
}

#[no_mangle]
pub unsafe extern "C" fn kernel_starts(argv: *mut CobValue, argc: c_int) -> CobValue {
    if !arity(MOD, "kernel_starts", argc, 2) {
        return cob_int(0);
    }
    let s = arg_str(MOD, "kernel_starts", argv, argc, 0);
    let p = arg_str(MOD, "kernel_starts", argv, argc, 1);
    cob_int(s.starts_with(p) as i64)
}

#[no_mangle]
pub unsafe extern "C" fn kernel_ends(argv: *mut CobValue, argc: c_int) -> CobValue {
    if !arity(MOD, "kernel_ends", argc, 2) {
        return cob_int(0);
    }
    let s = arg_str(MOD, "kernel_ends", argv, argc, 0);
    let p = arg_str(MOD, "kernel_ends", argv, argc, 1);
    cob_int(s.ends_with(p) as i64)
}

/// Every function this module exports, by name -- for builds that cannot
/// `dlopen()` (the browser playground) and bind the module in statically.
pub static EXPORTS: &[(&str, cob_abi::CobFFIFunc)] = &[
    ("kernel_len", kernel_len),
    ("kernel_upper", kernel_upper),
    ("kernel_lower", kernel_lower),
    ("kernel_slice", kernel_slice),
    ("kernel_find", kernel_find),
    ("kernel_repeat", kernel_repeat),
    ("kernel_trim", kernel_trim),
    ("kernel_replace", kernel_replace),
    ("kernel_char", kernel_char),
    ("kernel_code", kernel_code),
    ("kernel_from_code", kernel_from_code),
    ("kernel_to_int", kernel_to_int),
    ("kernel_starts", kernel_starts),
    ("kernel_ends", kernel_ends),
];

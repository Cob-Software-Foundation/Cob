//! cornmath -- numbers beyond + - * /          (shuck cornmath)
//!
//!   husk_abs(n)            absolute value
//!   ear_max(a, b)          the larger of two numbers
//!   ear_min(a, b)          the smaller of two numbers
//!   stalk_pow(base, exp)   base to a whole-number power (exp < 0 gives 0)
//!   stalk_sqrt(n)          whole-number square root, rounded down
//!   silk_mod(a, b)         remainder, always >= 0 for a positive b
//!   scatter_seed(n)        seed the random numbers (repeatable runs)
//!   scatter_rand(lo, hi)   a random whole number from lo to hi, inclusive
//!
//! scatter_rand uses its own generator (splitmix64), so a seeded run gives
//! the same numbers on every platform.

use std::os::raw::c_int;
use std::sync::Mutex;

use cob_abi::*;

const MOD: &str = "cornmath";

#[no_mangle]
pub unsafe extern "C" fn husk_abs(argv: *mut CobValue, argc: c_int) -> CobValue {
    if !arity(MOD, "husk_abs", argc, 1) {
        return cob_int(0);
    }
    let n = arg_int(MOD, "husk_abs", argv, argc, 0);
    cob_int(if n < 0 { n.wrapping_neg() } else { n })
}

#[no_mangle]
pub unsafe extern "C" fn ear_max(argv: *mut CobValue, argc: c_int) -> CobValue {
    if !arity(MOD, "ear_max", argc, 2) {
        return cob_int(0);
    }
    let a = arg_int(MOD, "ear_max", argv, argc, 0);
    let b = arg_int(MOD, "ear_max", argv, argc, 1);
    cob_int(if a > b { a } else { b })
}

#[no_mangle]
pub unsafe extern "C" fn ear_min(argv: *mut CobValue, argc: c_int) -> CobValue {
    if !arity(MOD, "ear_min", argc, 2) {
        return cob_int(0);
    }
    let a = arg_int(MOD, "ear_min", argv, argc, 0);
    let b = arg_int(MOD, "ear_min", argv, argc, 1);
    cob_int(if a < b { a } else { b })
}

#[no_mangle]
pub unsafe extern "C" fn stalk_pow(argv: *mut CobValue, argc: c_int) -> CobValue {
    if !arity(MOD, "stalk_pow", argc, 2) {
        return cob_int(0);
    }
    let base = arg_int(MOD, "stalk_pow", argv, argc, 0);
    let mut exp = arg_int(MOD, "stalk_pow", argv, argc, 1);
    if exp < 0 {
        ewrite("[cornmath] warning: stalk_pow() with a negative exponent gives 0\n");
        return cob_int(0);
    }
    let mut result: u64 = 1;
    let mut b = base as u64;
    while exp > 0 {
        if exp & 1 == 1 {
            result = result.wrapping_mul(b);
        }
        b = b.wrapping_mul(b);
        exp >>= 1;
    }
    cob_int(result as i64)
}

#[no_mangle]
pub unsafe extern "C" fn stalk_sqrt(argv: *mut CobValue, argc: c_int) -> CobValue {
    if !arity(MOD, "stalk_sqrt", argc, 1) {
        return cob_int(0);
    }
    let n = arg_int(MOD, "stalk_sqrt", argv, argc, 0);
    if n < 0 {
        ewrite("[cornmath] warning: stalk_sqrt() of a negative number gives 0\n");
        return cob_int(0);
    }
    let mut lo: i64 = 0;
    let mut hi: i64 = n.min(3037000499); // floor(sqrt(2^63))
    while lo < hi {
        let mid = lo + (hi - lo + 1) / 2;
        if mid <= n / mid {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    cob_int(lo)
}

#[no_mangle]
pub unsafe extern "C" fn silk_mod(argv: *mut CobValue, argc: c_int) -> CobValue {
    if !arity(MOD, "silk_mod", argc, 2) {
        return cob_int(0);
    }
    let a = arg_int(MOD, "silk_mod", argv, argc, 0);
    let b = arg_int(MOD, "silk_mod", argv, argc, 1);
    if b == 0 {
        ewrite("[cornmath] warning: silk_mod() by zero gives 0\n");
        return cob_int(0);
    }
    if b == -1 {
        return cob_int(0);
    }
    let mut m = a % b;
    if m != 0 && ((m < 0) != (b < 0)) {
        m += b;
    }
    cob_int(m)
}

/// (state, seeded)
static SCATTER: Mutex<(u64, bool)> = Mutex::new((0, false));

fn scatter_next() -> u64 {
    let mut g = SCATTER.lock().unwrap_or_else(|e| e.into_inner());
    if !g.1 {
        #[cfg(target_arch = "wasm32")]
        let now = cob_abi::host::now_secs() as u64;
        #[cfg(not(target_arch = "wasm32"))]
        let now = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_secs())
            .unwrap_or(0);
        g.0 = now.wrapping_mul(2654435761);
        g.1 = true;
    }
    g.0 = g.0.wrapping_add(0x9E3779B97F4A7C15);
    let mut z = g.0;
    z = (z ^ (z >> 30)).wrapping_mul(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)).wrapping_mul(0x94D049BB133111EB);
    z ^ (z >> 31)
}

#[no_mangle]
pub unsafe extern "C" fn scatter_seed(argv: *mut CobValue, argc: c_int) -> CobValue {
    if !arity(MOD, "scatter_seed", argc, 1) {
        return cob_int(0);
    }
    let seed = arg_int(MOD, "scatter_seed", argv, argc, 0);
    let mut g = SCATTER.lock().unwrap_or_else(|e| e.into_inner());
    *g = (seed as u64, true);
    cob_int(0)
}

#[no_mangle]
pub unsafe extern "C" fn scatter_rand(argv: *mut CobValue, argc: c_int) -> CobValue {
    if !arity(MOD, "scatter_rand", argc, 2) {
        return cob_int(0);
    }
    let mut lo = arg_int(MOD, "scatter_rand", argv, argc, 0);
    let mut hi = arg_int(MOD, "scatter_rand", argv, argc, 1);
    if hi < lo {
        std::mem::swap(&mut lo, &mut hi);
    }
    let span = (hi as u64).wrapping_sub(lo as u64).wrapping_add(1);
    if span == 0 {
        return cob_int(scatter_next() as i64);
    }
    cob_int((lo as u64).wrapping_add(scatter_next() % span) as i64)
}

/// Every function this module exports, by name -- for builds that cannot
/// `dlopen()` (the browser playground) and bind the module in statically.
pub static EXPORTS: &[(&str, cob_abi::CobFFIFunc)] = &[
    ("husk_abs", husk_abs),
    ("ear_max", ear_max),
    ("ear_min", ear_min),
    ("stalk_pow", stalk_pow),
    ("stalk_sqrt", stalk_sqrt),
    ("silk_mod", silk_mod),
    ("scatter_seed", scatter_seed),
    ("scatter_rand", scatter_rand),
];

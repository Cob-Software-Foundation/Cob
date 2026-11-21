//! furrow -- arrays: a fixed-length row, planted once        (shuck furrow)
//!
//! An array has a length chosen when it is made and never changes; every slot
//! starts as the number 0 and holds a number or a string. It is used through a
//! handle (a whole number, like `harvest()` and `pantry_open()` give you).
//! Positions count from 0. A bad handle or an out-of-range position prints a
//! warning and gives 0 / does nothing -- it never stops the program.
//! (For an array that grows, see `bushel`.)
//!
//!   furrow_new(n)             a new array of n slots, all 0 (its handle is 1 or more;
//!                             0 if n is negative or too big)
//!   furrow_free(f)            throw the array away; 0 on success, 1 on failure
//!   furrow_len(f)             how many slots it has
//!   furrow_get(f, i)          the value in slot i (0 if there is no such slot)
//!   furrow_set(f, i, v)       put v in slot i; 0 on success, 1 on failure
//!   furrow_fill(f, v)         put v in every slot
//!   furrow_swap(f, i, j)      exchange the values in slots i and j
//!   furrow_find(f, v)         the first slot holding v, or -1
//!   furrow_sum(f)             the sum of the numbers in it (strings are skipped)
//!   furrow_sort(f)            numbers first (smallest first), then strings (by bytes)
//!   furrow_copy(f)            a new array with the same values
//!
//! A number never equals a string: `furrow_find` of 3 will not match "3".

use std::cmp::Ordering;
use std::ffi::CStr;
use std::os::raw::c_int;
use std::sync::{Mutex, MutexGuard};

use cob_abi::*;

const MOD: &str = "furrow";
/// Most slots one array may have (a typo like furrow_new(1000000000000) must
/// not eat all memory).
const FURROW_MAX_SLOTS: i64 = 10_000_000;

#[derive(Clone, Debug, PartialEq)]
enum Elem {
    Int(i64),
    Str(Vec<u8>),
}

/// Every array ever made, 1-based by handle; `None` once freed. Handles are
/// never reused, so a stale one can never reach somebody else's array.
static ARRAYS: Mutex<Vec<Option<Vec<Elem>>>> = Mutex::new(Vec::new());

static GET: Slot = Slot::new();

fn arrays() -> MutexGuard<'static, Vec<Option<Vec<Elem>>>> {
    ARRAYS.lock().unwrap_or_else(|e| e.into_inner())
}

fn alloc(items: Vec<Elem>) -> i64 {
    let mut g = arrays();
    g.push(Some(items));
    g.len() as i64
}

/// Runs `f` on the array behind handle `h`. A handle that was never issued, or
/// was freed, warns and gives `None`.
fn with_array<R>(func: &str, h: i64, f: impl FnOnce(&mut Vec<Elem>) -> R) -> Option<R> {
    let mut g = arrays();
    let slot = usize::try_from(h)
        .ok()
        .and_then(|n| n.checked_sub(1))
        .and_then(|n| g.get_mut(n))
        .and_then(|s| s.as_mut());
    match slot {
        Some(a) => Some(f(a)),
        None => {
            ewrite(&format!("[{}] warning: {}() -- array {} is invalid or already freed\n", MOD, func, h));
            None
        }
    }
}

/// `i` as a slot number of an array with `len` slots.
fn index(i: i64, len: usize) -> Option<usize> {
    if i >= 0 && (i as u64) < len as u64 {
        Some(i as usize)
    } else {
        None
    }
}

fn warn_range(func: &str, i: i64, len: usize) {
    ewrite(&format!(
        "[{}] warning: {}() -- slot {} is out of range (the array has {} slot{})\n",
        MOD,
        func,
        i,
        len,
        if len == 1 { "" } else { "s" }
    ));
}

/// The i-th argument as a slot value: a string stays a string, anything else is a number.
///
/// # Safety
/// `i < argc`, and `argv` points to `argc` valid values.
unsafe fn arg_elem(argv: *mut CobValue, i: c_int) -> Elem {
    let v = *argv.add(i as usize);
    if v.kind == COB_STR {
        if v.as_.s.is_null() {
            Elem::Str(Vec::new())
        } else {
            Elem::Str(CStr::from_ptr(v.as_.s).to_bytes().to_vec())
        }
    } else {
        Elem::Int(v.as_.i as i64)
    }
}

fn ret_elem(e: &Elem) -> CobValue {
    match e {
        Elem::Int(n) => cob_int(*n),
        Elem::Str(s) => GET.ret(s),
    }
}

fn elem_cmp(a: &Elem, b: &Elem) -> Ordering {
    match (a, b) {
        (Elem::Int(x), Elem::Int(y)) => x.cmp(y),
        (Elem::Int(_), Elem::Str(_)) => Ordering::Less,
        (Elem::Str(_), Elem::Int(_)) => Ordering::Greater,
        (Elem::Str(x), Elem::Str(y)) => x.cmp(y),
    }
}

// ---- the functions ----------------------------------------------------------

#[no_mangle]
pub unsafe extern "C" fn furrow_new(argv: *mut CobValue, argc: c_int) -> CobValue {
    if !arity(MOD, "furrow_new", argc, 1) {
        return cob_int(0);
    }
    let n = arg_int(MOD, "furrow_new", argv, argc, 0);
    if !(0..=FURROW_MAX_SLOTS).contains(&n) {
        ewrite(&format!(
            "[{}] warning: furrow_new({}) -- the length must be between 0 and {}\n",
            MOD, n, FURROW_MAX_SLOTS
        ));
        return cob_int(0);
    }
    let mut items: Vec<Elem> = Vec::new();
    if items.try_reserve_exact(n as usize).is_err() {
        ewrite(&format!("[{}] warning: furrow_new({}) failed (out of memory)\n", MOD, n));
        return cob_int(0);
    }
    items.resize(n as usize, Elem::Int(0));
    cob_int(alloc(items))
}

#[no_mangle]
pub unsafe extern "C" fn furrow_free(argv: *mut CobValue, argc: c_int) -> CobValue {
    if !arity(MOD, "furrow_free", argc, 1) {
        return cob_int(1);
    }
    let h = arg_int(MOD, "furrow_free", argv, argc, 0);
    // validate through with_array so a bad handle gets the usual warning
    let Some(()) = with_array("furrow_free", h, |_| ()) else { return cob_int(1) };
    let mut g = arrays();
    g[h as usize - 1] = None;
    cob_int(0)
}

#[no_mangle]
pub unsafe extern "C" fn furrow_len(argv: *mut CobValue, argc: c_int) -> CobValue {
    if !arity(MOD, "furrow_len", argc, 1) {
        return cob_int(0);
    }
    let h = arg_int(MOD, "furrow_len", argv, argc, 0);
    cob_int(with_array("furrow_len", h, |a| a.len() as i64).unwrap_or(0))
}

#[no_mangle]
pub unsafe extern "C" fn furrow_get(argv: *mut CobValue, argc: c_int) -> CobValue {
    if !arity(MOD, "furrow_get", argc, 2) {
        return cob_int(0);
    }
    let h = arg_int(MOD, "furrow_get", argv, argc, 0);
    let i = arg_int(MOD, "furrow_get", argv, argc, 1);
    match with_array("furrow_get", h, |a| index(i, a.len()).map(|k| a[k].clone()).ok_or(a.len())) {
        Some(Ok(e)) => ret_elem(&e),
        Some(Err(len)) => {
            warn_range("furrow_get", i, len);
            cob_int(0)
        }
        None => cob_int(0),
    }
}

#[no_mangle]
pub unsafe extern "C" fn furrow_set(argv: *mut CobValue, argc: c_int) -> CobValue {
    if !arity(MOD, "furrow_set", argc, 3) {
        return cob_int(1);
    }
    let h = arg_int(MOD, "furrow_set", argv, argc, 0);
    let i = arg_int(MOD, "furrow_set", argv, argc, 1);
    let v = arg_elem(argv, 2);
    match with_array("furrow_set", h, |a| match index(i, a.len()) {
        Some(k) => {
            a[k] = v;
            Ok(())
        }
        None => Err(a.len()),
    }) {
        Some(Ok(())) => cob_int(0),
        Some(Err(len)) => {
            warn_range("furrow_set", i, len);
            cob_int(1)
        }
        None => cob_int(1),
    }
}

#[no_mangle]
pub unsafe extern "C" fn furrow_fill(argv: *mut CobValue, argc: c_int) -> CobValue {
    if !arity(MOD, "furrow_fill", argc, 2) {
        return cob_int(1);
    }
    let h = arg_int(MOD, "furrow_fill", argv, argc, 0);
    let v = arg_elem(argv, 1);
    cob_int(
        with_array("furrow_fill", h, |a| {
            for slot in a.iter_mut() {
                *slot = v.clone();
            }
        })
        .map_or(1, |_| 0),
    )
}

#[no_mangle]
pub unsafe extern "C" fn furrow_swap(argv: *mut CobValue, argc: c_int) -> CobValue {
    if !arity(MOD, "furrow_swap", argc, 3) {
        return cob_int(1);
    }
    let h = arg_int(MOD, "furrow_swap", argv, argc, 0);
    let i = arg_int(MOD, "furrow_swap", argv, argc, 1);
    let j = arg_int(MOD, "furrow_swap", argv, argc, 2);
    match with_array("furrow_swap", h, |a| match (index(i, a.len()), index(j, a.len())) {
        (Some(x), Some(y)) => {
            a.swap(x, y);
            Ok(())
        }
        (None, _) => Err((i, a.len())),
        (_, None) => Err((j, a.len())),
    }) {
        Some(Ok(())) => cob_int(0),
        Some(Err((bad, len))) => {
            warn_range("furrow_swap", bad, len);
            cob_int(1)
        }
        None => cob_int(1),
    }
}

#[no_mangle]
pub unsafe extern "C" fn furrow_find(argv: *mut CobValue, argc: c_int) -> CobValue {
    if !arity(MOD, "furrow_find", argc, 2) {
        return cob_int(-1);
    }
    let h = arg_int(MOD, "furrow_find", argv, argc, 0);
    let v = arg_elem(argv, 1);
    let found = with_array("furrow_find", h, |a| a.iter().position(|e| *e == v));
    cob_int(found.flatten().map_or(-1, |p| p as i64))
}

#[no_mangle]
pub unsafe extern "C" fn furrow_sum(argv: *mut CobValue, argc: c_int) -> CobValue {
    if !arity(MOD, "furrow_sum", argc, 1) {
        return cob_int(0);
    }
    let h = arg_int(MOD, "furrow_sum", argv, argc, 0);
    // wrapping, like every Cob number
    let total = with_array("furrow_sum", h, |a| {
        a.iter().fold(0i64, |acc, e| match e {
            Elem::Int(n) => acc.wrapping_add(*n),
            Elem::Str(_) => acc,
        })
    });
    cob_int(total.unwrap_or(0))
}

#[no_mangle]
pub unsafe extern "C" fn furrow_sort(argv: *mut CobValue, argc: c_int) -> CobValue {
    if !arity(MOD, "furrow_sort", argc, 1) {
        return cob_int(1);
    }
    let h = arg_int(MOD, "furrow_sort", argv, argc, 0);
    cob_int(with_array("furrow_sort", h, |a| a.sort_by(elem_cmp)).map_or(1, |_| 0))
}

#[no_mangle]
pub unsafe extern "C" fn furrow_copy(argv: *mut CobValue, argc: c_int) -> CobValue {
    if !arity(MOD, "furrow_copy", argc, 1) {
        return cob_int(0);
    }
    let h = arg_int(MOD, "furrow_copy", argv, argc, 0);
    match with_array("furrow_copy", h, |a| a.clone()) {
        Some(items) => cob_int(alloc(items)),
        None => cob_int(0),
    }
}

/// The same functions as a table, for builds that cannot `dlopen` (the browser).
pub static EXPORTS: &[(&str, cob_abi::CobFFIFunc)] = &[
    ("furrow_new", furrow_new),
    ("furrow_free", furrow_free),
    ("furrow_len", furrow_len),
    ("furrow_get", furrow_get),
    ("furrow_set", furrow_set),
    ("furrow_fill", furrow_fill),
    ("furrow_swap", furrow_swap),
    ("furrow_find", furrow_find),
    ("furrow_sum", furrow_sum),
    ("furrow_sort", furrow_sort),
    ("furrow_copy", furrow_copy),
];

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn index_bounds() {
        assert_eq!(index(0, 3), Some(0));
        assert_eq!(index(2, 3), Some(2));
        assert_eq!(index(3, 3), None);
        assert_eq!(index(-1, 3), None);
        assert_eq!(index(0, 0), None);
        assert_eq!(index(i64::MAX, 3), None);
    }

    #[test]
    fn ordering_puts_numbers_before_strings() {
        let mut v = vec![
            Elem::Str(b"b".to_vec()),
            Elem::Int(10),
            Elem::Str(b"a".to_vec()),
            Elem::Int(-4),
        ];
        v.sort_by(elem_cmp);
        assert_eq!(
            v,
            vec![Elem::Int(-4), Elem::Int(10), Elem::Str(b"a".to_vec()), Elem::Str(b"b".to_vec())]
        );
    }

    #[test]
    fn a_number_never_equals_a_string() {
        assert_ne!(Elem::Int(3), Elem::Str(b"3".to_vec()));
    }
}

//! bushel -- lists that grow, a bushel of whatever you pick     (shuck bushel)
//!
//! A list holds numbers and strings, mixed freely, and is used through a
//! handle (a whole number, like `harvest()` and `pantry_open()` give you).
//! Positions count from 0. A bad handle or an out-of-range position prints a
//! warning and gives 0 / does nothing -- it never stops the program.
//!
//!   bushel_new()              a new, empty list (its handle is 1 or more)
//!   bushel_free(b)            throw the list away; 0 on success, 1 on failure
//!   bushel_len(b)             how many items it holds
//!   bushel_push(b, v)         add v at the end; 0 on success, 1 on failure
//!   bushel_get(b, i)          the item at position i (0 if there is none)
//!   bushel_set(b, i, v)       replace the item at i; 0 on success, 1 on failure
//!   bushel_insert(b, i, v)    put v at i, moving later items up (i may equal len)
//!   bushel_remove(b, i)       take out the item at i and hand it back
//!   bushel_clear(b)           remove every item
//!   bushel_find(b, v)         position of the first item equal to v, or -1
//!   bushel_sort(b)            numbers first (smallest first), then strings (by bytes)
//!   bushel_reverse(b)         turn the list around
//!   bushel_extend(b, other)   add a copy of every item of `other` to the end of b
//!   bushel_copy(b)            a new list with the same items
//!   bushel_join(b, sep)       every item as text, with sep between them
//!   bushel_split(text, sep)   a new list of the pieces of text (sep "" = one byte each)
//!
//! Python-style access (the list syntax of Cob 0.1.0 is built on these four):
//!   bushel_at(b, i)           like bushel_get, but a negative i counts from the end (-1 = last)
//!   bushel_put(b, i, v)       like bushel_set, with the same negative positions
//!   bushel_with(b, v)         add v at the end and hand the list back, so calls can be chained
//!   bushel_range(n)           a new list holding 0, 1, ... n-1
//!   bushel_str(b)             the list as text, like Python: [1, "two", 3]
//!
//! A number never equals a string: `bushel_find` of 3 will not match "3".

use std::cmp::Ordering;
use std::ffi::CStr;
use std::os::raw::c_int;
use std::sync::{Mutex, MutexGuard};

use cob_abi::*;

const MOD: &str = "bushel";
/// Most items one list may hold (a runaway loop must not eat all memory).
const BUSHEL_MAX_ITEMS: usize = 10_000_000;
/// Longest string `bushel_join` will build.
const BUSHEL_MAX_RESULT: usize = 64 * 1024 * 1024;

#[derive(Clone, Debug, PartialEq)]
enum Elem {
    Int(i64),
    Str(Vec<u8>),
}

/// Every list ever made, 1-based by handle; `None` once freed. Handles are
/// never reused, so a stale one can never reach somebody else's list.
static LISTS: Mutex<Vec<Option<Vec<Elem>>>> = Mutex::new(Vec::new());

static GET: Slot = Slot::new();
static REMOVE: Slot = Slot::new();
static JOIN: Slot = Slot::new();
static STR: Slot = Slot::new();

fn lists() -> MutexGuard<'static, Vec<Option<Vec<Elem>>>> {
    LISTS.lock().unwrap_or_else(|e| e.into_inner())
}

fn alloc(items: Vec<Elem>) -> i64 {
    let mut g = lists();
    g.push(Some(items));
    g.len() as i64
}

/// Runs `f` on the list behind handle `h`. A handle that was never issued, or
/// was freed, warns and gives `None`.
fn with_list<R>(func: &str, h: i64, f: impl FnOnce(&mut Vec<Elem>) -> R) -> Option<R> {
    let mut g = lists();
    let slot = usize::try_from(h)
        .ok()
        .and_then(|n| n.checked_sub(1))
        .and_then(|n| g.get_mut(n))
        .and_then(|s| s.as_mut());
    match slot {
        Some(l) => Some(f(l)),
        None => {
            ewrite(&format!("[{}] warning: {}() -- list {} is invalid or already freed\n", MOD, func, h));
            None
        }
    }
}

/// `i` as a position inside a list of `len` items.
fn index(i: i64, len: usize) -> Option<usize> {
    if i >= 0 && (i as u64) < len as u64 {
        Some(i as usize)
    } else {
        None
    }
}

/// `i` as a position inside a list of `len` items, Python style: a negative `i` counts from the end.
fn pyindex(i: i64, len: usize) -> Option<usize> {
    let k = if i < 0 { i.checked_add(len as i64)? } else { i };
    index(k, len)
}

/// The list as Python would show it: `[1, "two", 3]`. Strings are quoted and escaped the way a Cob string literal is.
fn repr(items: &[Elem]) -> Vec<u8> {
    let mut out = vec![b'['];
    for (k, e) in items.iter().enumerate() {
        if k > 0 {
            out.extend_from_slice(b", ");
        }
        match e {
            Elem::Int(n) => out.extend_from_slice(n.to_string().as_bytes()),
            Elem::Str(s) => {
                out.push(b'"');
                for &c in s {
                    match c {
                        b'"' => out.extend_from_slice(b"\\\""),
                        b'\\' => out.extend_from_slice(b"\\\\"),
                        b'\n' => out.extend_from_slice(b"\\n"),
                        b'\t' => out.extend_from_slice(b"\\t"),
                        _ => out.push(c),
                    }
                }
                out.push(b'"');
            }
        }
    }
    out.push(b']');
    out
}

fn warn_range(func: &str, i: i64, len: usize) {
    ewrite(&format!(
        "[{}] warning: {}() -- position {} is out of range (the list has {} item{})\n",
        MOD,
        func,
        i,
        len,
        if len == 1 { "" } else { "s" }
    ));
}

fn warn_full(func: &str) {
    ewrite(&format!("[{}] warning: {}() -- a list cannot hold more than {} items\n", MOD, func, BUSHEL_MAX_ITEMS));
}

/// The i-th argument as a list item: a string stays a string, anything else is a number.
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

fn ret_elem(e: &Elem, slot: &Slot) -> CobValue {
    match e {
        Elem::Int(n) => cob_int(*n),
        Elem::Str(s) => slot.ret(s),
    }
}

fn elem_text(e: &Elem) -> Vec<u8> {
    match e {
        Elem::Int(n) => n.to_string().into_bytes(),
        Elem::Str(s) => s.clone(),
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

/// `text` cut at every `sep`, empty pieces kept. An empty `sep` cuts between
/// every byte. Stops early (returning what it has, one past the limit) so a
/// huge input cannot build an unbounded list.
fn split_bytes(text: &[u8], sep: &[u8]) -> Vec<Vec<u8>> {
    let mut out: Vec<Vec<u8>> = Vec::new();
    if sep.is_empty() {
        for &b in text.iter().take(BUSHEL_MAX_ITEMS + 1) {
            out.push(vec![b]);
        }
        return out;
    }
    let mut pos = 0usize;
    while let Some(off) = text[pos..].windows(sep.len()).position(|w| w == sep) {
        out.push(text[pos..pos + off].to_vec());
        pos += off + sep.len();
        if out.len() > BUSHEL_MAX_ITEMS {
            return out;
        }
    }
    out.push(text[pos..].to_vec());
    out
}

// ---- the functions ----------------------------------------------------------

#[no_mangle]
pub unsafe extern "C" fn bushel_new(_argv: *mut CobValue, argc: c_int) -> CobValue {
    if !arity(MOD, "bushel_new", argc, 0) {
        return cob_int(0);
    }
    cob_int(alloc(Vec::new()))
}

#[no_mangle]
pub unsafe extern "C" fn bushel_free(argv: *mut CobValue, argc: c_int) -> CobValue {
    if !arity(MOD, "bushel_free", argc, 1) {
        return cob_int(1);
    }
    let h = arg_int(MOD, "bushel_free", argv, argc, 0);
    // validate through with_list so a bad handle gets the usual warning
    let Some(()) = with_list("bushel_free", h, |_| ()) else { return cob_int(1) };
    let mut g = lists();
    g[h as usize - 1] = None;
    cob_int(0)
}

#[no_mangle]
pub unsafe extern "C" fn bushel_len(argv: *mut CobValue, argc: c_int) -> CobValue {
    if !arity(MOD, "bushel_len", argc, 1) {
        return cob_int(0);
    }
    let h = arg_int(MOD, "bushel_len", argv, argc, 0);
    cob_int(with_list("bushel_len", h, |l| l.len() as i64).unwrap_or(0))
}

#[no_mangle]
pub unsafe extern "C" fn bushel_push(argv: *mut CobValue, argc: c_int) -> CobValue {
    if !arity(MOD, "bushel_push", argc, 2) {
        return cob_int(1);
    }
    let h = arg_int(MOD, "bushel_push", argv, argc, 0);
    let v = arg_elem(argv, 1);
    match with_list("bushel_push", h, |l| {
        if l.len() >= BUSHEL_MAX_ITEMS {
            return false;
        }
        l.push(v);
        true
    }) {
        Some(true) => cob_int(0),
        Some(false) => {
            warn_full("bushel_push");
            cob_int(1)
        }
        None => cob_int(1),
    }
}

#[no_mangle]
pub unsafe extern "C" fn bushel_get(argv: *mut CobValue, argc: c_int) -> CobValue {
    if !arity(MOD, "bushel_get", argc, 2) {
        return cob_int(0);
    }
    let h = arg_int(MOD, "bushel_get", argv, argc, 0);
    let i = arg_int(MOD, "bushel_get", argv, argc, 1);
    match with_list("bushel_get", h, |l| index(i, l.len()).map(|k| l[k].clone()).ok_or(l.len())) {
        Some(Ok(e)) => ret_elem(&e, &GET),
        Some(Err(len)) => {
            warn_range("bushel_get", i, len);
            cob_int(0)
        }
        None => cob_int(0),
    }
}

#[no_mangle]
pub unsafe extern "C" fn bushel_set(argv: *mut CobValue, argc: c_int) -> CobValue {
    if !arity(MOD, "bushel_set", argc, 3) {
        return cob_int(1);
    }
    let h = arg_int(MOD, "bushel_set", argv, argc, 0);
    let i = arg_int(MOD, "bushel_set", argv, argc, 1);
    let v = arg_elem(argv, 2);
    match with_list("bushel_set", h, |l| match index(i, l.len()) {
        Some(k) => {
            l[k] = v;
            Ok(())
        }
        None => Err(l.len()),
    }) {
        Some(Ok(())) => cob_int(0),
        Some(Err(len)) => {
            warn_range("bushel_set", i, len);
            cob_int(1)
        }
        None => cob_int(1),
    }
}

#[no_mangle]
pub unsafe extern "C" fn bushel_insert(argv: *mut CobValue, argc: c_int) -> CobValue {
    if !arity(MOD, "bushel_insert", argc, 3) {
        return cob_int(1);
    }
    let h = arg_int(MOD, "bushel_insert", argv, argc, 0);
    let i = arg_int(MOD, "bushel_insert", argv, argc, 1);
    let v = arg_elem(argv, 2);
    enum Outcome {
        Done,
        Range(usize),
        Full,
    }
    match with_list("bushel_insert", h, |l| {
        if l.len() >= BUSHEL_MAX_ITEMS {
            return Outcome::Full;
        }
        // inserting at len is allowed: it is a push
        if i >= 0 && (i as u64) <= l.len() as u64 {
            l.insert(i as usize, v);
            Outcome::Done
        } else {
            Outcome::Range(l.len())
        }
    }) {
        Some(Outcome::Done) => cob_int(0),
        Some(Outcome::Range(len)) => {
            warn_range("bushel_insert", i, len);
            cob_int(1)
        }
        Some(Outcome::Full) => {
            warn_full("bushel_insert");
            cob_int(1)
        }
        None => cob_int(1),
    }
}

#[no_mangle]
pub unsafe extern "C" fn bushel_remove(argv: *mut CobValue, argc: c_int) -> CobValue {
    if !arity(MOD, "bushel_remove", argc, 2) {
        return cob_int(0);
    }
    let h = arg_int(MOD, "bushel_remove", argv, argc, 0);
    let i = arg_int(MOD, "bushel_remove", argv, argc, 1);
    match with_list("bushel_remove", h, |l| match index(i, l.len()) {
        Some(k) => Ok(l.remove(k)),
        None => Err(l.len()),
    }) {
        Some(Ok(e)) => ret_elem(&e, &REMOVE),
        Some(Err(len)) => {
            warn_range("bushel_remove", i, len);
            cob_int(0)
        }
        None => cob_int(0),
    }
}

#[no_mangle]
pub unsafe extern "C" fn bushel_clear(argv: *mut CobValue, argc: c_int) -> CobValue {
    if !arity(MOD, "bushel_clear", argc, 1) {
        return cob_int(1);
    }
    let h = arg_int(MOD, "bushel_clear", argv, argc, 0);
    cob_int(with_list("bushel_clear", h, |l| l.clear()).map_or(1, |_| 0))
}

#[no_mangle]
pub unsafe extern "C" fn bushel_find(argv: *mut CobValue, argc: c_int) -> CobValue {
    if !arity(MOD, "bushel_find", argc, 2) {
        return cob_int(-1);
    }
    let h = arg_int(MOD, "bushel_find", argv, argc, 0);
    let v = arg_elem(argv, 1);
    let found = with_list("bushel_find", h, |l| l.iter().position(|e| *e == v));
    cob_int(found.flatten().map_or(-1, |p| p as i64))
}

#[no_mangle]
pub unsafe extern "C" fn bushel_sort(argv: *mut CobValue, argc: c_int) -> CobValue {
    if !arity(MOD, "bushel_sort", argc, 1) {
        return cob_int(1);
    }
    let h = arg_int(MOD, "bushel_sort", argv, argc, 0);
    cob_int(with_list("bushel_sort", h, |l| l.sort_by(elem_cmp)).map_or(1, |_| 0))
}

#[no_mangle]
pub unsafe extern "C" fn bushel_reverse(argv: *mut CobValue, argc: c_int) -> CobValue {
    if !arity(MOD, "bushel_reverse", argc, 1) {
        return cob_int(1);
    }
    let h = arg_int(MOD, "bushel_reverse", argv, argc, 0);
    cob_int(with_list("bushel_reverse", h, |l| l.reverse()).map_or(1, |_| 0))
}

#[no_mangle]
pub unsafe extern "C" fn bushel_extend(argv: *mut CobValue, argc: c_int) -> CobValue {
    if !arity(MOD, "bushel_extend", argc, 2) {
        return cob_int(1);
    }
    let h = arg_int(MOD, "bushel_extend", argv, argc, 0);
    let other = arg_int(MOD, "bushel_extend", argv, argc, 1);
    // copy `other` first, so extending a list with itself is fine and only
    // one list is ever locked-and-borrowed at a time
    let Some(items) = with_list("bushel_extend", other, |l| l.clone()) else { return cob_int(1) };
    match with_list("bushel_extend", h, |l| {
        if l.len() + items.len() > BUSHEL_MAX_ITEMS {
            return false;
        }
        l.extend(items);
        true
    }) {
        Some(true) => cob_int(0),
        Some(false) => {
            warn_full("bushel_extend");
            cob_int(1)
        }
        None => cob_int(1),
    }
}

#[no_mangle]
pub unsafe extern "C" fn bushel_copy(argv: *mut CobValue, argc: c_int) -> CobValue {
    if !arity(MOD, "bushel_copy", argc, 1) {
        return cob_int(0);
    }
    let h = arg_int(MOD, "bushel_copy", argv, argc, 0);
    match with_list("bushel_copy", h, |l| l.clone()) {
        Some(items) => cob_int(alloc(items)),
        None => cob_int(0),
    }
}

#[no_mangle]
pub unsafe extern "C" fn bushel_join(argv: *mut CobValue, argc: c_int) -> CobValue {
    if !arity(MOD, "bushel_join", argc, 2) {
        return cob_empty_str();
    }
    let h = arg_int(MOD, "bushel_join", argv, argc, 0);
    let sep = arg_str(MOD, "bushel_join", argv, argc, 1);
    let built = with_list("bushel_join", h, |l| {
        let mut buf: Vec<u8> = Vec::new();
        for (k, e) in l.iter().enumerate() {
            if k > 0 {
                buf.extend_from_slice(sep);
            }
            buf.extend_from_slice(&elem_text(e));
            if buf.len() > BUSHEL_MAX_RESULT {
                return None;
            }
        }
        Some(buf)
    });
    match built {
        Some(Some(buf)) => JOIN.ret(&buf),
        Some(None) => {
            ewrite(&format!("[{}] warning: bushel_join() result is too long\n", MOD));
            JOIN.ret(b"")
        }
        None => JOIN.ret(b""),
    }
}

#[no_mangle]
pub unsafe extern "C" fn bushel_split(argv: *mut CobValue, argc: c_int) -> CobValue {
    if !arity(MOD, "bushel_split", argc, 2) {
        return cob_int(0);
    }
    let text = arg_str(MOD, "bushel_split", argv, argc, 0);
    let sep = arg_str(MOD, "bushel_split", argv, argc, 1);
    let pieces = split_bytes(text, sep);
    if pieces.len() > BUSHEL_MAX_ITEMS {
        warn_full("bushel_split");
        return cob_int(0);
    }
    cob_int(alloc(pieces.into_iter().map(Elem::Str).collect()))
}

#[no_mangle]
pub unsafe extern "C" fn bushel_at(argv: *mut CobValue, argc: c_int) -> CobValue {
    if !arity(MOD, "bushel_at", argc, 2) {
        return cob_int(0);
    }
    let h = arg_int(MOD, "bushel_at", argv, argc, 0);
    let i = arg_int(MOD, "bushel_at", argv, argc, 1);
    match with_list("bushel_at", h, |l| pyindex(i, l.len()).map(|k| l[k].clone()).ok_or(l.len())) {
        Some(Ok(e)) => ret_elem(&e, &GET),
        Some(Err(len)) => {
            warn_range("bushel_at", i, len);
            cob_int(0)
        }
        None => cob_int(0),
    }
}

#[no_mangle]
pub unsafe extern "C" fn bushel_put(argv: *mut CobValue, argc: c_int) -> CobValue {
    if !arity(MOD, "bushel_put", argc, 3) {
        return cob_int(1);
    }
    let h = arg_int(MOD, "bushel_put", argv, argc, 0);
    let i = arg_int(MOD, "bushel_put", argv, argc, 1);
    let v = arg_elem(argv, 2);
    match with_list("bushel_put", h, |l| match pyindex(i, l.len()) {
        Some(k) => {
            l[k] = v;
            Ok(())
        }
        None => Err(l.len()),
    }) {
        Some(Ok(())) => cob_int(0),
        Some(Err(len)) => {
            warn_range("bushel_put", i, len);
            cob_int(1)
        }
        None => cob_int(1),
    }
}

#[no_mangle]
pub unsafe extern "C" fn bushel_with(argv: *mut CobValue, argc: c_int) -> CobValue {
    if !arity(MOD, "bushel_with", argc, 2) {
        return cob_int(0);
    }
    let h = arg_int(MOD, "bushel_with", argv, argc, 0);
    let v = arg_elem(argv, 1);
    match with_list("bushel_with", h, |l| {
        if l.len() >= BUSHEL_MAX_ITEMS {
            return false;
        }
        l.push(v);
        true
    }) {
        Some(true) => cob_int(h),
        Some(false) => {
            warn_full("bushel_with");
            cob_int(h)
        }
        None => cob_int(0),
    }
}

#[no_mangle]
pub unsafe extern "C" fn bushel_range(argv: *mut CobValue, argc: c_int) -> CobValue {
    if !arity(MOD, "bushel_range", argc, 1) {
        return cob_int(0);
    }
    let n = arg_int(MOD, "bushel_range", argv, argc, 0);
    if n > BUSHEL_MAX_ITEMS as i64 {
        warn_full("bushel_range");
        return cob_int(0);
    }
    cob_int(alloc((0..n.max(0)).map(Elem::Int).collect()))
}

#[no_mangle]
pub unsafe extern "C" fn bushel_str(argv: *mut CobValue, argc: c_int) -> CobValue {
    if !arity(MOD, "bushel_str", argc, 1) {
        return STR.ret(b"");
    }
    let h = arg_int(MOD, "bushel_str", argv, argc, 0);
    let built = with_list("bushel_str", h, |l| {
        let mut size = 2usize;
        for e in l.iter() {
            size = size.saturating_add(elem_text(e).len() * 2 + 4);
            if size > BUSHEL_MAX_RESULT {
                return None;
            }
        }
        Some(repr(l))
    });
    match built {
        Some(Some(buf)) => STR.ret(&buf),
        Some(None) => {
            ewrite(&format!("[{}] warning: bushel_str() result is too long\n", MOD));
            STR.ret(b"")
        }
        None => STR.ret(b""),
    }
}

/// The same functions as a table, for builds that cannot `dlopen` (the browser).
pub static EXPORTS: &[(&str, cob_abi::CobFFIFunc)] = &[
    ("bushel_new", bushel_new),
    ("bushel_free", bushel_free),
    ("bushel_len", bushel_len),
    ("bushel_push", bushel_push),
    ("bushel_get", bushel_get),
    ("bushel_set", bushel_set),
    ("bushel_insert", bushel_insert),
    ("bushel_remove", bushel_remove),
    ("bushel_clear", bushel_clear),
    ("bushel_find", bushel_find),
    ("bushel_sort", bushel_sort),
    ("bushel_reverse", bushel_reverse),
    ("bushel_extend", bushel_extend),
    ("bushel_copy", bushel_copy),
    ("bushel_join", bushel_join),
    ("bushel_split", bushel_split),
    ("bushel_at", bushel_at),
    ("bushel_put", bushel_put),
    ("bushel_with", bushel_with),
    ("bushel_range", bushel_range),
    ("bushel_str", bushel_str),
];

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn python_positions() {
        assert_eq!(pyindex(0, 3), Some(0));
        assert_eq!(pyindex(-1, 3), Some(2));
        assert_eq!(pyindex(-3, 3), Some(0));
        assert_eq!(pyindex(-4, 3), None);
        assert_eq!(pyindex(3, 3), None);
        assert_eq!(pyindex(-1, 0), None);
        assert_eq!(pyindex(i64::MIN, 3), None);
    }

    #[test]
    fn repr_matches_python_shape() {
        assert_eq!(repr(&[]), b"[]".to_vec());
        assert_eq!(repr(&[Elem::Int(1), Elem::Str(b"two".to_vec()), Elem::Int(-3)]), b"[1, \"two\", -3]".to_vec());
        assert_eq!(repr(&[Elem::Str(b"a\"b\\c\nd".to_vec())]), b"[\"a\\\"b\\\\c\\nd\"]".to_vec());
    }

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
    fn split_cases() {
        let s = |t: &str, p: &str| -> Vec<String> {
            split_bytes(t.as_bytes(), p.as_bytes()).into_iter().map(|b| String::from_utf8(b).unwrap()).collect()
        };
        assert_eq!(s("a,b,c", ","), ["a", "b", "c"]);
        assert_eq!(s("a,,b", ","), ["a", "", "b"]);
        assert_eq!(s(",a,", ","), ["", "a", ""]);
        assert_eq!(s("", ","), [""]);
        assert_eq!(s("one", ","), ["one"]);
        assert_eq!(s("a--b--c", "--"), ["a", "b", "c"]);
        assert_eq!(s("abc", ""), ["a", "b", "c"]);
        assert!(s("", "").is_empty());
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

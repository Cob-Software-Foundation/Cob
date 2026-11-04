//! pantry -- a SQLite database to keep things in    (shuck pantry)
//!
//!   pantry_open(path)         open (or create) a database file; returns a
//!                             handle (1, 2, ...), or 0 on failure
//!   pantry_exec(h, sql)       run a statement that gives no answer
//!                             (CREATE, INSERT, UPDATE, ...); 0 on success
//!   pantry_query(h, sql)      run a SELECT; returns the first column of
//!                             the first row as a string ("" if none)
//!   pantry_close(h)           close it; returns 0
//! The SQLite amalgamation (vendor/SQLite) is compiled into libpantry.

use std::ffi::{c_void, CStr, CString};
use std::os::raw::{c_char, c_int};
use std::sync::Mutex;

use cob_abi::*;

const MOD: &str = "pantry";
const PANTRY_MAX_HANDLES: usize = 16;
const SQLITE_OK: c_int = 0;
const SQLITE_ABORT: c_int = 4;

#[allow(non_camel_case_types)]
type sqlite3 = c_void;

type ExecCb = extern "C" fn(*mut c_void, c_int, *mut *mut c_char, *mut *mut c_char) -> c_int;

extern "C" {
    fn sqlite3_open(filename: *const c_char, db: *mut *mut sqlite3) -> c_int;
    fn sqlite3_close(db: *mut sqlite3) -> c_int;
    fn sqlite3_errmsg(db: *mut sqlite3) -> *const c_char;
    fn sqlite3_exec(
        db: *mut sqlite3,
        sql: *const c_char,
        cb: Option<ExecCb>,
        arg: *mut c_void,
        errmsg: *mut *mut c_char,
    ) -> c_int;
    fn sqlite3_free(p: *mut c_void);
}

/// Open databases, as addresses (raw pointers are not `Send`).
static HANDLES: Mutex<[usize; PANTRY_MAX_HANDLES]> = Mutex::new([0; PANTRY_MAX_HANDLES]);
static RESULT: Slot = Slot::new();

fn handles() -> std::sync::MutexGuard<'static, [usize; PANTRY_MAX_HANDLES]> {
    HANDLES.lock().unwrap_or_else(|e| e.into_inner())
}

fn pantry_get(h: i64) -> *mut sqlite3 {
    if h < 1 || h > PANTRY_MAX_HANDLES as i64 {
        return std::ptr::null_mut();
    }
    handles()[(h - 1) as usize] as *mut sqlite3
}

unsafe fn c_text(p: *const c_char) -> String {
    if p.is_null() {
        "unknown error".to_string()
    } else {
        CStr::from_ptr(p).to_string_lossy().into_owned()
    }
}

#[no_mangle]
pub unsafe extern "C" fn pantry_open(argv: *mut CobValue, argc: c_int) -> CobValue {
    if !arity(MOD, "pantry_open", argc, 1) {
        return cob_int(0);
    }
    let path = arg_str(MOD, "pantry_open", argv, argc, 0);
    let cpath = CString::new(path).unwrap_or_default();
    let mut table = handles();
    for i in 0..PANTRY_MAX_HANDLES {
        if table[i] != 0 {
            continue;
        }
        let mut db: *mut sqlite3 = std::ptr::null_mut();
        if sqlite3_open(cpath.as_ptr(), &mut db) != SQLITE_OK {
            let why = if db.is_null() { "unknown error".to_string() } else { c_text(sqlite3_errmsg(db)) };
            ewrite(&format!("[{}] warning: pantry_open(\"{}\") failed: {}\n", MOD, lossy(path), why));
            if !db.is_null() {
                sqlite3_close(db);
            }
            return cob_int(0);
        }
        table[i] = db as usize;
        return cob_int(i as i64 + 1);
    }
    ewrite(&format!(
        "[{}] warning: pantry_open(\"{}\") failed: too many open handles (max {})\n",
        MOD,
        lossy(path),
        PANTRY_MAX_HANDLES
    ));
    cob_int(0)
}

#[no_mangle]
pub unsafe extern "C" fn pantry_exec(argv: *mut CobValue, argc: c_int) -> CobValue {
    if !arity(MOD, "pantry_exec", argc, 2) {
        return cob_int(-1);
    }
    let h = arg_int(MOD, "pantry_exec", argv, argc, 0);
    let sql = arg_str(MOD, "pantry_exec", argv, argc, 1);
    let db = pantry_get(h);
    if db.is_null() {
        ewrite(&format!("[{}] warning: pantry_exec() on invalid/closed handle {}\n", MOD, h));
        return cob_int(-1);
    }
    let csql = CString::new(sql).unwrap_or_default();
    let mut err: *mut c_char = std::ptr::null_mut();
    if sqlite3_exec(db, csql.as_ptr(), None, std::ptr::null_mut(), &mut err) != SQLITE_OK {
        ewrite(&format!("[{}] warning: pantry_exec() failed: {}\n", MOD, c_text(err)));
        sqlite3_free(err as *mut c_void);
        return cob_int(-1);
    }
    cob_int(0)
}

/// sqlite3_exec() callback: keep the first column of the first row, then stop.
extern "C" fn first_cb(user: *mut c_void, n: c_int, vals: *mut *mut c_char, _names: *mut *mut c_char) -> c_int {
    unsafe {
        let out = &mut *(user as *mut Option<Vec<u8>>);
        if n > 0 && !(*vals).is_null() {
            *out = Some(CStr::from_ptr(*vals).to_bytes().to_vec());
        }
    }
    1
}

#[no_mangle]
pub unsafe extern "C" fn pantry_query(argv: *mut CobValue, argc: c_int) -> CobValue {
    if !arity(MOD, "pantry_query", argc, 2) {
        return cob_empty_str();
    }
    let h = arg_int(MOD, "pantry_query", argv, argc, 0);
    let sql = arg_str(MOD, "pantry_query", argv, argc, 1);
    let db = pantry_get(h);
    if db.is_null() {
        ewrite(&format!("[{}] warning: pantry_query() on invalid/closed handle {}\n", MOD, h));
        return RESULT.ret(b"");
    }
    let csql = CString::new(sql).unwrap_or_default();
    let mut result: Option<Vec<u8>> = None;
    let mut err: *mut c_char = std::ptr::null_mut();
    let rc = sqlite3_exec(db, csql.as_ptr(), Some(first_cb), &mut result as *mut _ as *mut c_void, &mut err);
    if rc != SQLITE_OK && rc != SQLITE_ABORT {
        ewrite(&format!("[{}] warning: pantry_query() failed: {}\n", MOD, c_text(err)));
    }
    // also set (to "query aborted") when the callback stops after the first row
    sqlite3_free(err as *mut c_void);
    RESULT.ret(result.as_deref().unwrap_or(b""))
}

#[no_mangle]
pub unsafe extern "C" fn pantry_close(argv: *mut CobValue, argc: c_int) -> CobValue {
    if !arity(MOD, "pantry_close", argc, 1) {
        return cob_int(0);
    }
    let h = arg_int(MOD, "pantry_close", argv, argc, 0);
    let db = pantry_get(h);
    if !db.is_null() {
        sqlite3_close(db);
        handles()[(h - 1) as usize] = 0;
    }
    cob_int(0)
}

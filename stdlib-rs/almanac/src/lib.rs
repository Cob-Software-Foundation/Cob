//! almanac -- the farmer's calendar and clock    (shuck almanac)
//!
//!   almanac_now()        seconds since 1970-01-01 (Unix time)
//!   almanac_ticks()      milliseconds on a steady clock (for timing things)
//!   almanac_nap(ms)      sleep for `ms` milliseconds
//!   almanac_year()       the current year (local time)
//!   almanac_stamp()      "YYYY-MM-DD HH:MM:SS" in local time

use std::os::raw::c_int;

use cob_abi::*;

const MOD: &str = "almanac";
static STAMP: Slot = Slot::new();

#[cfg(target_arch = "wasm32")]
fn unix_now() -> i64 {
    cob_abi::host::now_secs()
}

#[cfg(not(target_arch = "wasm32"))]
fn unix_now() -> i64 {
    match std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH) {
        Ok(d) => d.as_secs() as i64,
        Err(e) => -(e.duration().as_secs() as i64),
    }
}

/// Broken-down local time: (year, month 1-12, day, hour, minute, second).
#[cfg(unix)]
fn local_now() -> Option<(i64, u32, u32, u32, u32, u32)> {
    unsafe {
        let t: libc::time_t = libc::time(std::ptr::null_mut());
        let mut tm: libc::tm = std::mem::zeroed();
        if libc::localtime_r(&t, &mut tm).is_null() {
            return None;
        }
        Some((
            tm.tm_year as i64 + 1900,
            (tm.tm_mon + 1) as u32,
            tm.tm_mday as u32,
            tm.tm_hour as u32,
            tm.tm_min as u32,
            tm.tm_sec as u32,
        ))
    }
}

#[cfg(target_arch = "wasm32")]
fn local_now() -> Option<(i64, u32, u32, u32, u32, u32)> {
    Some(cob_abi::host::local_time())
}

#[cfg(windows)]
fn local_now() -> Option<(i64, u32, u32, u32, u32, u32)> {
    #[repr(C)]
    #[derive(Default)]
    struct SystemTime {
        year: u16,
        month: u16,
        dow: u16,
        day: u16,
        hour: u16,
        minute: u16,
        second: u16,
        ms: u16,
    }
    extern "system" {
        fn GetLocalTime(st: *mut SystemTime);
    }
    let mut st = SystemTime::default();
    unsafe { GetLocalTime(&mut st) };
    Some((st.year as i64, st.month as u32, st.day as u32, st.hour as u32, st.minute as u32, st.second as u32))
}

#[no_mangle]
pub unsafe extern "C" fn almanac_now(_argv: *mut CobValue, _argc: c_int) -> CobValue {
    cob_int(unix_now())
}

#[no_mangle]
pub unsafe extern "C" fn almanac_ticks(_argv: *mut CobValue, _argc: c_int) -> CobValue {
    #[cfg(target_arch = "wasm32")]
    {
        return cob_int(cob_abi::host::ticks_ms());
    }
    #[cfg(windows)]
    {
        extern "system" {
            fn GetTickCount64() -> u64;
        }
        return cob_int(GetTickCount64() as i64);
    }
    #[cfg(unix)]
    {
        let mut ts: libc::timespec = std::mem::zeroed();
        libc::clock_gettime(libc::CLOCK_MONOTONIC, &mut ts);
        cob_int(ts.tv_sec as i64 * 1000 + ts.tv_nsec as i64 / 1_000_000)
    }
}

#[no_mangle]
pub unsafe extern "C" fn almanac_nap(argv: *mut CobValue, argc: c_int) -> CobValue {
    if !arity(MOD, "almanac_nap", argc, 1) {
        return cob_int(0);
    }
    let ms = arg_int(MOD, "almanac_nap", argv, argc, 0);
    if ms <= 0 {
        return cob_int(0);
    }
    #[cfg(target_arch = "wasm32")]
    cob_abi::host::nap_ms(ms);
    #[cfg(not(target_arch = "wasm32"))]
    std::thread::sleep(std::time::Duration::from_millis(ms as u64));
    cob_int(0)
}

#[no_mangle]
pub unsafe extern "C" fn almanac_year(_argv: *mut CobValue, _argc: c_int) -> CobValue {
    cob_int(local_now().map_or(0, |t| t.0))
}

#[no_mangle]
pub unsafe extern "C" fn almanac_stamp(_argv: *mut CobValue, _argc: c_int) -> CobValue {
    match local_now() {
        None => cob_empty_str(),
        Some((y, mo, d, h, mi, s)) => STAMP.ret(format!("{:04}-{:02}-{:02} {:02}:{:02}:{:02}", y, mo, d, h, mi, s).as_bytes()),
    }
}

/// Every function this module exports, by name -- for builds that cannot
/// `dlopen()` (the browser playground) and bind the module in statically.
pub static EXPORTS: &[(&str, cob_abi::CobFFIFunc)] = &[
    ("almanac_now", almanac_now),
    ("almanac_ticks", almanac_ticks),
    ("almanac_nap", almanac_nap),
    ("almanac_year", almanac_year),
    ("almanac_stamp", almanac_stamp),
];

//! Clock and sleep for the browser build, where std's `SystemTime` and
//! `thread::sleep` are unavailable: the host page provides them.

extern "C" {
    /// milliseconds since the Unix epoch
    fn cob_host_now_ms() -> f64;
    /// milliseconds on a steady clock
    fn cob_host_ticks_ms() -> f64;
    /// block for this many milliseconds
    fn cob_host_nap(ms: i32);
    /// writes year, month(1-12), day, hour, minute, second as six i32s
    fn cob_host_local_time(out: *mut i32);
}

pub fn now_secs() -> i64 {
    (unsafe { cob_host_now_ms() } / 1000.0).floor() as i64
}

pub fn ticks_ms() -> i64 {
    unsafe { cob_host_ticks_ms() as i64 }
}

pub fn nap_ms(ms: i64) {
    unsafe { cob_host_nap(ms.clamp(0, i32::MAX as i64) as i32) }
}

/// (year, month, day, hour, minute, second) in the viewer's local time.
pub fn local_time() -> (i64, u32, u32, u32, u32, u32) {
    let mut t = [0i32; 6];
    unsafe { cob_host_local_time(t.as_mut_ptr()) };
    (t[0] as i64, t[1] as u32, t[2] as u32, t[3] as u32, t[4] as u32, t[5] as u32)
}

//! C-compatible number parsing (`strtol` base 10), because Cob's observable
//! behaviour -- clamping on overflow, what counts as trailing junk -- was
//! defined by it.

fn is_c_space(b: u8) -> bool {
    matches!(b, b' ' | b'\t' | b'\n' | 0x0b | 0x0c | b'\r')
}

/// Parses like `strtol(s, &end, 10)`: returns (value, bytes consumed).
/// Consumed == 0 means no number was found. Overflow clamps to i64::MIN/MAX.
pub fn strtol10(s: &[u8]) -> (i64, usize) {
    let mut i = 0;
    while i < s.len() && is_c_space(s[i]) {
        i += 1;
    }
    let mut neg = false;
    if i < s.len() && (s[i] == b'+' || s[i] == b'-') {
        neg = s[i] == b'-';
        i += 1;
    }
    let digits_start = i;
    let mut acc: i128 = 0;
    let mut overflow = false;
    while i < s.len() && s[i].is_ascii_digit() {
        if !overflow {
            acc = acc * 10 + (s[i] - b'0') as i128;
            if acc > (i64::MAX as i128) + 1 {
                overflow = true;
            }
        }
        i += 1;
    }
    if i == digits_start {
        return (0, 0);
    }
    let v = if neg { -acc } else { acc };
    let v = if overflow || v > i64::MAX as i128 {
        if neg { i64::MIN } else { i64::MAX }
    } else if v < i64::MIN as i128 {
        i64::MIN
    } else {
        v as i64
    };
    (v, i)
}

/// "whole number, blanks allowed around it, nothing else" -- the rule behind
/// kernel_to_int() and porch_ask_int(). `None` if it isn't one.
pub fn parse_whole_number(s: &[u8]) -> Option<i64> {
    let mut q = 0;
    while q < s.len() && (s[q] == b' ' || s[q] == b'\t') {
        q += 1;
    }
    if q >= s.len() {
        return None;
    }
    let (n, used) = strtol10(&s[q..]);
    if used == 0 {
        return None;
    }
    let mut end = q + used;
    while end < s.len() && (s[end] == b' ' || s[end] == b'\t') {
        end += 1;
    }
    if end == s.len() {
        Some(n)
    } else {
        None
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn basics() {
        assert_eq!(strtol10(b"42"), (42, 2));
        assert_eq!(strtol10(b"  -7x"), (-7, 4));
        assert_eq!(strtol10(b"abc"), (0, 0));
        assert_eq!(strtol10(b"99999999999999999999").0, i64::MAX);
        assert_eq!(strtol10(b"-99999999999999999999").0, i64::MIN);
        assert_eq!(parse_whole_number(b"  7  "), Some(7));
        assert_eq!(parse_whole_number(b"12abc"), None);
        assert_eq!(parse_whole_number(b""), None);
    }
}

//! cob_sign -- Cob Enterprise license signing tool (maintainer only)
//!
//! Run this on YOUR machine. The private key it creates must never be
//! committed, copied to a server, or shipped (see .gitignore).
//!
//!   cob_sign keygen [--key-id N] [--out cob_private.key] [--pubkeys keys/drm_pubkeys.txt]
//!   cob_sign sign   --key cob_private.key --customer "Acme Corp"
//!                   [--expires YYYY-MM-DD|never] [--features a,b|*] [--seats N]
//!                   [--key-id N] [--out cob.lic]
//!                   [--machine <hash> | --no-bind]
//!   cob_sign info   cob.lic [--key cob_private.key]
//!   cob_sign machine-id
//!
//! MACHINE BINDING. By default a signed license carries `bind=machine`: the
//! first Cob Enterprise tool to start with it records this machine's id in the
//! license file (below the signature) and every later run must match. To pin a
//! license to a machine you already know, have the customer run
//! `cob_sign machine-id` (or `cob_interp_enterprise` prints it on a mismatch)
//! and pass the result as `--machine`; that is signed into the license and can't
//! be edited away. `--no-bind` issues an unbound license (the old behaviour).

use std::fs;
use std::path::Path;

use cob_drm::eddsa;
use cob_drm::{b64encode, machine, verify_text, Bind};

fn to_hex(b: &[u8]) -> String {
    b.iter().map(|x| format!("{:02x}", x)).collect()
}

fn from_hex32(s: &str) -> Option<[u8; 32]> {
    let s = s.trim();
    if s.len() < 64 || !s.is_char_boundary(64) {
        return None;
    }
    let mut out = [0u8; 32];
    for i in 0..32 {
        out[i] = u8::from_str_radix(&s[2 * i..2 * i + 2], 16).ok()?;
    }
    Some(out)
}

#[cfg(unix)]
fn random_bytes(buf: &mut [u8]) -> bool {
    use std::io::Read;
    fs::File::open("/dev/urandom").and_then(|mut f| f.read_exact(buf)).is_ok()
}

#[cfg(windows)]
fn random_bytes(buf: &mut [u8]) -> bool {
    #[link(name = "advapi32")]
    extern "system" {
        fn SystemFunction036(buf: *mut u8, len: u32) -> u8; // RtlGenRandom
    }
    unsafe { SystemFunction036(buf.as_mut_ptr(), buf.len() as u32) != 0 }
}

fn load_seed(path: &str) -> Option<[u8; 32]> {
    let Ok(text) = fs::read_to_string(path) else {
        eprintln!("cob_sign: cannot open key file '{}'", path);
        return None;
    };
    match text.lines().next().and_then(from_hex32) {
        Some(s) => Some(s),
        None => {
            eprintln!("cob_sign: '{}' is not a valid Cob private key", path);
            None
        }
    }
}

/// Value following `--name` in argv[2..], else `def`.
fn opt<'a>(args: &'a [String], name: &str, def: Option<&'a str>) -> Option<&'a str> {
    let mut i = 2;
    while i + 1 < args.len() {
        if args[i] == name {
            return Some(&args[i + 1]);
        }
        i += 1;
    }
    def
}

fn flag(args: &[String], name: &str) -> bool {
    args.iter().skip(2).any(|a| a == name)
}

fn atoi(s: &str) -> i32 {
    let t = s.trim();
    let (neg, t) = t.strip_prefix('-').map_or((false, t), |r| (true, r));
    let v: i64 = t.chars().take_while(|c| c.is_ascii_digit()).collect::<String>().parse().unwrap_or(0);
    (if neg { -v } else { v }) as i32
}

fn cmd_keygen(args: &[String]) -> i32 {
    let out = opt(args, "--out", Some("cob_private.key")).unwrap();
    let pubfile = opt(args, "--pubkeys", Some("keys/drm_pubkeys.txt")).unwrap();
    let key_id = atoi(opt(args, "--key-id", Some("1")).unwrap());
    if Path::new(out).exists() {
        eprintln!("cob_sign: refusing to overwrite existing '{}'", out);
        return 1;
    }
    // refuse a duplicate id up front, before any secret is written
    let existing = fs::read_to_string(pubfile).unwrap_or_default();
    if existing
        .lines()
        .any(|l| l.split_whitespace().next().and_then(|t| t.parse::<i32>().ok()) == Some(key_id))
    {
        eprintln!("cob_sign: key id {} already exists in '{}' (pick another --key-id to rotate)", key_id, pubfile);
        return 1;
    }
    let mut seed = [0u8; 32];
    if !random_bytes(&mut seed) {
        eprintln!("cob_sign: no secure randomness available");
        return 1;
    }
    let pk = eddsa::public_key(&seed);
    if fs::write(out, format!("{}\n", to_hex(&seed))).is_err() {
        eprintln!("cob_sign: cannot write '{}'", out);
        return 1;
    }
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        let _ = fs::set_permissions(out, fs::Permissions::from_mode(0o600));
    }
    let mut text = existing;
    if text.is_empty() {
        text.push_str("# Cob Enterprise license PUBLIC keys -- safe to commit.\n# One \"<key_id> <64 hex chars>\" per line. Written by `cob_sign keygen`.\n# To rotate keys, add another line: licenses signed with older keys keep working.\n");
    } else if !text.ends_with('\n') {
        text.push('\n');
    }
    text.push_str(&format!("{} {}\n", key_id, to_hex(&pk)));
    if let Some(dir) = Path::new(pubfile).parent() {
        let _ = fs::create_dir_all(dir);
    }
    if fs::write(pubfile, text).is_err() {
        eprintln!("cob_sign: cannot write '{}'", pubfile);
        return 1;
    }
    println!("Wrote private key  : {}   <-- KEEP SECRET, BACK IT UP OFFLINE, NEVER COMMIT", out);
    println!("Wrote public key   : {}   (commit this one)", pubfile);
    0
}

fn cmd_sign(args: &[String]) -> i32 {
    let (Some(keyf), Some(cust)) = (opt(args, "--key", None), opt(args, "--customer", None)) else {
        eprintln!("usage: cob_sign sign --key FILE --customer NAME [--expires DATE|never] [--features LIST|*] [--seats N] [--key-id N] [--out FILE] [--machine HASH | --no-bind]");
        return 1;
    };
    let exp = opt(args, "--expires", Some("never")).unwrap();
    let feat = opt(args, "--features", Some("*")).unwrap();
    let seats = atoi(opt(args, "--seats", Some("0")).unwrap());
    let key_id = atoi(opt(args, "--key-id", Some("1")).unwrap());
    let out = opt(args, "--out", Some("cob.lic")).unwrap();
    let machine_arg = opt(args, "--machine", None);
    let no_bind = flag(args, "--no-bind");

    for f in [cust, feat, exp] {
        if f.contains('\r') || f.contains('\n') {
            eprintln!("cob_sign: fields must not contain newlines");
            return 1;
        }
    }
    if no_bind && machine_arg.is_some() {
        eprintln!("cob_sign: --machine and --no-bind contradict each other");
        return 1;
    }
    if let Some(m) = machine_arg {
        if m.len() != 64 || !m.bytes().all(|b| b.is_ascii_hexdigit()) {
            eprintln!("cob_sign: --machine must be the 64-hex-digit id printed by `cob_sign machine-id`");
            return 1;
        }
    }
    let Some(seed) = load_seed(keyf) else { return 1 };

    let issued = {
        let secs = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0);
        cob_drm_today(secs as i64)
    };
    let binding = match (machine_arg, no_bind) {
        (Some(m), _) => format!("machine={}\n", m.to_ascii_lowercase()),
        (None, true) => String::new(),
        (None, false) => "bind=machine\n".to_string(),
    };
    let payload = format!(
        "COBLIC1\ncustomer={}\nissued={}\nexpires={}\nfeatures={}\nseats={}\nkey_id={}\n{}",
        cust, issued, exp, feat, seats, key_id, binding
    );
    if payload.len() >= 1024 {
        eprintln!("cob_sign: license fields too long");
        return 1;
    }
    let pk = eddsa::public_key(&seed);
    let sig = eddsa::sign(&seed, payload.as_bytes());
    let text = format!("{}---\n{}\n", payload, b64encode(&sig));
    if fs::write(out, &text).is_err() {
        eprintln!("cob_sign: cannot write '{}'", out);
        return 1;
    }
    // self-check: re-read and verify with the matching public key
    let again = fs::read(out).unwrap_or_default();
    let v = match verify_text(&again, &pk) {
        Ok(v) => v,
        Err(e) => {
            eprintln!("cob_sign: self-check failed: {}", e.text());
            return 1;
        }
    };
    let how = if machine_arg.is_some() {
        "bound to the given machine"
    } else if no_bind {
        "not machine-bound"
    } else {
        "binds to the first machine that runs it"
    };
    println!("Wrote {} for \"{}\" (expires {}, features {}, seats {}, key_id {}; {})", out, cust, exp, feat, seats, key_id, how);
    if v.expired {
        println!("warning: that expiry date is already in the past");
    }
    0
}

/// UTC "YYYY-MM-DD" for a Unix time.
fn cob_drm_today(secs: i64) -> String {
    let z = secs.div_euclid(86400) + 719468;
    let era = if z >= 0 { z } else { z - 146096 } / 146097;
    let doe = z - era * 146097;
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    let y = yoe + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    format!("{:04}-{:02}-{:02}", if m <= 2 { y + 1 } else { y }, m, d)
}

fn cmd_info(args: &[String]) -> i32 {
    let Some(path) = args.get(2).filter(|a| !a.starts_with("--")) else {
        eprintln!("usage: cob_sign info FILE [--key private.key]");
        return 1;
    };
    let Ok(raw) = fs::read(path) else {
        eprintln!("cob_sign: cannot open '{}'", path);
        return 1;
    };
    match opt(args, "--key", None) {
        None => {
            println!("{}\n(signature not checked -- pass --key to verify)", String::from_utf8_lossy(&raw));
        }
        Some(keyf) => {
            let Some(seed) = load_seed(keyf) else { return 1 };
            match verify_text(&raw, &eddsa::public_key(&seed)) {
                Err(e) => println!("signature: {}", e.text()),
                Ok(v) => {
                    println!("signature: VALID");
                    println!("customer : {}", v.lic.customer);
                    println!("issued   : {}", v.lic.issued);
                    println!("expires  : {}{}", v.lic.expires, if v.expired { "  (EXPIRED)" } else { "" });
                    println!("features : {}", v.lic.features);
                    println!("seats    : {}", v.lic.seats);
                    println!("key_id   : {}", v.lic.key_id);
                    match (&v.lic.machine, v.lic.bind, &v.bound_machine) {
                        (Some(m), _, _) => println!("binding  : pre-bound (signed) to machine {}", m),
                        (None, Bind::FirstRun, Some(b)) => {
                            println!("binding  : bound on {} to machine {}  (unsigned record)", v.bound_on.as_deref().unwrap_or("?"), b)
                        }
                        (None, Bind::FirstRun, None) => println!("binding  : binds on first run (not yet used)"),
                        (None, Bind::None, _) => println!("binding  : none"),
                    }
                }
            }
        }
    }
    0
}

fn cmd_machine_id() -> i32 {
    match machine::id_hash() {
        Some(h) => {
            println!("{}", h);
            0
        }
        None => {
            eprintln!("cob_sign: this system does not expose a machine id");
            1
        }
    }
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let code = match args.get(1).map(String::as_str) {
        Some("keygen") => cmd_keygen(&args),
        Some("sign") => cmd_sign(&args),
        Some("info") => cmd_info(&args),
        Some("machine-id") => cmd_machine_id(),
        _ => {
            eprint!(
                "cob_sign -- Cob Enterprise license tool (maintainer only)\n\
                 \x20 cob_sign keygen [--key-id N] [--out cob_private.key] [--pubkeys keys/drm_pubkeys.txt]\n\
                 \x20 cob_sign sign --key FILE --customer NAME [--expires YYYY-MM-DD|never]\n\
                 \x20               [--features a,b|*] [--seats N] [--key-id N] [--out cob.lic]\n\
                 \x20               [--machine HASH | --no-bind]\n\
                 \x20 cob_sign info FILE [--key private.key]\n\
                 \x20 cob_sign machine-id          print this machine's id (for --machine)\n"
            );
            1
        }
    };
    std::process::exit(code);
}

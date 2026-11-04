// Turns keys/drm_pubkeys.txt (the PUBLIC keys -- safe to commit; one
// "<key_id> <64 hex chars>" per line, '#' comments allowed) into a Rust table.
// A missing file gives an empty table, so cob_sign and the tests still build;
// an enterprise binary with no keys can't validate any license.
use std::{env, fs, path::Path};

fn main() {
    let src = Path::new("../../keys/drm_pubkeys.txt");
    println!("cargo:rerun-if-changed={}", src.display());
    println!("cargo:rerun-if-changed=build.rs");
    let mut out = String::from("pub const EMBEDDED_KEYS: &[(i32, [u8; 32])] = &[\n");
    if let Ok(text) = fs::read_to_string(src) {
        for (n, line) in text.lines().enumerate() {
            let line = line.trim();
            if line.is_empty() || line.starts_with('#') {
                continue;
            }
            let mut it = line.split_whitespace();
            let id: i32 = it.next().and_then(|s| s.parse().ok()).unwrap_or_else(|| panic!("keys/drm_pubkeys.txt:{}: bad key id", n + 1));
            let hex = it.next().unwrap_or_else(|| panic!("keys/drm_pubkeys.txt:{}: missing key", n + 1));
            assert!(hex.len() == 64 && hex.bytes().all(|b| b.is_ascii_hexdigit()), "keys/drm_pubkeys.txt:{}: key must be 64 hex chars", n + 1);
            let bytes: Vec<String> = (0..32).map(|i| format!("0x{}", &hex[2 * i..2 * i + 2])).collect();
            out.push_str(&format!("    ({}, [{}]),\n", id, bytes.join(", ")));
        }
    } else {
        println!("cargo:warning=keys/drm_pubkeys.txt not found: enterprise builds will reject every license (see ENTERPRISE.md)");
    }
    out.push_str("];\n");
    fs::write(Path::new(&env::var("OUT_DIR").unwrap()).join("keys_gen.rs"), out).unwrap();
}

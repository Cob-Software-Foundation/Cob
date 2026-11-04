// Cob programs recurse (up to 500 calls deep) and the parser is recursive
// descent: give the wasm shadow stack 8 MiB instead of the 1 MiB default so a
// deeply nested program fails cleanly instead of trapping.
fn main() {
    println!("cargo:rustc-link-arg-cdylib=-zstack-size=8388608");
}

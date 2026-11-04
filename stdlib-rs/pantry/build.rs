// The SQLite amalgamation vendored in vendor/SQLite is compiled straight into
// libpantry, exactly as the C build did (no system SQLite needed).
fn main() {
    let src = "../../vendor/SQLite/sqlite3.c";
    println!("cargo:rerun-if-changed={}", src);
    println!("cargo:rerun-if-changed=build.rs");
    let mut b = cc::Build::new();
    b.file(src)
        .include("../../vendor/SQLite")
        .define("SQLITE_THREADSAFE", "1")
        .define("SQLITE_OMIT_LOAD_EXTENSION", None)
        .opt_level(2)
        .pic(true)
        .warnings(false);
    b.compile("sqlite3_vendored");
    if !cfg!(windows) {
        println!("cargo:rustc-link-lib=pthread");
        println!("cargo:rustc-link-lib=m");
    }
}

//! popcorn_comp <file.strawberry> -o <output> [--no-gc] [--cc <path>] [--emit-c <path.c>]
//!
//! HOW THIS WORKS: popcorn_comp transpiles a `.strawberry` AST to plain C,
//! writes it to a file, then spawns a real C compiler (Zig's `zig cc` by
//! default; gcc/clang/cc or any cross compiler on request) as a subprocess to
//! turn that C into a native executable. Spawning a real compiler trades "no
//! external executables" for "works with whatever well-tested compiler is the
//! standard tool for the target" -- the same tradeoff Zig's C backend or
//! Cython make.
//!
//! CHOOSING THE COMPILER, in order:
//!   1. --cc <path>          explicit override, always wins
//!   2. $CobCC               environment override
//!   3. $CobOS / $CobArch    looked up in a table of Zig target triples
//!      (a bundled `zig/` folder next to this executable is preferred,
//!       otherwise `zig cc` from PATH)
//!   4. plain `zig cc`
//!
//! Every Cob variable is emitted as a tagged `CobValue`, and every operator is
//! a call into the small C runtime in runtime.rs, so compiled behaviour matches
//! interpreted behaviour (warnings from compiled code say "[cob]").
//!
//! The standard library needs no special support: it is `extern` calls into
//! shared libraries, linked with -l<module> plus -L/rpath for the standard
//! library directory.

mod runtime;
mod transpile;

use std::path::Path;
use std::process::Command;

use cob_core::ast::program_uses_harvest_or_trash;
use cob_core::consts::*;
use transpile::{check_recipes, collect_externs, transpile, ExternDecl};

/// (CobOS, CobArch) -> Zig target triple. Linux targets default to musl so the
/// result is a fully static binary with no runtime libc dependency.
const ZIG_TARGETS: &[(&str, &str, &str)] = &[
    ("windows", "amd64", "x86_64-windows-gnu"),
    ("windows", "386", "x86-windows-gnu"),
    ("windows", "arm64", "aarch64-windows-gnu"),
    ("linux", "amd64", "x86_64-linux-musl"),
    ("linux", "386", "x86-linux-musl"),
    ("linux", "arm64", "aarch64-linux-musl"),
    ("linux", "arm", "arm-linux-musleabihf"),
    ("darwin", "amd64", "x86_64-macos"),
    ("darwin", "arm64", "aarch64-macos"),
];

fn print_usage(argv0: &str) {
    eprint!(
        "Usage: {0} <file{1}> -o <output> [--no-gc] [--cc <compiler>] [--emit-c <path.c>]\n\
         \x20      {0} --license\n\
         \x20      {0} --version\n\
         \x20 --no-gc        required if the program uses harvest()/trash()\n\
         \x20 --cc PATH      explicit compiler to spawn (default: resolved from\n\
         \x20                CobCC / CobOS+CobArch / PATH -- see file header comment)\n\
         \x20 --emit-c PATH  also write the generated C source for inspection\n\
         \x20 --license      print the short license notice and exit\n\
         \n\
         Environment:\n\
         \x20 CobCC    explicit compiler command/path override\n\
         \x20 CobOS    windows|linux|darwin  (looked up in a small cross-compiler table)\n\
         \x20 CobArch  amd64|386|arm64|arm\n",
        argv0, CACHE_EXTENSION
    );
}

/// `<exe dir>/zig/zig(.exe)` if present -> an already-quoted `"path" cc`.
fn find_bundled_zig_cc() -> Option<String> {
    let dir = cob_core::consts::exe_path()?.parent()?.to_path_buf();
    let zig = dir.join("zig").join(if cfg!(windows) { "zig.exe" } else { "zig" });
    if !zig.is_file() {
        return None;
    }
    Some(format!("\"{}\" cc", zig.display()))
}

/// The full compiler *command* (may include arguments) to run, or `None` for
/// an unresolvable request (already reported).
fn resolve_compiler(cli_override: Option<&str>) -> Option<String> {
    if let Some(cc) = cli_override {
        if !cc.is_empty() {
            return Some(cc.to_string());
        }
    }
    if let Ok(cc) = std::env::var("CobCC") {
        if !cc.is_empty() {
            return Some(cc);
        }
    }
    let base = find_bundled_zig_cc().unwrap_or_else(|| "zig cc".to_string());
    if let (Ok(os), Ok(arch)) = (std::env::var("CobOS"), std::env::var("CobArch")) {
        return match ZIG_TARGETS.iter().find(|(o, a, _)| *o == os && *a == arch) {
            Some((_, _, triple)) => Some(format!("{} -target {}", base, triple)),
            None => {
                // refuse rather than silently build the WRONG platform's binary
                eprintln!(
                    "[popcorn_comp] error: no known Zig target for CobOS={} CobArch={}.\n  Pass --cc <path> or set $CobCC to the compiler you want used.",
                    os, arch
                );
                None
            }
        };
    }
    Some(base)
}

fn target_is_windows() -> bool {
    match std::env::var("CobOS") {
        Ok(os) => os == "windows",
        Err(_) => cfg!(windows),
    }
}

/// Shell-quotes one argument for the command line popcorn_comp spawns.
fn quote(s: &str) -> String {
    if cfg!(windows) {
        let mut o = String::from("\"");
        for c in s.chars() {
            if c == '"' || c == '\\' {
                o.push('\\');
            }
            o.push(c);
        }
        o.push('"');
        o
    } else {
        // single quotes: nothing inside is special to the shell
        format!("'{}'", s.replace('\'', "'\\''"))
    }
}

/// A library named with a path/extension ("math_ext.so", "./a.so") is passed to
/// the compiler as a literal link input; a bare name ("m", "porch") becomes a
/// conventional -l flag.
fn lib_looks_like_path(lib: &str) -> bool {
    lib.contains('.') || lib.contains('/') || lib.contains('\\')
}

/// One token per distinct extern library, ready to append to the compiler line.
fn build_extern_link_args(externs: &[ExternDecl], cwd: Option<&Path>) -> String {
    let mut parts: Vec<String> = Vec::new();
    let mut seen: Vec<&str> = Vec::new();
    let (mut any_path, mut any_bare) = (false, false);
    for d in externs {
        if seen.contains(&d.lib.as_str()) {
            continue;
        }
        seen.push(&d.lib);
        if lib_looks_like_path(&d.lib) {
            any_path = true;
            parts.push(quote(&d.lib));
        } else {
            any_bare = true;
            parts.push(format!("-l{}", d.lib));
        }
    }
    // A bare module name is a STANDARD LIBRARY shared library (or a system one
    // like "m"): look in the standard library directory at link time and record
    // it as an rpath so the finished program finds it again when it runs.
    if any_bare {
        if let Some(dir) = stdlib_dir() {
            let dir = dir.to_string_lossy().into_owned();
            parts.push(format!("-L{}", quote(&dir)));
            if !target_is_windows() {
                parts.push(format!("-Wl,-rpath,{}", quote(&dir)));
            }
        }
    }
    // A bare filename like "math_ext.so" links fine, but the binary's DT_NEEDED
    // is that bare name, searched on the SYSTEM library path at run time. An
    // rpath at the build-time directory makes "the .so next to the .cob script"
    // resolve for the common case of running from that directory. (PE already
    // searches the executable's directory.)
    if any_path && !target_is_windows() {
        if let Some(cwd) = cwd {
            parts.push(format!("-Wl,-rpath,{}", quote(&cwd.to_string_lossy())));
        }
    }
    parts.join(" ")
}

/// Runs `compiler -std=c99 -O2 -o out in [link args]`. Success means the
/// subprocess exited 0 AND the output file exists.
fn spawn_compile(compiler: &str, in_path: &str, out_path: &str, extra: &str) -> bool {
    let cmd = format!(
        "{} -std=c99 -O2 -o {} {}{}{}",
        compiler,
        quote(out_path),
        quote(in_path),
        if extra.is_empty() { "" } else { " " },
        extra
    );
    eprintln!("[popcorn_comp] spawning: {}", cmd);
    let status = run_shell(&cmd);
    matches!(status, Some(s) if s.success()) && Path::new(out_path).exists()
}

#[cfg(windows)]
fn run_shell(cmd: &str) -> Option<std::process::ExitStatus> {
    use std::os::windows::process::CommandExt;
    // cmd.exe strips only the first and last '"' of a line that starts with a
    // quote; wrapping the whole line in one more pair is the standard workaround.
    let line = if cmd.starts_with('"') { format!("\"{}\"", cmd) } else { cmd.to_string() };
    Command::new("cmd").arg("/C").raw_arg(line).status().ok()
}

#[cfg(not(windows))]
fn run_shell(cmd: &str) -> Option<std::process::ExitStatus> {
    Command::new("sh").arg("-c").arg(cmd).status().ok()
}

fn main() {
    std::process::exit(real_main());
}

fn real_main() -> i32 {
    let args: Vec<String> = std::env::args().collect();
    let argv0 = args.first().cloned().unwrap_or_else(|| "popcorn_comp".into());
    if check_hidden_version_flag(&args) {
        return 0;
    }
    if check_hidden_license_flag(&args) {
        return 0;
    }
    #[cfg(feature = "enterprise")]
    if cob_drm::require("popcorn_comp") != 0 {
        return 1;
    }

    let (mut in_path, mut out_path, mut emit_c, mut cc_override): (Option<String>, Option<String>, Option<String>, Option<String>) =
        (None, None, None, None);
    let mut no_gc = false;
    let mut i = 1;
    while i < args.len() {
        let a = &args[i];
        let take = |flag: &str, i: &mut usize| -> Result<String, ()> {
            if *i + 1 >= args.len() {
                eprintln!("[popcorn_comp] error: {} needs an argument", flag);
                return Err(());
            }
            *i += 1;
            Ok(args[*i].clone())
        };
        match a.as_str() {
            "-o" => match take("-o", &mut i) {
                Ok(v) => out_path = Some(v),
                Err(()) => return 1,
            },
            "--no-gc" => no_gc = true,
            "--cc" => match take("--cc", &mut i) {
                Ok(v) => cc_override = Some(v),
                Err(()) => return 1,
            },
            "--emit-c" => match take("--emit-c", &mut i) {
                Ok(v) => emit_c = Some(v),
                Err(()) => return 1,
            },
            f if f.starts_with('-') => {
                eprintln!("[popcorn_comp] error: unrecognized flag '{}'", f);
                print_usage(&argv0);
                return 1;
            }
            _ if in_path.is_none() => in_path = Some(a.clone()),
            _ => {
                eprintln!("[popcorn_comp] error: unexpected extra argument '{}'", a);
                print_usage(&argv0);
                return 1;
            }
        }
        i += 1;
    }
    let (Some(in_path), Some(out_path)) = (in_path, out_path) else {
        print_usage(&argv0);
        return 1;
    };

    let Some(compiler) = resolve_compiler(cc_override.as_deref()) else { return 1 };

    // --- load and check the bytecode ---
    let Some(data) = cob_core::fsutil::read_all(&in_path) else {
        eprintln!("[popcorn_comp] error: could not read '{}'", in_path);
        return 1;
    };
    if data.len() < STRAWBERRY_MAGIC.len() || &data[..STRAWBERRY_MAGIC.len()] != STRAWBERRY_MAGIC {
        eprintln!(
            "[popcorn_comp] error: '{}' is not a valid .strawberry file (expected magic \"{}\")",
            in_path,
            String::from_utf8_lossy(STRAWBERRY_MAGIC)
        );
        return 1;
    }
    let Ok(prog) = cob_core::cache::decode(&data) else {
        eprintln!("[popcorn_comp] error: '{}' is corrupt or truncated", in_path);
        return 1;
    };

    if !no_gc && program_uses_harvest_or_trash(&prog) {
        eprintln!("[popcorn_comp] error: this program uses harvest()/trash() -- rerun with --no-gc");
        return 1;
    }
    if check_recipes(&prog).is_err() {
        return 1;
    }

    // --- C API / FFI (CEP-2): collect extern declarations, conflict-check ---
    let mut externs: Vec<ExternDecl> = Vec::new();
    if collect_externs(&prog, &mut externs).is_err() {
        return 1;
    }
    let cwd = std::env::current_dir().ok();
    let extra = build_extern_link_args(&externs, cwd.as_deref());

    // --- transpile to C ---
    let c_source = transpile(&prog, &externs);

    // The C must live in a real file for a spawned compiler to read. --emit-c's
    // path doubles as the compile input and the inspectable artifact; otherwise
    // a temp file next to the requested output, cleaned up afterwards.
    let using_emit = emit_c.is_some();
    let c_path = emit_c.unwrap_or_else(|| format!("{}.popcorn_tmp.c", out_path));
    if std::fs::write(&c_path, &c_source).is_err() {
        eprintln!("[popcorn_comp] error: could not write generated C source to '{}'", c_path);
        return 1;
    }

    // --- spawn the real compiler ---
    if !spawn_compile(&compiler, &c_path, &out_path, &extra) {
        eprintln!(
            "[popcorn_comp] error: '{}' failed to compile the generated C source\n  (rerun with --emit-c out.c to inspect it, or check that '{}' is a valid,\n  installed compiler on PATH{})",
            compiler,
            compiler,
            if extra.is_empty() { "" } else { ", and that every extern library resolves at link time" }
        );
        if !using_emit {
            let _ = std::fs::remove_file(&c_path);
        }
        return 1;
    }
    if !using_emit {
        let _ = std::fs::remove_file(&c_path);
    }
    eprintln!("[popcorn_comp] wrote {} (compiler: {})", out_path, compiler);
    0
}

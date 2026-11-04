//! cob_interp [file.cob] [--no-cache] [--no-gc] [-i]

use cob_abi::cstdio::flush_stdout;
use cob_abi::ewrite;
use cob_core::consts::*;
use cob_core::{cache, fsutil, Session};

fn print_usage(argv0: &str) {
    ewrite(&format!(
        "Usage: {0} [file.cob] [--no-cache] [--no-gc] [-i]\n\
         \x20      {0} --license\n\
         \x20      {0} --version\n\
         \x20 (no file)    start the interactive REPL\n\
         \x20 -i, --repl   run the file (if any), then stay in the REPL with its state\n\
         \x20 --no-cache   ignore/skip the .strawberry fast-boot cache\n\
         \x20 --no-gc      unlock harvest(<bytes>) / trash(<variable>)\n\
         \x20 --license    print the short license notice and exit\n\
         \n\
         If a .cob file's first line is exactly `_MakeCache = False`\n\
         (whitespace/case-insensitive), the .strawberry cache is\n\
         disabled for that file automatically.\n\
         \n\
         Console input, text, time, files, SQLite and windows are in the\n\
         standard library: add e.g. `shuck porch` to a program to use them.\n",
        argv0
    ));
}

fn run(args: Vec<String>) -> i32 {
    let argv0 = args.first().cloned().unwrap_or_else(|| "cob_interp".into());
    if check_hidden_version_flag(&args) {
        return 0;
    }
    if check_hidden_license_flag(&args) {
        return 0;
    }
    #[cfg(feature = "enterprise")]
    if cob_drm::require("cob_interp") != 0 {
        return 1;
    }

    let mut src_path: Option<String> = None;
    let mut use_cache = true;
    let mut no_gc = false;
    let mut enter_repl = false;
    for a in args.iter().skip(1) {
        if a == "--no-cache" {
            use_cache = false;
        } else if a == FLAG_NO_GC {
            no_gc = true;
        } else if a == "--repl" || a == "-i" || a == "--interactive" {
            enter_repl = true;
        } else if a.starts_with('-') {
            ewrite(&format!("[cob_interp] error: unrecognized flag '{}'\n", a));
            print_usage(&argv0);
            return 1;
        } else if src_path.is_none() {
            src_path = Some(a.clone());
        } else {
            ewrite(&format!("[cob_interp] error: unexpected extra argument '{}'\n", a));
            print_usage(&argv0);
            return 1;
        }
    }

    let mut sess = Session::new(no_gc);

    let Some(src_path) = src_path else {
        sess.repl(true);
        return 0;
    };
    let Some(cache_path) = make_cache_path(&src_path) else {
        ewrite(&format!("[cob_interp] error: expected a {} file, got '{}'\n", SOURCE_EXTENSION, src_path));
        return 1;
    };
    let Some(raw) = fsutil::read_all(&src_path) else {
        ewrite(&format!("[cob_interp] error: could not read source file '{}'\n", src_path));
        return 1;
    };
    let source = fsutil::as_c_text(raw);

    if cob_core::parse::strip_makecache_directive(&source).is_some() {
        use_cache = false;
    }
    if enter_repl {
        use_cache = false; // the REPL needs the live program, not a cached copy
    }

    if use_cache && fsutil::exists(&cache_path) {
        match cache::load(&cache_path) {
            Ok(prog) => {
                if !no_gc && cob_core::ast::program_uses_harvest_or_trash(&prog) {
                    ewrite("[cob_interp] error: this program uses harvest()/trash() -- rerun with --no-gc\n");
                    return 1;
                }
                sess.recipes_register(&prog);
                sess.execute(&prog);
                return 0;
            }
            Err(()) => {
                ewrite(&format!("[cob_interp] note: cache '{}' unreadable, reparsing source\n", cache_path));
            }
        }
    }

    let Ok((prog, _)) = sess.parse_source(&source) else { return 1 };

    // recipes are hoisted: validate and register them, and check every call,
    // BEFORE anything runs -- so a typo fails up front, not halfway
    if sess.recipes_validate(&prog, false).is_err() || sess.check_calls(&prog, false).is_err() {
        return 1;
    }
    sess.recipes_register(&prog);

    if !no_gc && cob_core::ast::program_uses_harvest_or_trash(&prog) {
        ewrite("[cob_interp] error: this program uses harvest()/trash() -- rerun with --no-gc\n");
        return 1;
    }

    sess.execute(&prog);
    sess.ret = cob_core::Value::Int(0);

    if use_cache {
        cache::write(&cache_path, &prog);
    }
    if enter_repl {
        sess.repl(true);
    }
    0
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let code = if cfg!(target_os = "macos") {
        // GLFW (behind the `barn` window module) insists on the main thread on
        // macOS, so run right here; the main thread's 8 MB stack is plenty for
        // the 500-call recursion cap (measured: even 8 MB handles 50,000-deep
        // expression nesting).
        run(args)
    } else {
        // Elsewhere, give the interpreter a generous stack of its own so a
        // pathological program can't overflow the platform default (Windows'
        // main thread gets only 1 MB).
        let handle = std::thread::Builder::new()
            .name("cob".into())
            .stack_size(64 * 1024 * 1024)
            .spawn(move || run(args))
            .expect("could not start the interpreter thread");
        handle.join().unwrap_or(1)
    };
    flush_stdout();
    std::process::exit(code);
}

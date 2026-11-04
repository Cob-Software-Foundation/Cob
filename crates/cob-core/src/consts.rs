//! Project-wide constants and small shared helpers.

use std::path::PathBuf;

pub const PROJECT_NAME: &str = "Cob Language";
pub const PROJECT_CODENAME: &str = "Project Obsidian Falcon";
pub const VERSION_STRING: &str = "0.0.9";
pub const AUTHOR_HANDLE: &str = "Pixel-Pulse";
pub const REPO_URL: &str = "https://github.com/Cob-Software-Foundation/Cob";

pub const LICENSE_NAME: &str = "PolyForm Noncommercial License 1.0.0";
pub const LICENSE_URL: &str = "https://polyformproject.org/licenses/noncommercial/1.0.0";

pub fn version_banner() -> String {
    format!("Cob Language v{}  - Handcrafted by Pixel-Pulse", VERSION_STRING)
}

pub fn license_notice() -> String {
    format!(
        "{} is licensed under the {}.\n\
         Full terms: {}\n\
         Required Notice: Copyright Pixel-Pulse (pixel-pulse.dev)\n\
         This is a noncommercial license: permitted uses are personal,\n\
         educational, charitable, and governmental. See LICENSE.md.\n",
        PROJECT_NAME, LICENSE_NAME, LICENSE_URL
    )
}

/// Hidden `--version` easter egg: prints the banner if the flag is present.
pub fn check_hidden_version_flag(args: &[String]) -> bool {
    if args.iter().skip(1).any(|a| a == "--version") {
        cob_abi::cstdio::write_stdout(format!("{}\n", version_banner()).as_bytes());
        cob_abi::cstdio::flush_stdout();
        return true;
    }
    false
}

/// Documented `--license` flag: prints the short notice if present.
pub fn check_hidden_license_flag(args: &[String]) -> bool {
    if args.iter().skip(1).any(|a| a == "--license") {
        cob_abi::cstdio::write_stdout(license_notice().as_bytes());
        cob_abi::cstdio::flush_stdout();
        return true;
    }
    false
}

// ---- vocabulary -----------------------------------------------------------
pub const KW_WHILE: &str = "while";
pub const KW_SET: &str = "set";
pub const KW_POP: &str = "pop";
pub const KW_SHUCK: &str = "shuck";
pub const KW_HARVEST: &str = "harvest";
pub const KW_TRASH: &str = "trash";
pub const KW_IF: &str = "if";
pub const KW_ELIF: &str = "elif";
pub const KW_ELSE: &str = "else";
pub const KW_RECIPE: &str = "recipe";
pub const KW_SERVE: &str = "serve";
pub const KW_EXTERN: &str = "extern";
pub const KW_FROM: &str = "from";

pub const FLAG_NO_GC: &str = "--no-gc";
pub const CACHE_EXTENSION: &str = ".strawberry";
pub const SOURCE_EXTENSION: &str = ".cob";

pub const STDLIB_ENV: &str = "COB_STDLIB";
pub const STDLIB_SUBDIR: &str = "lib";
pub const FARMER_MODULES_DIR: &str = "cob_modules";
pub const FARMER_DEFAULT_BASE_URL: &str = "https://cpi.cob.pixel-pulse.dev";
pub const FARMER_BASE_URL_ENV: &str = "COB_FARMER_BASE_URL";

/// Deepest chain of recipe calls (recursion included).
pub const MAX_CALL_DEPTH: usize = 500;
pub const MAX_LOOP_ITERATIONS: u64 = 10_000_000;
pub const MAX_EXTERN_ARGS: usize = 255;
pub const MAX_SHUCK_DEPTH: usize = 16;

/// `.strawberry` cache magic. Bump only when the on-disk format changes.
pub const STRAWBERRY_MAGIC: &[u8; 9] = b"COBSTRW10";

#[cfg(windows)]
pub const PATH_SEP: char = '\\';
#[cfg(not(windows))]
pub const PATH_SEP: char = '/';

pub fn os_name() -> &'static str {
    if cfg!(windows) {
        "windows"
    } else if cfg!(target_os = "macos") {
        "darwin"
    } else if cfg!(target_os = "linux") {
        "linux"
    } else {
        "unknown"
    }
}

// ---- standard library location -------------------------------------------

/// Directory holding the stdlib's shared libraries and `.cob` wrappers:
/// `$COB_STDLIB` if set, else `<directory of the running executable>/lib`.
pub fn stdlib_dir() -> Option<PathBuf> {
    if let Some(env) = std::env::var_os(STDLIB_ENV) {
        if !env.is_empty() {
            return Some(PathBuf::from(env));
        }
    }
    Some(exe_path()?.parent()?.join(STDLIB_SUBDIR))
}

/// Path of the running executable, symlinks resolved.
///
/// `current_exe()` reads `/proc/self/exe` on Linux, which a chroot or a minimal
/// sandbox may not have mounted; the original C interpreter had the same
/// weakness. Fall back to `argv[0]`, searched on `$PATH` when it has no slash,
/// so `shuck` and the bundled Zig still find their files there.
pub fn exe_path() -> Option<PathBuf> {
    if let Ok(p) = std::env::current_exe() {
        return Some(p);
    }
    let arg0 = PathBuf::from(std::env::args_os().next()?);
    let resolved = |p: PathBuf| std::fs::canonicalize(&p).unwrap_or(p);
    if arg0.components().count() > 1 || arg0.is_absolute() {
        return Some(resolved(arg0));
    }
    let path = std::env::var_os("PATH")?;
    std::env::split_paths(&path).map(|d| d.join(&arg0)).find(|c| c.is_file()).map(resolved)
}

/// Full path of stdlib module `name`'s shared library
/// (`lib<n>.so` / `lib<n>.dylib` / `<n>.dll`).
pub fn stdlib_lib_path(name: &str) -> Option<PathBuf> {
    let dir = stdlib_dir()?;
    let file = if cfg!(windows) {
        format!("{}.dll", name)
    } else if cfg!(target_os = "macos") {
        format!("lib{}.dylib", name)
    } else {
        format!("lib{}.so", name)
    };
    Some(dir.join(file))
}

/// `myprogram.cob` -> `myprogram.strawberry`.
pub fn make_cache_path(src: &str) -> Option<String> {
    let stem = src.strip_suffix(SOURCE_EXTENSION)?;
    Some(format!("{}{}", stem, CACHE_EXTENSION))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn exe_path_finds_this_test_binary() {
        let p = exe_path().expect("a running process has an executable");
        assert!(p.is_absolute() || p.exists());
    }
}

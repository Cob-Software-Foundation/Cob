// barn sits on raylib (built by its own vendored Makefile, position-independent
// so it can live in a shared library) and raygui (a header-only C library,
// compiled via the one-file shim raygui_impl.c).
use std::path::Path;
use std::process::Command;

fn main() {
    let raylib = Path::new("../../vendor/raylib/src");
    let raygui = Path::new("../../vendor/raygui/src");
    println!("cargo:rerun-if-changed=raygui_impl.c");
    println!("cargo:rerun-if-changed=build.rs");

    let target = std::env::var("TARGET").unwrap_or_default();
    let windows = target.contains("windows");
    let mac = target.contains("apple");

    let lib = raylib.join("libraylib.a");
    if !lib.exists() {
        // Build raylib with the SAME compiler and archiver Cargo uses for this
        // target (CC_<target>, AR_<target>, ...). A bare `cc`/`ar` would pick the
        // host's -- on a cross build (say 32-bit Windows from a 64-bit mingw) that
        // produces a library of the wrong architecture.
        let tool = cc::Build::new().get_compiler();
        let cc_path = tool.path().to_string_lossy().into_owned();
        let ar = cc::Build::new().get_archiver();
        let ar_path = ar.get_program().to_string_lossy().into_owned();
        // keep only the flags that choose the architecture (-m32, --target=...)
        let arch_flags: Vec<String> = tool
            .args()
            .iter()
            .map(|a| a.to_string_lossy().into_owned())
            .filter(|a| a.starts_with("-m") || a.starts_with("--target"))
            .collect();
        let mut cflags = arch_flags;
        if !windows {
            cflags.insert(0, "-fPIC".to_string());
        }
        let mut cmd = Command::new("make");
        cmd.arg("-C")
            .arg(raylib)
            .arg("PLATFORM=PLATFORM_DESKTOP")
            .arg(format!("CC={}", cc_path))
            .arg(format!("AR={}", ar_path));
        if !cflags.is_empty() {
            cmd.arg(format!("CUSTOM_CFLAGS={}", cflags.join(" ")));
        }
        let ok = cmd.status().map(|s| s.success()).unwrap_or(false);
        assert!(ok && lib.exists(), "building vendor/raylib failed (X11 + OpenGL dev headers installed?)");
    }

    cc::Build::new()
        .file("raygui_impl.c")
        .include(raylib)
        .include(raygui)
        .pic(true)
        .warnings(false)
        .flag_if_supported("-w") // raygui.h is third-party; its warnings aren't ours
        .compile("raygui_impl");

    println!("cargo:rustc-link-search=native={}", raylib.canonicalize().unwrap().display());
    println!("cargo:rustc-link-lib=static=raylib");
    if windows {
        for l in ["opengl32", "gdi32", "winmm", "shell32"] {
            println!("cargo:rustc-link-lib={}", l);
        }
    } else if mac {
        for f in ["OpenGL", "Cocoa", "IOKit", "CoreAudio", "CoreVideo"] {
            println!("cargo:rustc-link-lib=framework={}", f);
        }
    } else {
        for l in ["GL", "m", "pthread", "dl", "rt", "X11"] {
            println!("cargo:rustc-link-lib={}", l);
        }
    }
}

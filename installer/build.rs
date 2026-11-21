// 1. Puts the payload where `installer.rs` can include it.
// 2. On Windows targets, embeds the icon, version info and a manifest.
//
// Release builds REQUIRE payload.zip (a setup program with nothing inside is a
// bug). Debug builds and `cargo test` fall back to an empty archive so the
// wizard can be worked on without building Cob first.
use std::{env, fs, path::PathBuf};

/// Windows only: the file icon (Explorer, taskbar), the Properties > Details
/// version info, and a manifest.
///
/// The manifest is not decoration. Windows treats any executable whose name
/// contains "setup" or "install" as a legacy installer and demands elevation
/// (a UAC prompt) unless it declares `asInvoker`. This wizard is portable and
/// must never ask for administrator rights.
fn embed_windows_resources() {
    if env::var("CARGO_CFG_TARGET_OS").as_deref() != Ok("windows") {
        return;
    }
    let manifest_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    let out_dir = PathBuf::from(env::var("OUT_DIR").unwrap());
    println!("cargo:rerun-if-changed=assets/Cob.ico");

    let num = |k: &str| env::var(k).unwrap_or_else(|_| "0".into());
    let (major, minor, patch) = (num("CARGO_PKG_VERSION_MAJOR"), num("CARGO_PKG_VERSION_MINOR"), num("CARGO_PKG_VERSION_PATCH"));
    let version = env::var("CARGO_PKG_VERSION").unwrap();

    let manifest = out_dir.join("cob.manifest");
    fs::write(
        &manifest,
        r#"<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
  <trustInfo xmlns="urn:schemas-microsoft-com:asm.v3">
    <security>
      <requestedPrivileges>
        <requestedExecutionLevel level="asInvoker" uiAccess="false"/>
      </requestedPrivileges>
    </security>
  </trustInfo>
  <compatibility xmlns="urn:schemas-microsoft-com:compatibility.v1">
    <application>
      <supportedOS Id="{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}"/>
    </application>
  </compatibility>
</assembly>
"#,
    )
    .unwrap();

    // forward slashes: both rc.exe and windres accept them, and they need no escaping
    let fwd = |p: PathBuf| p.to_string_lossy().replace('\\', "/");
    let rc = out_dir.join("cob.rc");
    fs::write(
        &rc,
        format!(
            r#"1 ICON "{ico}"
1 24 "{manifest}"
1 VERSIONINFO
 FILEVERSION {major},{minor},{patch},0
 PRODUCTVERSION {major},{minor},{patch},0
 FILEFLAGSMASK 0x3fL
 FILEFLAGS 0x0L
 FILEOS 0x40004L
 FILETYPE 0x1L
 FILESUBTYPE 0x0L
BEGIN
  BLOCK "StringFileInfo"
  BEGIN
    BLOCK "040904b0"
    BEGIN
      VALUE "CompanyName", "Cob Software Foundation"
      VALUE "FileDescription", "Cob Setup"
      VALUE "FileVersion", "{version}"
      VALUE "InternalName", "cob_installer"
      VALUE "OriginalFilename", "cob_installer.exe"
      VALUE "ProductName", "Cob"
      VALUE "ProductVersion", "{version}"
    END
  END
  BLOCK "VarFileInfo"
  BEGIN
    VALUE "Translation", 0x409, 1200
  END
END
"#,
            ico = fwd(manifest_dir.join("assets").join("Cob.ico")),
            manifest = fwd(manifest),
        ),
    )
    .unwrap();

    // We supply our own manifest, so stop MSVC's linker adding a second one.
    if env::var("CARGO_CFG_TARGET_ENV").as_deref() == Ok("msvc") {
        println!("cargo:rustc-link-arg-bins=/MANIFEST:NO");
    }
    embed_resource::compile(&rc, embed_resource::NONE)
        .manifest_required()
        .expect("could not embed the Windows resources (icon, version info, manifest)");
}

fn main() {
    embed_windows_resources();
    println!("cargo:rerun-if-changed=payload.zip");
    println!("cargo:rerun-if-changed=build.rs");

    let out = PathBuf::from(env::var("OUT_DIR").unwrap()).join("payload.zip");
    let src = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap()).join("payload.zip");

    if src.is_file() {
        fs::copy(&src, &out).expect("could not copy payload.zip");
    } else if env::var("PROFILE").as_deref() == Ok("release") {
        panic!("payload.zip not found next to Cargo.toml -- run `make payload` (or place your own) before a release build");
    } else {
        println!("cargo:warning=payload.zip not found; embedding an EMPTY archive (debug build)");
        // the 22-byte end-of-central-directory record of a zip with no entries
        let mut empty = vec![0x50, 0x4b, 0x05, 0x06];
        empty.extend_from_slice(&[0u8; 18]);
        fs::write(&out, empty).unwrap();
    }
}

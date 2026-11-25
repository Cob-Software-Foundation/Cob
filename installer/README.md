# Cob setup wizard

A self-extracting installer for Cob: one executable that unpacks the interpreter,
the compiler and the standard library into a folder you choose. It is portable --
no registry, no `PATH` edits, no administrator rights.

It is its own cargo project and deliberately **not** part of the main Cob workspace:
the GUI (egui) needs a newer Rust than the workspace's 1.75 MSRV, and the language
itself must never depend on a GUI stack.

## Build

```
make payload     # builds ../bin and zips it into payload.zip (the release layout: `cob`, `popcorn_comp`, `lib/`, licences)
make             # release installer -> dist/Cob-<version>-Setup-<os>
make test        # extraction tests; needs no payload
make run         # build and launch it
```

## Platforms

| Platform | What you ship | How it is made |
|---|---|---|
| Windows | `Cob-<version>-Setup-windows-amd64.exe` | `cargo build --release`; `build.rs` embeds the icon, version info and manifest |
| macOS | `Cob-<version>-Setup-macos-<arm64\|amd64>.dmg` containing `Cob Setup.app` | `cargo build --release`, then `bash tools/package_macos.sh` (run on a Mac) |
| Linux | `Cob-<version>-Setup-linux` (a bare executable) | `cargo build --release` |

`make` does the right one for the machine it runs on. Each installer embeds the Cob build for
its own platform *and architecture*, so Apple Silicon and Intel get separate `.dmg` files.

**Windows.** The `.exe` carries the Cob icon (all sizes Explorer uses), version info (Properties >
Details) and a manifest declaring `asInvoker`. The manifest matters: without it Windows treats any
program with "setup" in its name as a legacy installer and shows a UAC prompt, which a portable
installer must never do. It is unsigned, so SmartScreen will warn once ("More info" > "Run anyway").

**macOS.** `package_macos.sh` wraps the binary in `Cob Setup.app` (icon, `Info.plist`, minimum
macOS 11) and builds the `.dmg`. The app is signed *ad hoc* (`codesign -s -`), which Apple Silicon
requires; it is not Developer ID signed or notarized, so Gatekeeper asks users to approve it once
(right-click > Open). Add your Developer ID and `notarytool` steps to the script when you have a
certificate. Files a program writes are not quarantined unless it opts in, so the extracted `cob` should not
trigger a second Gatekeeper prompt (expected, but not verified on a Mac).

**CI.** The `installer` job in `.github/workflows/build.yml` builds all three on native runners,
using each platform's release archive from the `build` job as the payload (the wizard strips the
archive's `cob-<os>-<arch>/` wrapper folder). The files are uploaded as `artifact-installer-*`, with
hashes and provenance, and published by the `release` job.

## Payload

`payload.zip` is embedded in the executable at compile time. A **release** build fails
if it is missing; a debug build or `cargo test` embeds an empty archive instead, so the
wizard can be worked on without building Cob first.

## Version and branding

* The version everywhere in the UI is `CARGO_PKG_VERSION`: bumping `Cargo.toml` (keep it in
  step with `../VERSION`) is the only edit a release needs.
* The window icon, the title logo and the sidebar banner are in `assets/` and are generated
  from the corn logo, `assets/source/cob-logo.png`, by `make assets` (`tools/make_assets.py`,
  needs Pillow). The script cuts the corn out of its gradient backdrop, so a new logo of a
  similar kind (dark outline on a smooth background) works as a drop-in replacement; a
  higher-resolution original gives a sharper banner. Or drop in your own PNGs of the same
  names and sizes (`icon.png` 256x256, `logo.png` 128x128, `sidebar.png` 330x770 -- drawn at 2x).
* The words on the banner (name, version, platform) are drawn live, not baked into the image.

## Behaviour

* Extraction runs on a worker thread; the progress page shows real `done / total` counts.
* "Overwrite existing files" is honoured: when off, files already present are kept and reported.
* An archive entry whose path would escape the destination (`../x`, absolute) aborts the
  extraction before anything is written outside the folder.
* A single wrapper folder around the whole archive (the release archives are
  `cob-windows-amd64/...`) is stripped, so a release zip works as `payload.zip` unchanged.
* Unix permission bits are kept, so `cob` stays executable.
* "Open Folder" uses `explorer` / `open` / `xdg-open` for the platform.

## What has been verified

* **Linux:** built, and driven end to end under a virtual display (every page, a real extraction,
  then the extracted program run).
* **Windows:** cross-compiled from Linux to a PE32+ x86-64 `.exe`; the embedded icon group (7 sizes),
  version info (0.0.10) and `asInvoker` manifest were read back out of it. **Not run on Windows.**
* **macOS:** the packaging script was run on Linux with stubbed `codesign`/`hdiutil`: the bundle
  layout, executable bit, icon and `Info.plist` (parsed with a real plist parser) are right.
  **Not built or run on a Mac.**
* The extraction logic (zip-slip refusal, overwrite, wrapper stripping, permissions) has unit tests
  that run on every platform with `cargo test`.

The first CI run on the Windows and macOS runners is the real test of those two paths.

Not included: code signing for either platform (see above) and a Windows ARM64 or 32-bit installer.

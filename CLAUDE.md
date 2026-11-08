# CLAUDE.md

Guidance for working on **Cob**, a small interpreted language (`recipe`/`serve`,
`pop`, `shuck`) with an interpreter, a native compiler, a package manager and a
standard library. The whole toolchain is Rust (stable, MSRV **1.75**). Two C
libraries are vendored and left alone: SQLite (used by `pantry`) and
raylib/raygui (used by `barn`).

## Build and test

```sh
make                 # cob_interp, popcorn_comp, farmer + stdlib -> bin/, bin/lib/
                     #   (also the *_enterprise tools when keys/drm_pubkeys.txt exists)
make test            # tools/tests/run_tests.sh, then `cargo test --workspace`
make stdlib-barn     # windows module; needs X11 + OpenGL dev headers (opt-in)
make play            # browser build -> docs/play/cob_interp.wasm
make wasm-test       # build the browser module, check it against native under Node
make TARGET=<triple> # cross-compile (rustup target add <triple> first)
```

`make test` must end with `17 passed, 0 failed` and exit 0. Run the interpreter
from the repo with `bin/cob_interp file.cob` (add `--no-cache` while testing, or
a stale `.strawberry` cache can hide your change).

## Layout

| Path | Role |
|---|---|
| `crates/cob-abi` | The native-extension ABI (`CobValue`, mirrors `include/cob.h`), C-stdio access, `strtol` semantics, helpers shared by every module |
| `crates/cob-core` | Parser, call checker, evaluator, `.strawberry` cache, REPL (`Session` holds all state) |
| `crates/cob_interp` | The interpreter binary |
| `crates/popcorn_comp` | AST -> C transpiler, then spawns a real C compiler (Zig by default) |
| `crates/farmer` | Package manager: `farmer harvest <pkg>` |
| `crates/cob-drm`, `crates/cob_sign` | Enterprise license verification / signing |
| `crates/cob-wasm` | The interpreter for the browser. **Not** in the workspace (own `[workspace]`) |
| `stdlib-rs/*` | One crate per module: `porch cornmath kernels almanac granary pantry`, and `barn` (**not** in the workspace) |
| `stdlib/*.cob` | The tiny `extern` wrappers that `shuck <module>` loads |
| `include/cob.h` | The C ABI for people writing extensions in C. Keep it in sync with `cob-abi` |
| `tools/tests`, `tools/stress` | Regression tests; stress tests (`barn` needs Xvfb) |
| `docs/` | The website; `docs/play/cob_interp.js` is hand-written glue (committed), the `.wasm` is generated |
| `keys/drm_pubkeys.txt` | PUBLIC license keys only |

Standard-library modules are `cdylib`s exporting `extern "C"` functions with the
`CobValue` ABI and loaded with `dlopen`. `porch`, `cornmath`, `kernels` and
`almanac` are also `rlib`s with an `EXPORTS` table, because the browser build
links them in statically (it cannot `dlopen`).

## Invariants -- do not break these

- **All console output goes through `cob_abi::cstdio`**, never `std::io::stdout()`.
  The interpreter, the stdlib modules, C extensions and programs built by
  `popcorn_comp` must share one set of C stdio buffers or output reorders. On
  Windows `cstdio` resolves the C runtime at run time (UCRT first) so `fwrite`
  and `stdout` always come from the same DLL.
- **Error and warning text is part of the language.** The tests compare it
  exactly (`tools/tests/errors.sh`). Reword nothing casually.
- **Cob strings are bytes with no NUL**, not `String`. Numbers are `i64` with
  *wrapping* arithmetic. Truncate source at the first NUL, as the C version did.
- **Byte compatibility:** the `.strawberry` cache format (magic `COBSTRW10`), the
  C that `popcorn_comp` emits, and the extension ABI are unchanged from the
  original C implementation. Caches written by either must read in the other.
- **The license signature is EdDSA with Blake2b-512 (Monocypher's), not standard
  Ed25519.** `cob-drm`'s known-answer test pins a signature made by the C tool;
  if it fails, existing licenses stop verifying.
- The free binaries contain no license code: it sits behind the `enterprise`
  Cargo feature and is built into a separate target dir by `make enterprise`.

## Platform behaviour worth knowing

- The interpreter runs on a 64 MB thread (deep recursion), **except on macOS**,
  where GLFW (`barn`) requires the main thread.
- It finds its `lib/` next to the real executable (`consts::exe_path`), falling
  back to `argv[0]` when `/proc/self/exe` is missing (chroots, sandboxes).
  `COB_STDLIB` overrides.
- Other environment variables: `COB_LICENSE`, `COB_FARMER_BASE_URL`, and for
  `popcorn_comp` `CobCC`, `CobOS`, `CobArch`.
- The browser build has 64-bit numbers too (`CobLong` is `i64` on wasm32), and
  stubs any `extern` library it can't link, with one notice per library.
- Dependencies are pinned for an old toolchain: `zeroize = "=1.7.0"` (newer
  releases need Cargo 1.85+) and `cc = "=1.2.30"`. Do not lower `cc` below 1.2:
  older releases hand clang the Rust name `aarch64-pc-windows-gnullvm`, which
  clang rejects (this broke the Windows arm64 build). Commit all three lockfiles
  (root, `stdlib-rs/barn`, `crates/cob-wasm`) and don't run a blanket
  `cargo update`.

## Enterprise licenses

`cob_sign sign` writes `bind=machine` by default. The first run of an enterprise
tool records a salted hash of the OS machine UUID **below** the signature
(`bound_machine=`); the signature covers only the part above `---`. That record
is unsigned, so deleting it re-binds the license: it deters casual copying, not
a determined user. `--machine <hash>` signs the machine ID into the payload,
which can't be edited away. The private key (`cob_private.key`, `*.key`,
`*.lic`) is git-ignored; never commit it. Details: `ENTERPRISE.md`.

## CI

`.github/workflows/build.yml` builds nine targets. Its shell steps `source
tools/ci/portable.sh` (`sha256`, `mkzip`, `unzip_to`) because runners differ:
macOS has no `sha256sum`, Windows has no `zip`. When you edit a workflow, run
`actionlint` (it uses ShellCheck on the embedded scripts) and compare against
the existing findings (about six are old: unknown runner label, style notes).
The Linux jobs have run green; the Windows and macOS fixes in `portable.sh`,
the MSYS2 32-bit toolchain and the llvm-mingw URL had **not** yet run on a
runner when this file was written.

## Conventions and gotchas

- Edit `crates/` and `stdlib-rs/`. Don't edit `vendor/` (third-party C).
- Tcl/Tk was removed on purpose; don't reintroduce `tassel` or its mappings.
- `unsafe` belongs at FFI boundaries (`cob-abi`, the stdlib modules, the
  evaluator's extern call). Keep it out of the parser and checker.
- Recipes are `Rc<Recipe>`; the registry shares them with the AST.
- When changing the language, add a case to `tools/tests/errors.sh` or a
  `parity_*.cob` fixture; the wasm test diffs the browser build against native,
  so a fixture covers both.
- `make` recipes run under `/bin/sh` (dash on Ubuntu): no brace expansion or
  bashisms in the Makefile.
- On a toolchain without the wasm32 standard library (some sandboxes), build the
  browser module with `RUSTC_BOOTSTRAP=1 WASM_CARGO_FLAGS=-Zbuild-std=std,panic_abort`
  and an LLD matching rustc's LLVM. On a normal toolchain
  `rustup target add wasm32-unknown-unknown` is enough.
- Third-party crate licenses are listed in `legal/RUST_DEPENDENCIES.md`;
  regenerate it from `cargo metadata` when `Cargo.lock` changes.
- The project is PolyForm Noncommercial (`LICENSE.md`); keep the required notice.

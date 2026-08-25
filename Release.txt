Cob Language v0.0.2 -- Project Obsidian Falcon

A corn-themed hybrid language with Python-style indentation, an
interpreter, a native compiler, and a package manager.

WHAT'S NEW IN v0.0.2
---------------------
- while <condition>: now really loops, with real nested block bodies
  and a safety cap against runaway/infinite loops.
- set <name> = <expr> supports integer variables and arithmetic
  (+ - * /, with standard precedence) and comparisons
  (== != < > <= >=).
- shuck <library_name> loads <library_name>.cob (checking the current
  directory, then cob_modules/<library_name>/<library_name>.cob -- the
  layout `farmer harvest` installs into) and splices it into your
  program. Circular and too-deep shucks are rejected with a clear
  error instead of hanging or crashing.
- harvest(<bytes>) / trash(<variable>) are real now: harvest allocates
  raw memory and returns an opaque handle, trash frees it. Both are
  locked behind the --no-gc flag -- a program using them without it is
  refused outright, not silently ignored.
- A .cob file whose literal first line is `_MakeCache = False`
  (whitespace/case-insensitive) disables the .strawberry fast-boot
  cache entirely for that file.
- popcorn_comp is a real native compiler now. It reads a .strawberry
  file, transpiles it to plain C, and compiles that to a standalone
  native executable using TCC's own compiler source statically linked
  directly into popcorn_comp -- no external compiler, no dynamic
  linking to a system TCC install. CobOS/CobArch environment variables
  are validated against what this specific popcorn_comp binary was
  built for; cross-targeting a different OS/arch requires a
  popcorn_comp built for that target (see .github/workflows/build.yml).
  Currently built for native Linux amd64 in CI; other targets need a
  matching TCC runtime (libtcc1.a) built for them first.
- farmer is the package manager. `farmer harvest <package>` fetches
  metadata from a static JSON API (hosted free on GitHub Pages),
  verifies the package name matches what was requested, downloads the
  package zip, verifies its sha256 if one was provided, and unzips it
  into cob_modules/<package>/ -- ready for `shuck <package>`. All
  values that reach a shell command line are validated and quoted
  before use.
- A real, working example package ("greeter") and static registry are
  published under docs/ and served via GitHub Pages.

SCOPE NOTES
-----------
- popcorn_comp's cross-compilation story is honest, not aspirational:
  a single statically-linked libtcc build only targets the one
  platform it was compiled for. True cross-target support means
  building separate popcorn_comp binaries per target, each with a
  matching TCC runtime -- that's still a work in progress beyond
  native Linux amd64.
- cob_interp and farmer build for all 10 target platforms
  (Windows/Linux/macOS x amd64/386/arm64/arm as applicable).

Binaries are provided per-platform in this release. Verify downloads
against checksums.txt. A popcorn_comp release also ships a runtime/
folder (libtcc1.a + TCC's bundled headers) alongside the binary --
keep them together, or point popcorn_comp at another copy via the
POPCORN_TCC_RUNTIME_DIR environment variable.

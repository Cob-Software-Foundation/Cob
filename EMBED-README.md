# Cob Embeddable Package

This is a minimal, no-installer, extract-and-run package meant to be
dropped *inside another application's own distribution* so that
application can run `.cob` scripts, the same idea as
[Python's embeddable package](https://docs.python.org/3/using/windows.html#the-embeddable-package)
-- just extract it into a folder and go, no registry entries, no
system-wide install, nothing else to set up.

It is **not** the Cob dev toolchain. If you're developing *in* Cob
(writing/running/compiling `.cob` files yourself), use the regular
release archive instead, which also includes `farmer` (the package
manager) and `popcorn_comp` (the native compiler) alongside `cob`.
This package exists for the other case: your own application wants to
embed a Cob *runtime* to execute scripts on its behalf.

## What's in here

```
cob(.exe)          The interpreter. cob_interp_full under the hood --
                    SQLite (sql_open/sql_exec/sql_query/sql_close) and
                    a native window with widgets (window_open/label/
                    wait/close/button/slider/textbox) both built in.
                    No Tcl/Tk -- see "Why cob_interp_full and not one
                    of the other builds?" below.
LICENSE.md          Cob's own license (PolyForm Noncommercial 1.0.0)
                    -- read "License" below before embedding this in
                    anything. This file must travel with the package
                    wherever it goes; don't strip it out.
legal/              Third-party license text for what's actually
                    compiled into cob(.exe): raylib and raygui, both
                    zlib License. (SQLite is public domain -- no
                    separate license file exists for it. Tcl/Tk and
                    miniz aren't in this binary at all, so their
                    license terms don't apply here the way they would
                    to the full release archive.)
VERSION              Plain text, e.g. "0.0.5" -- check this before
                    assuming a `.strawberry` cache or feature set.
README.md           This file.
```

## Embedding contract

```
cob <script.cob> [--no-cache] [--no-gc]
```

- **Exit code**: `0` on success, `1` on any error (bad usage, a file
  that doesn't exist, a syntax error in the script, etc). There's no
  finer-grained exit code than that -- if your host application needs
  to distinguish failure reasons, parse stderr.
- **stdout**: exactly what the `.cob` script itself printed via
  `pop()`, nothing else. Safe to capture and treat as the script's own
  output.
- **stderr**: `[cob_interp] warning: ...` lines -- both real runtime
  warnings (a bad SQL handle, a widget on a closed window, etc.) and,
  in this specific binary, `window_*`/`sql_*` never print a
  "compiled without X support" stub warning, because `cob_interp_full`
  has both built in. If you ever swap in a different build variant,
  expect those stub warnings to reappear on stderr for the calls that
  build doesn't support -- they're deliberately non-fatal warnings,
  not crashes.
- **`--no-cache`**: skips the `.strawberry` fast-boot cache for this
  run. A script can also disable caching for itself by making
  `_MakeCache = False` its literal first line -- prefer that over the
  flag if you don't control how your host application invokes `cob`.
- **`--no-gc`**: required before a script's `harvest()`/`trash()`
  calls will do anything; without it they're refused outright, not
  silently ignored.
- **`--version`**: prints a one-line version banner and exits 0.
  Intentionally undocumented in `cob`'s own `--help` output -- an
  easter egg for maintainers, mentioned here because an embedding
  application is exactly the kind of caller that might want to check
  this programmatically.
- A real, native OS window opens for any script that calls
  `window_open()` -- if your host application invokes `cob` in a
  headless/server context with no display available, `window_open()`
  fails cleanly (prints a warning, returns `0`) rather than crashing,
  the same way it does today in CI (see the project's `build.yml` for
  the Xvfb-based pattern this was verified against).

## Why `cob_interp_full` and not one of the other builds?

Cob's interpreter has four build variants (`cob_interp`,
`cob_interp_window`, `cob_interp_full`, `cob_interp_db` -- see the
main README). This package standardizes on `cob_interp_full`
specifically because it's the one built with zero autoconf/configure
anywhere in its own dependency chain (SQLite's single-file
amalgamation, raylib/raygui's plain Makefiles) -- the same property
that makes it the default `make` target for the toolchain as a whole.
`cob_interp_db` (Tcl/Tk) was deliberately left out of this embeddable
package: it needs `TCL_LIBRARY` pointed at a real Tcl standard library
directory to work, which doesn't fit "extract one folder and run."

## What's not in here, on purpose

- **`farmer`** -- the package manager. An embedding application
  controls what `.cob` files it ships and runs; it doesn't need its
  end users pulling packages from Cob's own registry at runtime.
- **`popcorn_comp`** -- the native compiler. This package is a
  runtime, not a build tool.
- **`docs/`** -- the language reference site. Not needed to *run* a
  script, only to *write* one.

If your use case needs any of those, use the regular release archive
instead of this package.

## License -- read this before embedding Cob in anything

Cob is licensed under the **PolyForm Noncommercial License 1.0.0**
(full text in `LICENSE.md`, also at
<https://polyformproject.org/licenses/noncommercial/1.0.0>). The short
version, not a substitute for actually reading `LICENSE.md`:

- **Noncommercial use is permitted.** Personal projects, research,
  education, hobby projects, and use by charitable/educational/public
  research/government institutions are all fine regardless of funding
  source.
- **Commercial use is not covered by this license.** If the
  application you want to embed `cob` into is commercial (sold,
  monetized, used to provide a paid service, etc.), redistributing
  this package as part of it is *not* permitted under these terms.
  That needs a separate license from the Cob Software Foundation --
  this package doesn't grant one, and nothing in this README changes
  that.
- **Required Notice.** Anyone who receives a copy of this package from
  you must also receive a copy of the license terms (i.e. keep
  `LICENSE.md` in the package) and this notice:

  > Required Notice: Copyright Pixel-Pulse (pixel-pulse.duckdns.org)

- This package also carries **raylib** and **raygui** (both zlib
  License, text in `legal/`) as statically-linked third-party
  components of `cob(.exe)` itself -- their license terms apply
  alongside Cob's own, not instead of it.

None of the above is legal advice; if you're not sure whether your use
case qualifies, ask the Cob Software Foundation before shipping.

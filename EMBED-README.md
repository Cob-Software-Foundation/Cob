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
cob(.exe)          The interpreter (the core language only; `cob_interp`
                    under the hood -- the `make embed` target builds it
                    under the name cob_interp_full for historical
                    reasons).
lib/                The STANDARD LIBRARY: one shared library per module
                    (lib<name>.so / lib<name>.dylib / <name>.dll) plus
                    its <name>.cob wrapper. `shuck porch`, `shuck pantry`
                    and the rest load from here. It MUST stay next to
                    cob(.exe) as `lib/` (or set the COB_STDLIB environment
                    variable to wherever you put it) -- without it every
                    `shuck <module>` fails with a "could not load
                    library" warning. Modules: porch (console input),
                    cornmath, kernels (text), almanac (time), granary
                    (files), pantry (SQLite) and barn (a native window
                    with widgets). Not tassel (Tcl/Tk) -- see "Why no
                    tassel?" below.
LICENSE.md          Cob's own license (PolyForm Noncommercial 1.0.0)
                    -- read "License" below before embedding this in
                    anything. This file must travel with the package
                    wherever it goes; don't strip it out.
legal/              Third-party license text for what's actually
                    compiled into the library modules: raylib and raygui
                    (inside barn), both zlib License. (SQLite, inside
                    pantry, is public domain -- no separate license file
                    exists for it. Tcl/Tk and miniz aren't in this
                    package at all, so their license terms don't apply
                    here the way they would to the full release archive.)
VERSION              Plain text, e.g. "0.0.9" -- check this before
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
- **stderr**: `[cob_interp] warning: ...` lines for the interpreter
  itself, and `[<module>] warning: ...` lines (`[pantry] warning: ...`,
  `[barn] warning: ...`, `[porch] warning: ...`) for the standard
  library modules -- a bad SQL handle, a widget on a closed window, a
  non-numeric answer to `porch_ask_int()`, and so on. All are
  deliberately non-fatal warnings, not crashes. If a module's library
  is missing from `lib/`, you get ONE warning naming it (`could not
  load library 'pantry' ...`) and the module's functions return 0 / "".
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
  `barn_open()` -- if your host application invokes `cob` in a
  headless/server context with no display available, `barn_open()`
  fails cleanly (prints a warning, returns `0`) rather than crashing.
- **Console input** is `porch_ask()` / `porch_ask_int()` (`shuck porch`),
  reading the process's standard input -- so a host that wants to answer
  a script's questions writes to the child's stdin.
- **No REPL, unless you ask for one**: `cob` with no script starts the
  interactive REPL. An embedding host should always pass a script path.

## Why no tassel?

Before 0.0.9 the interpreter had several build variants with different
features compiled in. Now there is one interpreter, and features are
standard library modules in `lib/`. This package ships the ones that
build with zero autoconf/configure anywhere in their dependency chain
(SQLite's single-file amalgamation, raylib/raygui's plain Makefiles).
`tassel` (Tcl/Tk) was deliberately left out: it needs `TCL_LIBRARY`
pointed at a real Tcl standard library directory to work, which doesn't
fit "extract one folder and run."

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

  > Required Notice: Copyright Pixel-Pulse (pixel-pulse.dev)

- This package also carries **raylib** and **raygui** (both zlib
  License, text in `legal/`) as statically-linked third-party
  components of the `barn` library in `lib/` -- their license terms
  apply alongside Cob's own, not instead of it.

None of the above is legal advice; if you're not sure whether your use
case qualifies, ask the Cob Software Foundation before shipping.

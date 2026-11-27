# Cob™ (Project Obsidian Falcon)


[![CodeQL - Code Quality](https://github.com/Cob-Software-Foundation/Cob/actions/workflows/github-code-quality/codeql/badge.svg)](https://github.com/Cob-Software-Foundation/Cob/actions/workflows/github-code-quality/codeql)

[![Cob](https://github.com/Cob-Software-Foundation/Cob/actions/workflows/build.yml/badge.svg)](https://github.com/Cob-Software-Foundation/Cob/actions/workflows/build.yml)

[![CodeQL Advanced](https://github.com/Cob-Software-Foundation/Cob/actions/workflows/codeql.yml/badge.svg)](https://github.com/Cob-Software-Foundation/Cob/actions/workflows/codeql.yml)

[![Docker](https://github.com/Cob-Software-Foundation/Cob/actions/workflows/docker.yml/badge.svg)](https://github.com/Cob-Software-Foundation/Cob/actions/workflows/docker.yml)

[![Stress Test](https://github.com/Cob-Software-Foundation/Cob/actions/workflows/ci.yml/badge.svg)](https://github.com/Cob-Software-Foundation/Cob/actions/workflows/ci.yml)

Cob is a corn-themed hybrid programming language combining Python-style indentation with low-level capability. It features a cross-platform interpreter, a native compiler powered by Zig, and its own package manager.

Development, stewardship, and project rights are co-managed by the **Cob Software Foundation™ (CSF)** and **Pixel Pulse**.

## 🚀 Key Features

* **Python-style Indentation:** Clean, block-based visual structure.
* **Functions:** `recipe` / `serve` — define, call, recurse.
* **An interactive REPL:** run `cob_interp` with no file and try things out.
* **A corn-themed standard library of shared libraries:** text, numbers, time, files, console input, SQLite, windows — each a real `.so` / `.dylib` / `.dll`, loaded with `shuck`.
* **Dual Execution:** Run instantly via `cob_interp` or compile to standalone executables with `popcorn_comp`.
* **Built-in Package Ecosystem:** Download, verify, and resolve modules via `farmer`.
* **Explicit Memory Control:** Optional, manual heap management isolated behind strict compiler safety flags.

## 💻 Language Syntax

The language itself is small: variables, arithmetic, `pop`, `while`, `if`, `recipe`, `shuck`. Everything else lives in the standard library (see below) and is brought in with `shuck`.

### Hello World & Variables
```cob
# Print output to the console
pop("Hello, World!")

# Variables use the set keyword
set age = 20
set next_year = age + 1
```

### Loops & Conditions
```cob
set count = 0
while count < 5:
    pop("Harvesting...")
    set count = count + 1

if count == 5:
    pop("five")
elif count > 5:
    pop("more")
else:
    pop("fewer")

# combine tests with and / or / not (and ( ) to group); and / or stop early, like Python
if count > 0 and not count > 9:        # a comment can follow code
    pop("between")
if (count < 3 or count > 4) and count != 99:
    pop("grouped")
```
`not` binds tightest, then `and`, then `or`. A `#` starts a comment, on its own line or after code; inside a string it is just text.

### Functions (`recipe` / `serve`)
```cob
# serve hands a value back, like `return`
recipe add(a, b):
    serve a + b

# ends without serve: the call gives 0
recipe greet(name):
    pop("Hello, " + name + "!")

# recursion works (up to 500 calls deep)
recipe fact(n):
    if n <= 1:
        serve 1
    serve n * fact(n - 1)

pop(add(2, 3))
# a call can stand on a line of its own
greet("Ada")
pop(fact(10))
```
(A `#` starts a comment, on its own line or after code.)
* A recipe's parameters, and every name it `set`s, are **local** to it. Any other name reads the global of that name. A recipe cannot assign a global — it hands results back with `serve`.
* Recipes are defined at the top level, and may be called from above where they are written (they are hoisted). They can call each other freely.
* Arguments are evaluated left to right. A wrong number of arguments, or a call to a name that does not exist, is an error **before the program starts**, with the line number.
* Works the same in `cob_interp` and in programs built by `popcorn_comp`.

### The REPL
```
$ cob_interp
Cob 0.1.1 REPL -- :help for help, :quit or Ctrl-D to leave
cob> 1 + 2
3
cob> set x = 10
cob> recipe square(n):
...      serve n * n
...
cob> square(x)
100
cob> :quit
```
A bare expression prints its value; a line ending in `:` starts a block (finish with a blank line); variables and recipes persist, and typing a recipe again replaces it. Commands: `:help`, `:vars`, `:recipes`, `:load <file>`, `:reset`, `:quit`. `cob_interp file.cob -i` runs a file and then drops into the REPL with everything it defined. The REPL is part of `cob_interp` only (not `popcorn_comp`, and not the browser build).

### Modular Imports (`shuck`)
```cob
# Slices in <name>.cob from the current directory, then cob_modules/<name>/,
# then the standard library directory
shuck greeter
shuck kernels
```

### Manual Memory Management (Requires `--no-gc`)
```cob
# Explicit allocation and freeing
set buffer = harvest(1024)
trash(buffer)
```

## 🌽 The Standard Library

Every module is a **shared library** (`lib<name>.so` on Linux, `lib<name>.dylib` on macOS, `<name>.dll` on Windows) plus a small `<name>.cob` wrapper of `extern` declarations that `shuck <name>` loads. Every module and every function is corn themed. They live in `lib/` next to the tools (or in `$COB_STDLIB`).

| `shuck` | What it is | Functions |
|---|---|---|
| `porch` | talk to the person at the keyboard | `porch_ask(prompt)`, `porch_ask_int(prompt)` |
| `cornmath` | numbers beyond `+ - * /` | `husk_abs`, `ear_max`, `ear_min`, `stalk_pow`, `stalk_sqrt`, `silk_mod`, `scatter_seed`, `scatter_rand` |
| `kernels` | text, kernel by kernel | `kernel_len`, `kernel_upper`, `kernel_lower`, `kernel_slice`, `kernel_find`, `kernel_repeat`, `kernel_trim`, `kernel_replace`, `kernel_char`, `kernel_code`, `kernel_from_code`, `kernel_to_int`, `kernel_starts`, `kernel_ends` |
| `almanac` | the farmer's calendar and clock | `almanac_now`, `almanac_ticks`, `almanac_nap`, `almanac_year`, `almanac_stamp` |
| `granary` | files, stored like grain | `granary_read`, `granary_write`, `granary_append`, `granary_exists`, `granary_remove` |
| `bushel` | lists that grow, a bushel of whatever you pick | `bushel_new`, `bushel_push`, `bushel_get`, `bushel_set`, `bushel_insert`, `bushel_remove`, `bushel_len`, `bushel_find`, `bushel_sort`, `bushel_reverse`, `bushel_extend`, `bushel_copy`, `bushel_join`, `bushel_split`, `bushel_clear`, `bushel_free`, `bushel_at`, `bushel_put`, `bushel_with`, `bushel_range`, `bushel_str` |
| `furrow` | arrays: a fixed-length row, planted once | `furrow_new`, `furrow_get`, `furrow_set`, `furrow_fill`, `furrow_swap`, `furrow_find`, `furrow_sum`, `furrow_sort`, `furrow_copy`, `furrow_len`, `furrow_free` |
| `pantry` | a SQLite database to keep things in | `pantry_open`, `pantry_exec`, `pantry_query`, `pantry_close` |
| `barn` | a window to put things in (raylib + raygui) | `barn_open`, `barn_label`, `barn_wait`, `barn_close`, `barn_button`, `barn_slider`, `barn_textbox` |
| `silo` | the everyday five in one go | `porch` + `cornmath` + `kernels` + `almanac` + `granary` |


### Lists, Python style

With `shuck bushel` (0.1.0) a list reads like a Python list:

```cob
shuck bushel

set fruit = ["pear", "apple", 30]     # a literal; [] is an empty list
pop(fruit[0])                         # pear
pop(fruit[-1])                        # 30: a negative position counts from the end
set fruit[1] = "fig"                  # store into a position
set grid = [[1, 2], [3, 4]]
set grid[1][0] = 33                   # lists inside lists

for item in fruit:                    # go through a list
    pop(item)
for i in bushel_range(3):             # 0, 1, 2
    pop(i)
pop(bushel_str(fruit))                # ["pear", "fig", 30]
```

A list is a reference, as in Python (`set b = a` is the same list; `bushel_copy` makes another), and it is still a handle,
so `pop(fruit)` prints a number and `bushel_str(fruit)` is how to print the items. Slices, `in`, `+` on lists and `len(xs)` are not there yet.

```cob
shuck porch
shuck kernels

set name = porch_ask("Name? ")             # one line from stdin, as a string
set age = porch_ask_int("Age? ")           # parsed as a whole number (0 if it isn't one)
set next = age + 1
pop("Hi " + kernel_upper(name) + ", next year you are " + next)
```
`porch_ask()` returns `""` at end of input; `porch_ask_int()` returns `0` (with a warning on stderr) for a non-numeric line or at end of input. The prompt is optional.

```cob
shuck pantry
set h = pantry_open("scores.db")
set rc = pantry_exec(h, "CREATE TABLE IF NOT EXISTS t (name TEXT)")
set name = pantry_query(h, "SELECT name FROM t LIMIT 1")
pop("got: " + name)
set rc = pantry_close(h)
```

```cob
shuck barn
set w = barn_open("Cob Window Demo")
set rc = barn_label(w, "Hello from Cob!")
set clicked = barn_button(w, "OK")
set volume = barn_slider(w, "Volume", 100)
set name = barn_textbox(w, "Name")
set rc = barn_wait(w, 5)
set rc = barn_close(w)
```
`barn_button` / `barn_slider` / `barn_textbox` each identify their widget by its label: the first call with a new label creates it (auto-stacked), later calls read or update it.

A library file in your own project directory shadows a standard library module of the same name (a `porch.cob` next to your program is found before the real `porch`) — name your own files accordingly.

### Upgrading from 0.0.8
The built-in `input`, SQLite and window keywords are gone; they are standard library functions now. The compiler tells you exactly what to write:

| 0.0.8 | 0.0.9 |
|---|---|
| `input(p)` / `input_int(p)` | `shuck porch` then `porch_ask(p)` / `porch_ask_int(p)` |
| `sql_open` / `sql_exec` / `sql_query` / `sql_close` | `shuck pantry` then `pantry_open` / `pantry_exec` / `pantry_query` / `pantry_close` |
| `shuck cobwindow` + `window_open` / `window_label` / `window_wait` / `window_close` / `window_button` / `window_slider` / `window_textbox` | `shuck barn` then `barn_open` / `barn_label` / `barn_wait` / `barn_close` / `barn_button` / `barn_slider` / `barn_textbox` |

## ⚙️ Toolchain & Ecosystem

* **`cob_interp`**: The interpreter and REPL. Core language only — no vendor libraries and no X11/OpenGL needed. (`cob_interp_full`, `cob_interp_db` and `cob_interp_window` still exist as `make` targets for old scripts; they now build this same interpreter plus the `pantry` / `barn` modules.)
* **`popcorn_comp`**: Native compiler. Transpiles to C and spawns a real compiler (Zig by default, or `$CobCC`/`--cc`) to produce a standalone binary — cross-compile with CobOS/CobArch. Recipes and **every standard library module** compile. `porch`, `cornmath`, `kernels`, `almanac`, `bushel` and `furrow` are linked **statically** from `lib/libcob_stdlib.a` when you pass `--static-stdlib` (and by default for musl targets), so the finished program needs no `lib/` folder at run time; otherwise they are shared libraries and the program records the standard library directory as its rpath (keep `lib/` where it was, or set `COB_STDLIB`). `pantry`, `granary` and `barn` wrap C libraries and are always shared libraries, so a program that uses them still needs them next to it. A cross-compiled program needs the modules built for its target, with `COB_STDLIB` pointing at them.
* **`farmer`**: Package manager. Installs zipped modules from a secure static JSON registry into `cob_modules/` via `farmer harvest <package>`.
* **Caching Control**: Write `_MakeCache = False` as the literal first line of a `.cob` file to bypass the `.strawberry` fast-boot cache.

### Building
Cob is written in Rust (stable, 1.75 or newer). You need `cargo`, and a C compiler: `pantry` embeds the SQLite amalgamation and `barn` builds raylib/raygui, both vendored C libraries.
```
make                # cob_interp, popcorn_comp, farmer, and the standard library in bin/lib/
                    # (plus the *_enterprise tools when keys/drm_pubkeys.txt exists)
make test           # the regression suite (interpreter, compiler, REPL, stdlib)
make stdlib-barn    # optional: windows (needs X11 + OpenGL dev headers on Linux)
```
Ship `bin/lib/` next to the binaries. The browser playground runs the same interpreter compiled to WebAssembly: `make play` builds it into `docs/play/` (needs `rustup target add wasm32-unknown-unknown`), and `make wasm-test` checks it against the native build under Node.

### Docker
GitHub Actions builds and publishes a container image (linux/amd64 and linux/arm64) with the interpreter and REPL, the compiler, `farmer`, and the standard library — everything except `barn`, which needs a display:
```
docker run -it --rm ghcr.io/cob-software-foundation/cob                    # the REPL
docker run --rm -v "$PWD:/work" ghcr.io/cob-software-foundation/cob hello.cob
docker run --rm -v "$PWD:/work" ghcr.io/cob-software-foundation/cob popcorn_comp hello.strawberry -o hello
```
Add `--user "$(id -u):$(id -g)"` to read and write your own files as yourself (the image runs as an unprivileged user). A program compiled inside the image is for the image — run it there with `--entrypoint /work/hello`. Build it yourself with `docker build -t cob .` and check it with `sh tools/docker/smoke_test.sh cob`; the workflow is `.github/workflows/docker.yml`, and `Dockerfile.stress` is the separate image CI uses for its stress suite.

## 📜 Governance

Cob is actively maintained under the shared direction of **Pixel Pulse** and the **CSF** board.

## ⚖️ License & Trademark

Cob is source-available under the **PolyForm Noncommercial License 1.0.0**. Free for personal, educational, and non-profit use. **Commercial application is strictly prohibited.**

"Cob", "Cob Language", "popcorn_comp", "farmer", "Pixel Pulse", and "CSF" (Cob Software Foundation) are trademarks of the Cob project creators. Common law trademark rights are claimed under United States law based on prior continuous public use in software distribution.

Copyright (c) 2026 by Pixel Pulse and the Cob Software Foundation. All rights reserved.

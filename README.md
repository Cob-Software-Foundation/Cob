# Cob™ (Project Obsidian Falcon)

Cob is a corn-themed hybrid programming language combining Python-style indentation with low-level capability. It features a cross-platform interpreter, a native compiler powered by Zig, and its own package manager.

Development, stewardship, and project rights are co-managed by the **Cob Software Foundation™ (CSF)** and **Pixel Pulse**.

## 🚀 Key Features

* **Python-style Indentation:** Clean, block-based visual structure.
* **Dual Execution:** Run instantly via `cob_interp` or compile to standalone executables with `popcorn_comp`.
* **Built-in Package Ecosystem:** Download, verify, and resolve modules via `farmer`.
* **Explicit Memory Control:** Optional, manual heap management isolated behind strict compiler safety flags.

## 💻 Language Syntax

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
```

### Modular Imports (`shuck`)
```cob
# Slices code from local directory or cob_modules/
shuck greeter
```

### Manual Memory Management (Requires `--no-gc`)
```cob
# Explicit allocation and freeing
set buffer = harvest(1024)
trash(buffer)
```

### SQLite (Requires `cob_interp_full` or `cob_interp_db`)
```cob
set h = sql_open("scores.db")
set rc = sql_exec(h, "CREATE TABLE IF NOT EXISTS t (name TEXT)")
set name = sql_query(h, "SELECT name FROM t LIMIT 1")
pop("got: " + name)
set rc = sql_close(h)
```

### Native Windows & Widgets (Requires `cob_interp_full` or `cob_interp_window`)
```cob
shuck cobwindow
set w = window_open("Cob Window Demo")
set rc = window_label(w, "Hello from Cob!")
set clicked = window_button(w, "OK")
set volume = window_slider(w, "Volume", 100)
set name = window_textbox(w, "Name")
set rc = window_wait(w, 5)
set rc = window_close(w)
```
Backed by vendored `raylib` (window/drawing) and `raygui` (buttons,
sliders, text boxes) — no OS windowing API code, no configure step.
`window_button`/`window_slider`/`window_textbox` each identify their
widget by its own label text; the first call with a new label creates
it, auto-stacked on screen, and later calls with that label read or
update the same widget.

## ⚙️ Toolchain & Ecosystem

* **`cob_interp`**: The multi-platform interpreter. Plain `make` builds `cob_interp_full` (SQLite + native windows/widgets) by default; `make cob_interp` builds the original zero-dependency binary; `make cob_interp_db` swaps the window backend for Tcl/Tk instead.
* **`popcorn_comp`**: Native compiler. Transpiles to C and spawns a real compiler (Zig by default, or `$CobCC`/`--cc`) to produce a standalone binary — cross-compile with CobOS/CobArch. (SQLite/window/widget keywords and string values are cob_interp-only so far; not yet supported by popcorn_comp's codegen.)
* **`farmer`**: Package manager. Installs zipped modules from a secure static JSON registry into `cob_modules/` via `farmer harvest <package>`.
* **Caching Control**: Write `_MakeCache = False` as the literal first line of a `.cob` file to bypass the `.strawberry` fast-boot cache.

## 📜 Governance

Cob is actively maintained under the shared direction of **Pixel Pulse** and the **CSF** board.

## ⚖️ License & Trademark

Cob is source-available under the **PolyForm Noncommercial License 1.0.0**. Free for personal, educational, and non-profit use. **Commercial application is strictly prohibited.**

"Cob", "Cob Language", "popcorn_comp", "farmer", "Pixel Pulse", and "CSF" (Cob Software Foundation) are trademarks of the Cob project creators. Common law trademark rights are claimed under United States law based on prior continuous public use in software distribution.

Copyright (c) 2026 by Pixel Pulse and the Cob Software Foundation. All rights reserved.

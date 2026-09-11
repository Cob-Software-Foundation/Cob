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

## ⚙️ Toolchain & Ecosystem

* **`cob_interp`**: The multi-platform bytecode/source interpreter.
* **`popcorn_comp`**: Native compiler. Transpiles to C and compiles to a standalone binary using an embedded TCC instance (statically linked for native Linux amd64 targets).
* **`farmer`**: Package manager. Installs zipped modules from a secure static JSON registry into `cob_modules/` via `farmer harvest <package>`.
* **Caching Control**: Write `_MakeCache = False` as the literal first line of a `.cob` file to bypass the `.strawberry` fast-boot cache.

## 📜 Governance

Cob is actively maintained under the shared direction of **Pixel Pulse** and the **CSF** board.

## ⚖️ License & Trademark

Cob is source-available under the **PolyForm Noncommercial License 1.0.0**. Free for personal, educational, and non-profit use. **Commercial application is strictly prohibited.**

"Cob", "Cob Language", "popcorn_comp", "farmer", "Pixel Pulse", and "CSF" (Cob Software Foundation) are trademarks of the Cob project creators. Common law trademark rights are claimed under United States law based on prior continuous public use in software distribution.

Copyright (c) 2026 by Pixel Pulse and the Cob Software Foundation. All rights reserved.

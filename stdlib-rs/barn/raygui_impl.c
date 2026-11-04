/* raygui is a header-only C library: exactly one translation unit has to
 * compile its implementation. This is that unit -- everything else in barn is
 * Rust. */
#include "raylib.h"
#define RAYGUI_IMPLEMENTATION
#include "raygui.h"

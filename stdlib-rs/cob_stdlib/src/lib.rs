//! Every standard library module that is plain Rust (porch, cornmath, kernels, almanac, bushel, furrow) in ONE static
//! library, `libcob_stdlib.a`.
//!
//! The shared libraries next to `cob_interp` are what `shuck` loads at run time. A program compiled with popcorn_comp
//! can instead link this archive (`--static-stdlib`, the default for musl targets), so the finished executable needs no
//! `lib/` directory, no `COB_STDLIB` and no rpath. The modules that wrap C libraries (pantry, granary, barn) are not in
//! here; a program that uses them still links their shared library.
//!
//! Nothing is written here: naming the crates makes cargo link them, and their `#[no_mangle]` functions are the exports.
pub use {almanac, bushel, cornmath, furrow, kernels, porch};

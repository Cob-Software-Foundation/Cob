# Third-party Rust crates

Cob is built from the crates below (fetched from crates.io by Cargo), each under
its own license -- not this project's PolyForm Noncommercial license. This file
is generated from `cargo metadata` (including crates only used on other
platforms); regenerate it when `Cargo.lock` changes. A crate's full license text
ships in its source package on crates.io.

| Crate | Version | License (SPDX) |
|---|---|---|
| `adler2` | 2.0.1 | 0BSD OR MIT OR Apache-2.0 |
| `blake2` | 0.10.6 | MIT OR Apache-2.0 |
| `block-buffer` | 0.10.4 | MIT OR Apache-2.0 |
| `byteorder` | 1.5.0 | Unlicense OR MIT |
| `cc` | 1.0.83 | MIT OR Apache-2.0 |
| `cfg-if` | 1.0.5 | MIT OR Apache-2.0 |
| `cpufeatures` | 0.2.12 | MIT OR Apache-2.0 |
| `crc32fast` | 1.5.3 | MIT OR Apache-2.0 |
| `crossbeam-utils` | 0.8.23 | MIT OR Apache-2.0 |
| `crypto-common` | 0.1.7 | MIT OR Apache-2.0 |
| `curve25519-dalek` | 4.1.1 | BSD-3-Clause |
| `curve25519-dalek-derive` | 0.1.1 | MIT/Apache-2.0 |
| `digest` | 0.10.7 | MIT OR Apache-2.0 |
| `fiat-crypto` | 0.2.9 | MIT OR Apache-2.0 OR BSD-1-Clause |
| `flate2` | 1.1.10 | MIT OR Apache-2.0 |
| `generic-array` | 0.14.7 | MIT |
| `libc` | 0.2.150 | MIT OR Apache-2.0 |
| `libc` | 0.2.191 | MIT OR Apache-2.0 |
| `libloading` | 0.8.1 | ISC |
| `miniz_oxide` | 0.9.1 | MIT OR Zlib OR Apache-2.0 |
| `platforms` | 3.12.0 | Apache-2.0 OR MIT |
| `proc-macro2` | 1.0.107 | MIT OR Apache-2.0 |
| `quote` | 1.0.47 | MIT OR Apache-2.0 |
| `rustc_version` | 0.4.1 | MIT OR Apache-2.0 |
| `semver` | 1.0.28 | MIT OR Apache-2.0 |
| `sha2` | 0.10.8 | MIT OR Apache-2.0 |
| `simd-adler32` | 0.3.10 | MIT |
| `subtle` | 2.6.1 | BSD-3-Clause |
| `syn` | 2.0.119 | MIT OR Apache-2.0 |
| `typenum` | 1.20.1 | MIT OR Apache-2.0 |
| `unicode-ident` | 1.0.27 | (MIT OR Apache-2.0) AND Unicode-3.0 |
| `version_check` | 0.9.5 | MIT/Apache-2.0 |
| `windows-sys` | 0.48.0 | MIT OR Apache-2.0 |
| `windows-targets` | 0.48.5 | MIT OR Apache-2.0 |
| `windows_aarch64_gnullvm` | 0.48.5 | MIT OR Apache-2.0 |
| `windows_aarch64_msvc` | 0.48.5 | MIT OR Apache-2.0 |
| `windows_i686_gnu` | 0.48.5 | MIT OR Apache-2.0 |
| `windows_i686_msvc` | 0.48.5 | MIT OR Apache-2.0 |
| `windows_x86_64_gnu` | 0.48.5 | MIT OR Apache-2.0 |
| `windows_x86_64_gnullvm` | 0.48.5 | MIT OR Apache-2.0 |
| `windows_x86_64_msvc` | 0.48.5 | MIT OR Apache-2.0 |
| `zeroize` | 1.7.0 | Apache-2.0 OR MIT |
| `zip` | 0.6.6 | MIT |

43 crates. Notable obligations: `curve25519-dalek` and `subtle` are BSD-3-Clause (binary
redistributions must reproduce their copyright notices and disclaimers).

Also vendored as C source under `vendor/` (see `LICENSE.md`): SQLite (public
domain), raylib and raygui (zlib).

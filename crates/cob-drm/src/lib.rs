//! cob-drm -- Cob Enterprise license verification and machine binding.
//!
//! Only the enterprise builds (`--features enterprise`) of `cob_interp` and
//! `popcorn_comp` link this; the free build never contains any license code.
//! `cob_sign` (the maintainer's signing tool) uses it too, for the same file
//! format and the same machine id.

pub mod eddsa;
pub mod license;
pub mod machine;

pub use license::{
    b64decode, b64encode, current, decide_binding, feature_enabled, load, require, search_paths, verify_text, write_binding,
    Bind, BindDecision, LicError, License, Loaded, Verified, EMBEDDED_KEYS,
};

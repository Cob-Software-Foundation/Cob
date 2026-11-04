//! cob-core: the Cob language engine -- parser, call checker, evaluator,
//! `.strawberry` AST cache and REPL. The `cob_interp` binary is a thin shell
//! around this crate.

pub mod ast;
pub mod cache;
pub mod check;
pub mod consts;
pub mod eval;
pub mod fsutil;
pub mod parse;
pub mod repl;
pub mod session;

pub use session::{Session, Value};

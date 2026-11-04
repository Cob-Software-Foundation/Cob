//! The Cob syntax tree. Tag numbers match the `.strawberry` cache format
//! (see cache.rs) -- do not reorder.

use std::rc::Rc;

#[derive(Debug, Clone, PartialEq)]
pub struct CallExpr {
    pub name: String,
    pub args: Vec<Expr>,
    /// Source line, for diagnostics only; not stored in the cache.
    pub line: i32,
}

#[derive(Debug, Clone, PartialEq)]
pub enum Expr {
    Num(i64),
    Str(Vec<u8>),
    Var(String),
    /// `+ - * /`
    Bin(u8, Box<Expr>, Box<Expr>),
    Harvest(Box<Expr>),
    /// A call to a name bound by an `extern` statement (CEP-2).
    ExternCall(CallExpr),
    /// A call to a `recipe`.
    Call(CallExpr),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CondOp {
    Truthy = 0,
    Eq = 1,
    Ne = 2,
    Lt = 3,
    Gt = 4,
    Le = 5,
    Ge = 6,
}

#[derive(Debug, Clone, PartialEq)]
pub struct Cond {
    pub left: Expr,
    pub op: CondOp,
    /// `None` when `op == Truthy`.
    pub right: Option<Expr>,
}

pub type Program = Vec<Stmt>;

#[derive(Debug, PartialEq)]
pub struct Recipe {
    pub name: String,
    pub params: Vec<String>,
    pub body: Program,
    pub line: i32,
}

#[derive(Debug, Clone, PartialEq)]
pub enum Stmt {
    Pop(Expr),
    Set(String, Expr),
    While(Cond, Program),
    Trash(String),
    /// `extern name(...) from "lib"`: (C symbol, library path)
    Extern(String, Vec<u8>),
    If(Cond, Program, Program),
    Recipe(Rc<Recipe>),
    Serve(Expr),
    Call(Expr),
}

impl Expr {
    pub fn uses_harvest(&self) -> bool {
        match self {
            Expr::Harvest(_) => true,
            Expr::Bin(_, l, r) => l.uses_harvest() || r.uses_harvest(),
            Expr::ExternCall(c) | Expr::Call(c) => c.args.iter().any(|a| a.uses_harvest()),
            _ => false,
        }
    }
}

impl Cond {
    pub fn uses_harvest(&self) -> bool {
        self.left.uses_harvest() || self.right.as_ref().map_or(false, |r| r.uses_harvest())
    }
}

/// Does anything in this program need the harvest/trash runtime?
pub fn program_uses_harvest_or_trash(p: &Program) -> bool {
    p.iter().any(|s| match s {
        Stmt::Set(_, e) | Stmt::Pop(e) | Stmt::Serve(e) | Stmt::Call(e) => e.uses_harvest(),
        Stmt::Trash(_) => true,
        Stmt::While(_, b) => program_uses_harvest_or_trash(b),
        Stmt::If(_, a, b) => program_uses_harvest_or_trash(a) || program_uses_harvest_or_trash(b),
        Stmt::Recipe(r) => program_uses_harvest_or_trash(&r.body),
        Stmt::Extern(..) => false,
    })
}

pub fn program_has_recipe(p: &Program) -> bool {
    p.iter().any(|s| matches!(s, Stmt::Recipe(_)))
}

/// Names a recipe makes local: its parameters, then everything it `set`s.
pub fn recipe_locals(r: &Recipe) -> Vec<String> {
    fn collect(p: &Program, out: &mut Vec<String>) {
        for s in p {
            match s {
                Stmt::Set(n, _) => {
                    if !out.iter().any(|x| x == n) {
                        out.push(n.clone());
                    }
                }
                Stmt::While(_, b) => collect(b, out),
                Stmt::If(_, a, b) => {
                    collect(a, out);
                    collect(b, out);
                }
                _ => {}
            }
        }
    }
    let mut out: Vec<String> = Vec::new();
    for p in &r.params {
        if !out.iter().any(|x| x == p) {
            out.push(p.clone());
        }
    }
    collect(&r.body, &mut out);
    out
}

//! Post-parse call checking: every call must name a recipe with the right
//! number of arguments, with helpful messages for stdlib names.

use crate::ast::*;
use crate::session::{report, Session};

fn stdlib_module_for(name: &str) -> Option<&'static str> {
    const TABLE: &[(&str, &str)] = &[
        ("porch_", "porch"),
        ("kernel_", "kernels"),
        ("almanac_", "almanac"),
        ("granary_", "granary"),
        ("pantry_", "pantry"),
        ("barn_", "barn"),
        ("husk_", "cornmath"),
        ("ear_", "cornmath"),
        ("stalk_", "cornmath"),
        ("silk_", "cornmath"),
        ("scatter_", "cornmath"),
    ];
    TABLE.iter().find(|(p, _)| name.starts_with(p)).map(|(_, m)| *m)
}

/// Names that were built-in keywords before v0.0.9 and live in the standard
/// library now -- so an upgrading program is told exactly what to write.
fn moved_builtin(name: &str) -> Option<(&'static str, &'static str)> {
    const MOVED: &[(&str, &str, &str)] = &[
        ("input", "porch_ask", "porch"),
        ("input_int", "porch_ask_int", "porch"),
        ("sql_open", "pantry_open", "pantry"),
        ("sql_exec", "pantry_exec", "pantry"),
        ("sql_query", "pantry_query", "pantry"),
        ("sql_close", "pantry_close", "pantry"),
        ("window_open", "barn_open", "barn"),
        ("window_label", "barn_label", "barn"),
        ("window_wait", "barn_wait", "barn"),
        ("window_close", "barn_close", "barn"),
        ("window_button", "barn_button", "barn"),
        ("window_slider", "barn_slider", "barn"),
        ("window_textbox", "barn_textbox", "barn"),
    ];
    MOVED.iter().find(|(o, _, _)| *o == name).map(|(_, now, m)| (*now, *m))
}

impl Session {
    fn check_lookup(&self, top: Option<&Program>, name: &str) -> Option<usize> {
        if let Some(top) = top {
            for s in top {
                if let Stmt::Recipe(r) = s {
                    if r.name == name {
                        return Some(r.params.len());
                    }
                }
            }
        }
        self.func_find(name).map(|f| f.def.params.len())
    }

    fn check_expr(&self, top: Option<&Program>, e: &Expr, lenient: bool) -> Result<(), ()> {
        match e {
            Expr::Call(c) => {
                match self.check_lookup(top, &c.name) {
                    None => {
                        if !lenient {
                            if let Some((moved_to, moved_mod)) = moved_builtin(&c.name) {
                                report(c.line, "error", &format!("'{}' is now {}() in the standard library: add `shuck {}` at the top and call {}(...)", c.name, moved_to, moved_mod, moved_to));
                            } else if let Some(m) = stdlib_module_for(&c.name) {
                                report(c.line, "error", &format!("'{}' is not defined -- it is in the standard library: add `shuck {}` at the top", c.name, m));
                            } else {
                                report(c.line, "error", &format!("call to '{}', which is not a recipe or a library function", c.name));
                            }
                            return Err(());
                        }
                    }
                    Some(np) if np != c.args.len() => {
                        report(
                            c.line,
                            "error",
                            &format!(
                                "recipe '{}' takes {} argument{} but {} {} given",
                                c.name,
                                np,
                                if np == 1 { "" } else { "s" },
                                c.args.len(),
                                if c.args.len() == 1 { "was" } else { "were" }
                            ),
                        );
                        return Err(());
                    }
                    Some(_) => {}
                }
                for a in &c.args {
                    self.check_expr(top, a, lenient)?;
                }
                Ok(())
            }
            Expr::ExternCall(c) => {
                for a in &c.args {
                    self.check_expr(top, a, lenient)?;
                }
                Ok(())
            }
            Expr::Bin(_, l, r) => {
                self.check_expr(top, l, lenient)?;
                self.check_expr(top, r, lenient)
            }
            Expr::Harvest(x) => self.check_expr(top, x, lenient),
            Expr::Num(_) | Expr::Str(_) | Expr::Var(_) => Ok(()),
        }
    }

    fn check_cond(&self, top: Option<&Program>, c: &Cond, lenient: bool) -> Result<(), ()> {
        self.check_expr(top, &c.left, lenient)?;
        if let Some(r) = &c.right {
            self.check_expr(top, r, lenient)?;
        }
        Ok(())
    }

    /// `lenient` (REPL) applies ONLY inside recipe bodies, where a call may
    /// name a recipe that will be typed later.
    fn check_program(&self, top: Option<&Program>, p: &Program, lenient: bool, in_recipe: bool) -> Result<(), ()> {
        let l = in_recipe && lenient;
        for s in p {
            match s {
                Stmt::Pop(e) | Stmt::Set(_, e) | Stmt::Serve(e) | Stmt::Call(e) => self.check_expr(top, e, l)?,
                Stmt::While(c, body) => {
                    self.check_cond(top, c, l)?;
                    self.check_program(top, body, lenient, in_recipe)?;
                }
                Stmt::If(c, a, b) => {
                    self.check_cond(top, c, l)?;
                    self.check_program(top, a, lenient, in_recipe)?;
                    self.check_program(top, b, lenient, in_recipe)?;
                }
                Stmt::Recipe(r) => self.check_program(top, &r.body, lenient, true)?,
                Stmt::Trash(_) | Stmt::Extern(..) => {}
            }
        }
        Ok(())
    }

    pub fn check_calls(&self, top: &Program, lenient: bool) -> Result<(), ()> {
        self.check_program(Some(top), top, lenient, false)
    }

    /// Only the REPL checks a lone expression.
    pub fn check_calls_expr(&self, e: &Expr, lenient: bool) -> Result<(), ()> {
        self.check_expr(None, e, lenient)
    }
}

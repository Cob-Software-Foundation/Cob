//! The tree-walking evaluator.

use cob_abi::cstdio;
use cob_abi::{ewrite, CobValue, COB_INT, COB_STR};

use crate::ast::*;
use crate::consts::*;
use crate::session::*;

fn to_text(n: i64) -> Vec<u8> {
    n.to_string().into_bytes()
}

impl Session {
    /// A variable lookup inside a recipe sees its locals first, then the globals.
    fn ctx_get(&self, name: &str) -> Option<Value> {
        if let Some(l) = &self.locals {
            if let Some(v) = l.get(name) {
                return Some(v);
            }
        }
        self.vars.get(name)
    }

    pub fn eval_expr(&mut self, e: &Expr) -> Value {
        match e {
            Expr::Num(n) => Value::Int(*n),
            Expr::Str(s) => Value::Str(s.clone()),
            Expr::Var(name) => match self.ctx_get(name) {
                Some(v) => v,
                None => {
                    ewrite(&format!("[cob_interp] warning: undefined variable '{}', treated as 0\n", name));
                    Value::Int(0)
                }
            },
            Expr::Bin(op, l, r) => {
                let lv = self.eval_expr(l);
                let rv = self.eval_expr(r);
                if *op == b'+' && (matches!(lv, Value::Str(_)) || matches!(rv, Value::Str(_))) {
                    let mut out = match lv {
                        Value::Str(s) => s,
                        Value::Int(n) => to_text(n),
                    };
                    match rv {
                        Value::Str(s) => out.extend_from_slice(&s),
                        Value::Int(n) => out.extend_from_slice(&to_text(n)),
                    }
                    return Value::Str(out);
                }
                let (a, b) = match (lv, rv) {
                    (Value::Int(a), Value::Int(b)) => (a, b),
                    _ => {
                        ewrite(&format!(
                            "[cob_interp] warning: operator '{}' requires two numbers, got a string; result treated as 0\n",
                            *op as char
                        ));
                        return Value::Int(0);
                    }
                };
                match op {
                    b'+' => Value::Int(a.wrapping_add(b)),
                    b'-' => Value::Int(a.wrapping_sub(b)),
                    b'*' => Value::Int(a.wrapping_mul(b)),
                    b'/' => {
                        if b == 0 {
                            ewrite("[cob_interp] warning: division by zero, result treated as 0\n");
                            return Value::Int(0);
                        }
                        Value::Int(a.wrapping_div(b))
                    }
                    _ => Value::Int(0),
                }
            }
            Expr::Harvest(x) => {
                let bv = self.eval_expr(x);
                let mut bytes = match bv {
                    Value::Str(_) => {
                        ewrite("[cob_interp] warning: harvest() needs a number, got a string; treated as 0\n");
                        0
                    }
                    Value::Int(n) => n,
                };
                if bytes < 0 {
                    ewrite("[cob_interp] warning: harvest() with negative size, treated as 0\n");
                    bytes = 0;
                }
                let id = self.handles.alloc(bytes as usize);
                if id == 0 {
                    ewrite(&format!("[cob_interp] warning: harvest({}) failed (out of memory)\n", bytes));
                }
                Value::Int(id)
            }
            Expr::Call(c) => self.call_recipe(c),
            Expr::ExternCall(c) => self.call_extern(c),
        }
    }

    fn call_extern(&mut self, c: &CallExpr) -> Value {
        // evaluate left to right; keep NUL-terminated copies alive for the call
        let mut vals: Vec<Value> = Vec::with_capacity(c.args.len());
        for a in &c.args {
            vals.push(self.eval_expr(a));
        }
        let mut bufs: Vec<Vec<u8>> = Vec::new();
        let mut str_slot: Vec<Option<usize>> = Vec::with_capacity(vals.len());
        for v in &vals {
            if let Value::Str(s) = v {
                let mut b = s.clone();
                b.push(0);
                bufs.push(b);
                str_slot.push(Some(bufs.len() - 1));
            } else {
                str_slot.push(None);
            }
        }
        let mut argv: Vec<CobValue> = vals
            .iter()
            .zip(&str_slot)
            .map(|(v, slot)| match (v, slot) {
                (Value::Int(n), _) => CobValue { kind: COB_INT, as_: cob_abi::CobUnion { i: *n as cob_abi::CobLong } },
                (Value::Str(_), Some(i)) => CobValue {
                    kind: COB_STR,
                    as_: cob_abi::CobUnion { s: bufs[*i].as_ptr() as *const std::os::raw::c_char },
                },
                _ => CobValue { kind: COB_INT, as_: cob_abi::CobUnion { i: 0 } },
            })
            .collect();
        match self.extern_find(&c.name) {
            None => {
                ewrite(&format!(
                    "[cob_interp] warning: extern '{}' called before its `extern` statement ran; treated as 0\n",
                    c.name
                ));
                Value::Int(0)
            }
            // failed or sandboxed binding: already warned once, stay quiet per call
            Some(b) => match b.func {
                None => Value::Int(0),
                Some(f) => {
                    let r = unsafe { f(argv.as_mut_ptr(), argv.len() as std::os::raw::c_int) };
                    unsafe {
                        if r.kind == COB_STR {
                            if r.as_.s.is_null() {
                                Value::Str(Vec::new())
                            } else {
                                Value::Str(std::ffi::CStr::from_ptr(r.as_.s).to_bytes().to_vec())
                            }
                        } else {
                            Value::Int(r.as_.i as i64)
                        }
                    }
                }
            },
        }
    }

    /// Runs a recipe: arguments are evaluated left to right in the CALLER'S
    /// scope, a fresh set of locals is built (every local starts at 0,
    /// parameters take the argument values), the body runs, and whatever
    /// `serve` handed back is the value of the call (0 if the body ran off
    /// its end).
    fn call_recipe(&mut self, c: &CallExpr) -> Value {
        let (def, local_names) = match self.func_find(&c.name) {
            Some(f) => (f.def.clone(), f.locals.clone()),
            None => {
                report(0, "warning", &format!("call to undefined recipe '{}', treated as 0", c.name));
                return Value::Int(0);
            }
        };
        if c.args.len() != def.params.len() {
            report(
                0,
                "warning",
                &format!(
                    "recipe '{}' takes {} argument(s) but {} given; call treated as 0",
                    c.name,
                    def.params.len(),
                    c.args.len()
                ),
            );
            return Value::Int(0);
        }
        let mut argv: Vec<Value> = Vec::with_capacity(c.args.len());
        for a in &c.args {
            argv.push(self.eval_expr(a));
        }
        if self.call_depth >= MAX_CALL_DEPTH {
            report(
                0,
                "warning",
                &format!("recipe '{}' is nested more than {} calls deep; call treated as 0", c.name, MAX_CALL_DEPTH),
            );
            return Value::Int(0);
        }
        let mut locals = VarTable::new();
        for n in &local_names {
            locals.set(n, Value::Int(0));
        }
        for (p, v) in def.params.iter().zip(argv) {
            locals.set(p, v);
        }
        let saved_locals = self.locals.replace(locals);
        let saved_ret = std::mem::replace(&mut self.ret, Value::Int(0));
        self.call_depth += 1;
        let returned = self.execute(&def.body);
        let result = std::mem::replace(&mut self.ret, saved_ret);
        self.locals = saved_locals;
        self.call_depth -= 1;
        self.fell_off = !returned;
        result
    }

    pub fn eval_cond(&mut self, c: &Cond) -> bool {
        let l = self.eval_expr(&c.left);
        if c.op == CondOp::Truthy {
            return match l {
                Value::Str(s) => !s.is_empty(),
                Value::Int(n) => n != 0,
            };
        }
        let r = self.eval_expr(c.right.as_ref().expect("comparison has a right side"));
        match (l, r) {
            (Value::Str(a), Value::Str(b)) => {
                let cmp = a.cmp(&b);
                match c.op {
                    CondOp::Eq => cmp.is_eq(),
                    CondOp::Ne => cmp.is_ne(),
                    CondOp::Lt => cmp.is_lt(),
                    CondOp::Gt => cmp.is_gt(),
                    CondOp::Le => cmp.is_le(),
                    CondOp::Ge => cmp.is_ge(),
                    CondOp::Truthy => false,
                }
            }
            (Value::Int(a), Value::Int(b)) => match c.op {
                CondOp::Eq => a == b,
                CondOp::Ne => a != b,
                CondOp::Lt => a < b,
                CondOp::Gt => a > b,
                CondOp::Le => a <= b,
                CondOp::Ge => a >= b,
                CondOp::Truthy => false,
            },
            _ => {
                ewrite("[cob_interp] warning: comparing a number to a string; result is false\n");
                false
            }
        }
    }

    /// Runs a block. Returns true if a `serve` ran inside it (the caller must
    /// stop and hand the value up through `self.ret`).
    pub fn execute(&mut self, prog: &Program) -> bool {
        for s in prog {
            match s {
                Stmt::Pop(e) => {
                    let v = self.eval_expr(e);
                    match v {
                        Value::Str(s) => cstdio::write_stdout(&s),
                        Value::Int(n) => cstdio::write_stdout(n.to_string().as_bytes()),
                    }
                    cstdio::write_stdout(b"\n");
                }
                Stmt::Set(name, e) => {
                    let v = self.eval_expr(e);
                    // inside a recipe, every `set` target is one of its locals
                    match &mut self.locals {
                        Some(l) => l.set(name, v),
                        None => self.vars.set(name, v),
                    }
                }
                Stmt::Trash(name) => {
                    let Some(v) = self.ctx_get(name) else {
                        ewrite(&format!("[cob_interp] warning: trash() on undefined variable '{}'\n", name));
                        continue;
                    };
                    match v {
                        Value::Str(_) => {
                            ewrite(&format!("[cob_interp] warning: trash({}) -- variable holds a string, not a handle\n", name));
                        }
                        Value::Int(id) => {
                            if self.handles.free(id).is_err() {
                                ewrite(&format!(
                                    "[cob_interp] warning: trash({}) -- handle {} is invalid or already freed\n",
                                    name, id
                                ));
                            }
                        }
                    }
                }
                Stmt::Extern(name, lib) => self.extern_bind(name, lib),
                Stmt::If(c, a, b) => {
                    let taken = if self.eval_cond(c) { a } else { b };
                    if self.execute(taken) {
                        return true;
                    }
                }
                Stmt::Recipe(_) => {} // hoisted: already registered before the program started
                Stmt::Serve(e) => {
                    let v = self.eval_expr(e);
                    self.ret = v;
                    return true;
                }
                Stmt::Call(e) => {
                    let _ = self.eval_expr(e);
                }
                Stmt::While(c, body) => {
                    let mut iterations: u64 = 0;
                    while self.eval_cond(c) {
                        if self.execute(body) {
                            return true;
                        }
                        iterations += 1;
                        if iterations >= MAX_LOOP_ITERATIONS {
                            ewrite(&format!(
                                "[cob_interp] warning: while loop hit the {}-iteration safety cap, stopping it early\n",
                                MAX_LOOP_ITERATIONS
                            ));
                            break;
                        }
                    }
                }
            }
        }
        false
    }
}

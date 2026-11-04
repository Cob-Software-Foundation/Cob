//! AST -> C source text.
//!
//! Layout of the generated translation unit: runtime, extern adapters,
//! file-scope globals, recipe prototypes + bodies, then `main()` holding the
//! top-level statements.

use cob_core::ast::*;
use cob_core::consts::MAX_CALL_DEPTH;

use crate::runtime;

/// An ordered set of names.
#[derive(Default)]
pub struct NameSet(pub Vec<String>);

impl NameSet {
    pub fn add(&mut self, n: &str) {
        if !self.0.iter().any(|x| x == n) {
            self.0.push(n.to_string());
        }
    }
    pub fn has(&self, n: &str) -> bool {
        self.0.iter().any(|x| x == n)
    }
}

fn collect_expr(e: &Expr, ns: &mut NameSet) {
    match e {
        Expr::Var(v) => ns.add(v),
        Expr::Bin(_, l, r) => {
            collect_expr(l, ns);
            collect_expr(r, ns);
        }
        Expr::Harvest(x) => collect_expr(x, ns),
        Expr::ExternCall(c) | Expr::Call(c) => c.args.iter().for_each(|a| collect_expr(a, ns)),
        Expr::Num(_) | Expr::Str(_) => {}
    }
}

fn collect_cond(c: &Cond, ns: &mut NameSet) {
    collect_expr(&c.left, ns);
    if let Some(r) = &c.right {
        collect_expr(r, ns);
    }
}

fn collect_program(p: &Program, ns: &mut NameSet) {
    for s in p {
        match s {
            Stmt::Pop(e) | Stmt::Serve(e) | Stmt::Call(e) => collect_expr(e, ns),
            Stmt::Set(n, e) => {
                ns.add(n);
                collect_expr(e, ns);
            }
            Stmt::Trash(n) => ns.add(n),
            Stmt::While(c, b) => {
                collect_cond(c, ns);
                collect_program(b, ns);
            }
            Stmt::If(c, a, b) => {
                collect_cond(c, ns);
                collect_program(a, ns);
                collect_program(b, ns);
            }
            Stmt::Recipe(_) | Stmt::Extern(..) => {}
        }
    }
}

fn locals_of(r: &Recipe) -> NameSet {
    NameSet(recipe_locals(r))
}

/// One distinct `extern` declaration (name, library).
pub struct ExternDecl {
    pub name: String,
    pub lib: String,
}

/// Collects every `extern` anywhere in the program as ONE static declaration
/// per name. Repeating the same (name, library) pair is fine; the same name
/// against two libraries is an error -- a compiled binary links once, so
/// there is no principled way to pick.
pub fn collect_externs(p: &Program, out: &mut Vec<ExternDecl>) -> Result<(), ()> {
    for s in p {
        match s {
            Stmt::Extern(name, lib) => {
                let lib = String::from_utf8_lossy(lib).into_owned();
                if let Some(prev) = out.iter().find(|d| d.name == *name) {
                    if prev.lib == lib {
                        continue;
                    }
                    eprintln!(
                        "[popcorn_comp] error: extern '{}' is declared against two different libraries\n  ('{}' and '{}') -- popcorn_comp links statically, so every occurrence of the\n  same extern name in one program must name the same library",
                        name, prev.lib, lib
                    );
                    return Err(());
                }
                out.push(ExternDecl { name: name.clone(), lib });
            }
            Stmt::Recipe(r) => collect_externs(&r.body, out)?,
            Stmt::While(_, b) => collect_externs(b, out)?,
            Stmt::If(_, a, b) => {
                collect_externs(a, out)?;
                collect_externs(b, out)?;
            }
            _ => {}
        }
    }
    Ok(())
}

// ---- expression / statement emission ----------------------------------------

struct Out(Vec<u8>);

impl Out {
    fn s(&mut self, t: &str) {
        self.0.extend_from_slice(t.as_bytes());
    }
}

fn emit_c_string_literal(o: &mut Out, text: &[u8]) {
    o.s("\"");
    for &c in text {
        match c {
            b'"' => o.s("\\\""),
            b'\\' => o.s("\\\\"),
            b'\n' => o.s("\\n"),
            b'\t' => o.s("\\t"),
            c if c < 0x20 || c == 0x7f => o.s(&format!("\\x{:02x}\"\"", c)),
            c => o.0.push(c),
        }
    }
    o.s("\"");
}

/// Does evaluating this expression run code that can print or read (a recipe
/// or library call)? Only when BOTH operands of an operator do does their
/// order matter -- see `emit_sequenced`.
fn has_effects(e: &Expr) -> bool {
    match e {
        Expr::Call(_) | Expr::ExternCall(_) => true,
        Expr::Bin(_, l, r) => has_effects(l) || has_effects(r),
        Expr::Harvest(x) => has_effects(x),
        _ => false,
    }
}

/// fn(left, right) with `left` guaranteed to run before `right`. C leaves the
/// evaluation order of function arguments unspecified, but Cob evaluates left
/// to right. When both sides have effects the left value is parked on the
/// argument stack by a comma expression (which IS sequenced).
fn emit_sequenced(o: &mut Out, func: &str, l: &Expr, r: &Expr) {
    if has_effects(l) && has_effects(r) {
        o.s("(cob_argpush(");
        emit_expr(o, l);
        o.s(&format!("), {}(cob_argpop(), ", func));
        emit_expr(o, r);
        o.s("))");
        return;
    }
    o.s(&format!("{}(", func));
    emit_expr(o, l);
    o.s(", ");
    emit_expr(o, r);
    o.s(")");
}

fn emit_expr(o: &mut Out, e: &Expr) {
    match e {
        Expr::Num(n) => {
            if *n == i64::MIN {
                // -9223372036854775808L is not a valid C literal
                o.s("cob_int(-9223372036854775807L - 1)");
            } else {
                o.s(&format!("cob_int({}L)", n));
            }
        }
        Expr::Str(s) => {
            o.s("cob_str(");
            emit_c_string_literal(o, s);
            o.s(")");
        }
        Expr::Var(v) => o.s(&format!("cob_copy(&cob_var_{})", v)),
        Expr::Bin(op, l, r) => {
            let f = match op {
                b'+' => "cob_add",
                b'-' => "cob_sub",
                b'*' => "cob_mul",
                _ => "cob_divop",
            };
            emit_sequenced(o, f, l, r);
        }
        Expr::Harvest(x) => {
            o.s("cob_int((long)(intptr_t)malloc((size_t)cob_harvest_bytes(");
            emit_expr(o, x);
            o.s(")))");
        }
        Expr::Call(c) => {
            // (cob_argpush(a0), cob_argpush(a1), cob_fn_name()): the comma
            // operator evaluates the arguments strictly left to right and the
            // callee pops them into its parameters.
            if c.args.is_empty() {
                o.s(&format!("cob_fn_{}()", c.name));
                return;
            }
            o.s("(");
            for a in &c.args {
                o.s("cob_argpush(");
                emit_expr(o, a);
                o.s("), ");
            }
            o.s(&format!("cob_fn_{}())", c.name));
        }
        Expr::ExternCall(c) => {
            // cob_call_<n>(...) is the per-name adapter emitted once near the
            // top of the file. argc==0 passes NULL/0 (an empty compound
            // literal is not valid C99).
            if c.args.is_empty() {
                o.s(&format!("cob_call_{}(NULL, 0)", c.name));
                return;
            }
            let effectful = c.args.iter().filter(|a| has_effects(a)).count();
            if effectful >= 2 {
                // two or more arguments that print/read: evaluate them in
                // order onto the argument stack, call on that frame, drop it
                o.s("(");
                for a in &c.args {
                    o.s("cob_argpush(");
                    emit_expr(o, a);
                    o.s("), ");
                }
                let n = c.args.len();
                o.s(&format!("cob_argdrop(cob_call_{}(cob_argtop({}), {}), {}))", c.name, n, n, n));
                return;
            }
            o.s(&format!("cob_call_{}((CobValue[]){{", c.name));
            for (j, a) in c.args.iter().enumerate() {
                if j > 0 {
                    o.s(", ");
                }
                emit_expr(o, a);
            }
            o.s(&format!("}}, {})", c.args.len()));
        }
    }
}

fn emit_cond(o: &mut Out, c: &Cond) {
    if c.op == CondOp::Truthy {
        o.s("cob_truthy(");
        emit_expr(o, &c.left);
        o.s(")");
        return;
    }
    let f = match c.op {
        CondOp::Eq => "cob_eq",
        CondOp::Ne => "cob_ne",
        CondOp::Lt => "cob_lt",
        CondOp::Gt => "cob_gt",
        CondOp::Le => "cob_le",
        _ => "cob_ge",
    };
    emit_sequenced(o, f, &c.left, c.right.as_ref().expect("comparison has a right side"));
}

fn emit_program(o: &mut Out, p: &Program) {
    for s in p {
        match s {
            Stmt::Pop(e) => {
                o.s("cob_pop(");
                emit_expr(o, e);
                o.s(");\n");
            }
            Stmt::Set(name, e) => {
                // Evaluate FIRST, then free the old value and store: the
                // right-hand side may read the very variable being assigned.
                o.s("{ CobValue cob_tmp = ");
                emit_expr(o, e);
                o.s(&format!("; cob_free(&cob_var_{0}); cob_var_{0} = cob_tmp; }}\n", name));
            }
            Stmt::Trash(n) => o.s(&format!(
                "if (cob_var_{0}.is_str) fprintf(stderr, \"[cob] warning: trash({0}) -- variable holds a string, not a handle\\n\");\nelse {{ free((void*)(intptr_t)cob_var_{0}.i); cob_var_{0}.i = 0; }}\n",
                n
            )),
            Stmt::While(c, b) => {
                o.s("while (");
                emit_cond(o, c);
                o.s(") {\n");
                emit_program(o, b);
                o.s("}\n");
            }
            Stmt::If(c, a, b) => {
                o.s("if (");
                emit_cond(o, c);
                o.s(") {\n");
                emit_program(o, a);
                if !b.is_empty() {
                    o.s("} else {\n");
                    emit_program(o, b);
                }
                o.s("}\n");
            }
            // The compiled program's binding is entirely static, and recipes
            // are emitted once at file scope.
            Stmt::Extern(..) | Stmt::Recipe(_) => {}
            Stmt::Serve(e) => {
                // hand the value to the single exit of the enclosing recipe
                o.s("{ CobValue cob_tmp = ");
                emit_expr(o, e);
                o.s("; cob_free(&cob_retval); cob_retval = cob_tmp; goto cob_done; }\n");
            }
            Stmt::Call(e) => {
                o.s("{ CobValue cob_tmp = ");
                emit_expr(o, e);
                o.s("; cob_free(&cob_tmp); }\n");
            }
        }
    }
}

/// One adapter per distinct extern name. It converts each argument to the
/// ABI-identical `CobFFIValue`, calls the native function once, and converts
/// the result back. Argument strings stay alive until AFTER the call (cob.h's
/// documented contract); only then are the originals freed.
fn emit_extern_adapters(o: &mut Out, externs: &[ExternDecl]) {
    if externs.is_empty() {
        return;
    }
    o.s(runtime::FFI_PREAMBLE);
    for d in externs {
        let name = &d.name;
        o.s(&format!("extern CobFFIValue {}(CobFFIValue *, int);\n", name));
        o.s(&format!(
            "static CobValue cob_call_{0}(CobValue *cob_ffi_args, int cob_ffi_argc) {{\n\
             \x20   CobFFIValue cob_ffi_buf[cob_ffi_argc > 0 ? (size_t)cob_ffi_argc : 1];\n\
             \x20   int cob_ffi_i;\n\
             \x20   CobFFIValue cob_ffi_r;\n\
             \x20   CobValue cob_ffi_result;\n\
             \x20   for (cob_ffi_i = 0; cob_ffi_i < cob_ffi_argc; cob_ffi_i++) {{\n\
             \x20       if (cob_ffi_args[cob_ffi_i].is_str) {{\n\
             \x20           cob_ffi_buf[cob_ffi_i].kind = COB_FFI_STR;\n\
             \x20           cob_ffi_buf[cob_ffi_i].as.s = cob_ffi_args[cob_ffi_i].s;\n\
             \x20       }} else {{\n\
             \x20           cob_ffi_buf[cob_ffi_i].kind = COB_FFI_INT;\n\
             \x20           cob_ffi_buf[cob_ffi_i].as.i = cob_ffi_args[cob_ffi_i].i;\n\
             \x20       }}\n\
             \x20   }}\n\
             \x20   cob_ffi_r = {0}(cob_ffi_argc > 0 ? cob_ffi_buf : NULL, cob_ffi_argc);\n\
             \x20   cob_ffi_result = cob_ffi_r.kind == COB_FFI_STR ? cob_str(cob_ffi_r.as.s ? cob_ffi_r.as.s : \"\") : cob_int(cob_ffi_r.as.i);\n\
             \x20   for (cob_ffi_i = 0; cob_ffi_i < cob_ffi_argc; cob_ffi_i++) cob_free(&cob_ffi_args[cob_ffi_i]);\n\
             \x20   return cob_ffi_result;\n\
             }}\n",
            name
        ));
    }
}

/// One recipe as a C function. Parameters and `set` names are C locals
/// (declared up front, starting at 0); every other name is the file-scope
/// global. Arguments were pushed left to right, so they are popped in
/// reverse. A `serve` is a goto to the one exit, which frees the locals.
fn emit_recipe(o: &mut Out, r: &Recipe) {
    let locals = locals_of(r);
    o.s(&format!("static CobValue cob_fn_{}(void) {{\n", r.name));
    o.s("CobValue cob_retval = cob_int(0);\n");
    for l in &locals.0 {
        o.s(&format!("CobValue cob_var_{} = cob_int(0);\n", l));
    }
    o.s(&format!("int cob_over = (cob_depth >= {});\n", MAX_CALL_DEPTH));
    for p in r.params.iter().rev() {
        o.s(&format!("cob_var_{} = cob_argpop();\n", p));
    }
    o.s(&format!(
        "if (cob_over) {{ fprintf(stderr, \"[cob] warning: recipe '{}' is nested more than {} calls deep; call treated as 0\\n\"); goto cob_leave; }}\ncob_depth++;\n",
        r.name, MAX_CALL_DEPTH
    ));
    emit_program(o, &r.body);
    o.s("cob_done:\ncob_depth--;\ncob_leave:\n");
    for l in &locals.0 {
        o.s(&format!("cob_free(&cob_var_{});\n", l));
    }
    o.s("return cob_retval;\n}\n");
}

/// Builds the full, self-contained C translation unit for `prog`.
pub fn transpile(prog: &Program, externs: &[ExternDecl]) -> Vec<u8> {
    let mut o = Out(Vec::new());
    let mut globals = NameSet::default();
    let mut any_recipe = false;

    // globals: the names main uses, plus every non-local name a recipe reads
    collect_program(prog, &mut globals);
    for s in prog {
        if let Stmt::Recipe(r) = s {
            any_recipe = true;
            let locals = locals_of(r);
            let mut used = NameSet::default();
            collect_program(&r.body, &mut used);
            for u in &used.0 {
                if !locals.has(u) {
                    globals.add(u);
                }
            }
        }
    }

    o.s("#include <stdio.h>\n#include <stdlib.h>\n#include <stddef.h>\n#include <stdint.h>\n#include <string.h>\n");
    o.s(runtime::PREAMBLE);
    // the argument stack serves recipe calls AND the left-to-right sequencing
    // of two library calls in one expression
    if any_recipe || !externs.is_empty() {
        o.s(runtime::RECIPE_RUNTIME);
    }
    emit_extern_adapters(&mut o, externs);

    // file-scope globals: zero-initialised, which is exactly cob_int(0)
    for g in &globals.0 {
        o.s(&format!("static CobValue cob_var_{};\n", g));
    }
    for s in prog {
        if let Stmt::Recipe(r) = s {
            o.s(&format!("static CobValue cob_fn_{}(void);\n", r.name));
        }
    }
    for s in prog {
        if let Stmt::Recipe(r) = s {
            emit_recipe(&mut o, r);
        }
    }

    o.s("int main(void) {\n");
    emit_program(&mut o, prog);
    for g in &globals.0 {
        o.s(&format!("cob_free(&cob_var_{});\n", g));
    }
    o.s("return 0;\n}\n");
    o.0
}

// ---- compile-time checking ----------------------------------------------------

fn find_recipe<'a>(top: &'a Program, name: &str) -> Option<&'a Recipe> {
    top.iter().find_map(|s| match s {
        Stmt::Recipe(r) if r.name == name => Some(&**r),
        _ => None,
    })
}

fn check_expr(top: &Program, e: &Expr) -> Result<(), ()> {
    match e {
        Expr::Call(c) => {
            let Some(r) = find_recipe(top, &c.name) else {
                eprintln!("[popcorn_comp] error: call to '{}', which is not a recipe or a library function (did you forget a `shuck`?)", c.name);
                return Err(());
            };
            if r.params.len() != c.args.len() {
                eprintln!(
                    "[popcorn_comp] error: recipe '{}' takes {} argument{} but {} {} given",
                    c.name,
                    r.params.len(),
                    if r.params.len() == 1 { "" } else { "s" },
                    c.args.len(),
                    if c.args.len() == 1 { "was" } else { "were" }
                );
                return Err(());
            }
            c.args.iter().try_for_each(|a| check_expr(top, a))
        }
        Expr::ExternCall(c) => c.args.iter().try_for_each(|a| check_expr(top, a)),
        Expr::Bin(_, l, r) => {
            check_expr(top, l)?;
            check_expr(top, r)
        }
        Expr::Harvest(x) => check_expr(top, x),
        _ => Ok(()),
    }
}

fn check_cond(top: &Program, c: &Cond) -> Result<(), ()> {
    check_expr(top, &c.left)?;
    if let Some(r) = &c.right {
        check_expr(top, r)?;
    }
    Ok(())
}

fn check_program(top: &Program, p: &Program, in_recipe: bool, at_top: bool) -> Result<(), ()> {
    for s in p {
        match s {
            Stmt::Pop(e) | Stmt::Set(_, e) | Stmt::Call(e) => check_expr(top, e)?,
            Stmt::Serve(e) => {
                if !in_recipe {
                    eprintln!("[popcorn_comp] error: 'serve' outside a recipe");
                    return Err(());
                }
                check_expr(top, e)?;
            }
            Stmt::While(c, b) => {
                check_cond(top, c)?;
                check_program(top, b, in_recipe, false)?;
            }
            Stmt::If(c, a, b) => {
                check_cond(top, c)?;
                check_program(top, a, in_recipe, false)?;
                check_program(top, b, in_recipe, false)?;
            }
            Stmt::Recipe(r) => {
                if !at_top {
                    eprintln!("[popcorn_comp] error: recipe '{}' must be defined at the top level", r.name);
                    return Err(());
                }
                check_program(top, &r.body, true, false)?;
            }
            Stmt::Trash(_) | Stmt::Extern(..) => {}
        }
    }
    Ok(())
}

/// No duplicate recipe, every call names a recipe with the right number of
/// arguments, `serve` only inside a recipe, recipes only at the top level.
pub fn check_recipes(top: &Program) -> Result<(), ()> {
    let mut seen: Vec<&str> = Vec::new();
    for s in top {
        if let Stmt::Recipe(r) = s {
            if seen.contains(&r.name.as_str()) {
                eprintln!("[popcorn_comp] error: recipe '{}' is defined twice", r.name);
                return Err(());
            }
            seen.push(&r.name);
        }
    }
    check_program(top, top, false, true)
}

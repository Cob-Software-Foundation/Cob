//! Lexing-by-lines and the recursive-descent parser. Diagnostics and their
//! wording are part of the language's observable behaviour (the test suite
//! pins them), so they are reproduced exactly.

use std::rc::Rc;

use cob_abi::num::strtol10;
use cob_abi::{ewrite, lossy};

use crate::ast::*;
use crate::consts::*;
use crate::fsutil;
use crate::session::Session;

type P<T> = Option<(T, usize)>;

fn at(s: &[u8], p: usize) -> u8 {
    s.get(p).copied().unwrap_or(0)
}
fn skip_blank(s: &[u8], mut p: usize) -> usize {
    while matches!(at(s, p), b' ' | b'\t') {
        p += 1;
    }
    p
}
fn is_ident_start(c: u8) -> bool {
    c.is_ascii_alphabetic() || c == b'_'
}
fn is_ident_char(c: u8) -> bool {
    c.is_ascii_alphanumeric() || c == b'_'
}
fn rest(s: &[u8], p: usize) -> String {
    lossy(s.get(p..).unwrap_or(b""))
}

fn parse_string_literal(s: &[u8], p: usize) -> P<Vec<u8>> {
    if at(s, p) != b'"' {
        return None;
    }
    let mut p = p + 1;
    let mut buf = Vec::new();
    while at(s, p) != 0 && at(s, p) != b'"' {
        let mut c = at(s, p);
        if c == b'\\' && at(s, p + 1) != 0 {
            c = match at(s, p + 1) {
                b'n' => b'\n',
                b't' => b'\t',
                b'"' => b'"',
                b'\\' => b'\\',
                other => other,
            };
            p += 2;
        } else {
            p += 1;
        }
        buf.push(c);
    }
    if at(s, p) != b'"' {
        return None;
    }
    Some((buf, p + 1))
}

fn parse_identifier(s: &[u8], p: usize) -> P<String> {
    if !is_ident_start(at(s, p)) {
        return None;
    }
    let start = p;
    let mut q = p;
    while is_ident_char(at(s, q)) {
        q += 1;
    }
    Some((String::from_utf8_lossy(&s[start..q]).into_owned(), q))
}

fn is_reserved_word(name: &str) -> bool {
    [
        KW_WHILE, KW_SET, KW_POP, KW_SHUCK, KW_HARVEST, KW_TRASH, KW_EXTERN, KW_IF, KW_ELIF,
        KW_ELSE, KW_RECIPE, KW_SERVE,
    ]
    .contains(&name)
}

/// One non-blank, non-comment source line.
pub struct PhysLine {
    pub indent: i32,
    pub content: Vec<u8>,
    pub line_no: i32,
}

/// Recognises the `_makecache=false` first line. Returns the offset to skip to.
pub fn strip_makecache_directive(src: &[u8]) -> Option<usize> {
    let line_end = src.iter().position(|&b| b == b'\n');
    let len = line_end.unwrap_or(src.len());
    let mut buf = Vec::new();
    for &c in &src[..len] {
        if buf.len() >= 127 {
            break;
        }
        if c == b' ' || c == b'\t' || c == b'\r' {
            continue;
        }
        buf.push(c.to_ascii_lowercase());
    }
    if buf == b"_makecache=false" {
        Some(line_end.map_or(len, |e| e + 1))
    } else {
        None
    }
}

pub fn split_lines(src: &[u8], start_line_no: i32) -> Result<Vec<PhysLine>, ()> {
    let mut out = Vec::new();
    let mut line_no = start_line_no;
    let mut start = 0usize;
    while start < src.len() {
        let end = src[start..].iter().position(|&b| b == b'\n').map(|e| start + e);
        let raw_end = end.unwrap_or(src.len());
        let mut line = &src[start..raw_end];
        if line.last() == Some(&b'\r') {
            line = &line[..line.len() - 1];
        }
        if line.first() == Some(&b'\t') {
            ewrite(&format!(
                "[cob_interp] indentation error at line {}: tabs are not allowed, use spaces\n",
                line_no
            ));
            return Err(());
        }
        let indent = line.iter().take_while(|&&b| b == b' ').count();
        let trimmed = &line[indent..];
        if !(trimmed.is_empty() || trimmed[0] == b'#') {
            out.push(PhysLine { indent: indent as i32, content: trimmed.to_vec(), line_no });
        }
        line_no += 1;
        start = match end {
            Some(e) => e + 1,
            None => src.len(),
        };
    }
    Ok(out)
}

impl Session {
    // ------------------------------------------------------------------
    // expressions
    // ------------------------------------------------------------------
    fn parse_named_call(&self, is_extern: bool, name: String, s: &[u8], p: usize) -> P<Expr> {
        let mut p = skip_blank(s, p);
        let mut args: Vec<Expr> = Vec::new();
        if at(s, p) != b')' {
            loop {
                let (arg, np) = self.parse_expr(s, p)?;
                if args.len() >= MAX_EXTERN_ARGS {
                    return None;
                }
                args.push(arg);
                p = skip_blank(s, np);
                if at(s, p) == b',' {
                    p = skip_blank(s, p + 1);
                    continue;
                }
                break;
            }
            if at(s, p) != b')' {
                return None;
            }
        }
        let call = CallExpr { name, args, line: self.parse_line };
        let e = if is_extern { Expr::ExternCall(call) } else { Expr::Call(call) };
        Some((e, p + 1))
    }

    fn parse_primary(&self, s: &[u8], p: usize) -> P<Expr> {
        let p = skip_blank(s, p);
        let c = at(s, p);
        if c == b'"' {
            let (text, after) = parse_string_literal(s, p)?;
            return Some((Expr::Str(text), after));
        }
        let kw = KW_HARVEST.as_bytes();
        if s.get(p..).map_or(false, |r| r.starts_with(kw)) {
            let after_kw = skip_blank(s, p + kw.len());
            if at(s, after_kw) == b'(' {
                let after_open = skip_blank(s, after_kw + 1);
                let (bytes_expr, after_expr) = self.parse_expr(s, after_open)?;
                let after_expr = skip_blank(s, after_expr);
                if at(s, after_expr) != b')' {
                    return None;
                }
                return Some((Expr::Harvest(Box::new(bytes_expr)), after_expr + 1));
            }
        }
        if c.is_ascii_digit() || (c == b'-' && at(s, p + 1).is_ascii_digit()) {
            let (v, used) = strtol10(&s[p..]);
            return Some((Expr::Num(v), p + used));
        }
        if is_ident_start(c) {
            let (name, after) = parse_identifier(s, p)?;
            let paren = skip_blank(s, after);
            if self.extern_decl.iter().any(|n| *n == name) && at(s, paren) == b'(' {
                return self.parse_named_call(true, name, s, paren + 1);
            }
            if at(s, paren) == b'(' {
                return self.parse_named_call(false, name, s, paren + 1);
            }
            return Some((Expr::Var(name), after));
        }
        None
    }

    fn parse_term(&self, s: &[u8], p: usize) -> P<Expr> {
        let (mut left, mut p) = self.parse_primary(s, p)?;
        loop {
            let q = skip_blank(s, p);
            let c = at(s, q);
            if c == b'*' || c == b'/' {
                let (right, after) = self.parse_primary(s, q + 1)?;
                left = Expr::Bin(c, Box::new(left), Box::new(right));
                p = after;
            } else {
                break;
            }
        }
        Some((left, p))
    }

    pub(crate) fn parse_expr(&self, s: &[u8], p: usize) -> P<Expr> {
        let (mut left, mut p) = self.parse_term(s, p)?;
        loop {
            let q = skip_blank(s, p);
            let c = at(s, q);
            if c == b'+' || c == b'-' {
                let (right, after) = self.parse_term(s, q + 1)?;
                left = Expr::Bin(c, Box::new(left), Box::new(right));
                p = after;
            } else {
                break;
            }
        }
        Some((left, p))
    }

    fn parse_comparator(s: &[u8], p: usize) -> Option<(CondOp, usize)> {
        let q = skip_blank(s, p);
        let (a, b) = (at(s, q), at(s, q + 1));
        Some(match (a, b) {
            (b'=', b'=') => (CondOp::Eq, q + 2),
            (b'!', b'=') => (CondOp::Ne, q + 2),
            (b'<', b'=') => (CondOp::Le, q + 2),
            (b'>', b'=') => (CondOp::Ge, q + 2),
            (b'<', _) => (CondOp::Lt, q + 1),
            (b'>', _) => (CondOp::Gt, q + 1),
            _ => return None,
        })
    }

    fn parse_condition(&self, s: &[u8], p: usize) -> P<Cond> {
        let (left, mut p) = self.parse_expr(s, p)?;
        let (op, right) = match Self::parse_comparator(s, p) {
            Some((op, after_op)) => {
                let (r, np) = self.parse_expr(s, after_op)?;
                p = np;
                (op, Some(r))
            }
            None => (CondOp::Truthy, None),
        };
        Some((Cond { left, op, right }, p))
    }

    /// Parses a whole expression the REPL typed on its own (None if there is trailing junk).
    pub fn parse_expr_text(&self, text: &[u8]) -> Option<Expr> {
        let (e, p) = self.parse_expr(text, 0)?;
        if at(text, skip_blank(text, p)) != 0 {
            return None;
        }
        Some(e)
    }

    // ------------------------------------------------------------------
    // statements
    // ------------------------------------------------------------------
    fn parse_pop_statement(&self, trimmed: &[u8], line_no: i32) -> Result<Expr, ()> {
        let p = skip_blank(trimmed, KW_POP.len());
        if at(trimmed, p) != b'(' {
            ewrite(&format!("[cob_interp] syntax error at line {}: expected '(' after pop\n", line_no));
            return Err(());
        }
        let p = skip_blank(trimmed, p + 1);
        let Some((expr, p)) = self.parse_expr(trimmed, p) else {
            ewrite(&format!("[cob_interp] syntax error at line {}: malformed expression in pop()\n", line_no));
            return Err(());
        };
        let p = skip_blank(trimmed, p);
        if at(trimmed, p) != b')' {
            ewrite(&format!("[cob_interp] syntax error at line {}: expected ')' to close pop(\n", line_no));
            return Err(());
        }
        Ok(expr)
    }

    fn parse_set_statement(&self, after_kw: &[u8], line_no: i32) -> Result<(String, Expr), ()> {
        let p = skip_blank(after_kw, 0);
        let Some((name, p)) = parse_identifier(after_kw, p) else {
            ewrite(&format!("[cob_interp] syntax error at line {}: expected a variable name after 'set'\n", line_no));
            return Err(());
        };
        let p = skip_blank(after_kw, p);
        if at(after_kw, p) != b'=' {
            ewrite(&format!("[cob_interp] syntax error at line {}: expected '=' in set statement\n", line_no));
            return Err(());
        }
        let Some((expr, _)) = self.parse_expr(after_kw, p + 1) else {
            ewrite(&format!("[cob_interp] syntax error at line {}: malformed expression in set statement\n", line_no));
            return Err(());
        };
        Ok((name, expr))
    }

    fn parse_block_header(&self, after_kw: &[u8], line_no: i32, kw: &str) -> Result<Cond, ()> {
        let Some((cond, p)) = self.parse_condition(after_kw, 0) else {
            ewrite(&format!("[cob_interp] syntax error at line {}: malformed condition in {} statement\n", line_no, kw));
            return Err(());
        };
        let p = skip_blank(after_kw, p);
        if at(after_kw, p) != b':' {
            ewrite(&format!("[cob_interp] syntax error at line {}: expected ':' to open {} block\n", line_no, kw));
            return Err(());
        }
        Ok(cond)
    }

    fn parse_trash_statement(&self, after_kw: &[u8], line_no: i32) -> Result<String, ()> {
        let p = skip_blank(after_kw, 0);
        if at(after_kw, p) != b'(' {
            ewrite(&format!("[cob_interp] syntax error at line {}: expected '(' after trash\n", line_no));
            return Err(());
        }
        let p = skip_blank(after_kw, p + 1);
        let Some((name, p)) = parse_identifier(after_kw, p) else {
            ewrite(&format!("[cob_interp] syntax error at line {}: expected a variable name in trash()\n", line_no));
            return Err(());
        };
        let p = skip_blank(after_kw, p);
        if at(after_kw, p) != b')' {
            ewrite(&format!("[cob_interp] syntax error at line {}: expected ')' to close trash(\n", line_no));
            return Err(());
        }
        Ok(name)
    }

    fn parse_extern_statement(&mut self, after_kw: &[u8], line_no: i32) -> Result<(String, Vec<u8>), ()> {
        let s = after_kw;
        let p = skip_blank(s, 0);
        let Some((name, p)) = parse_identifier(s, p) else {
            ewrite(&format!("[cob_interp] syntax error at line {}: expected a function name after 'extern'\n", line_no));
            return Err(());
        };
        if is_reserved_word(&name) {
            ewrite(&format!("[cob_interp] syntax error at line {}: extern cannot reuse the reserved name '{}'\n", line_no, name));
            return Err(());
        }
        if self.func_exists(&name) {
            ewrite(&format!("[cob_interp] syntax error at line {}: extern cannot reuse the name '{}' of a recipe\n", line_no, name));
            return Err(());
        }
        let p = skip_blank(s, p);
        if at(s, p) != b'(' {
            ewrite(&format!("[cob_interp] syntax error at line {}: expected '(' after extern {}\n", line_no, name));
            return Err(());
        }
        let mut p = skip_blank(s, p + 1);
        if at(s, p) != b')' {
            let mut nparams = 0;
            loop {
                let Some((_param, np)) = parse_identifier(s, p) else {
                    ewrite(&format!("[cob_interp] syntax error at line {}: expected a parameter name in extern {}(...)\n", line_no, name));
                    return Err(());
                };
                p = np;
                nparams += 1;
                if nparams > MAX_EXTERN_ARGS {
                    ewrite(&format!("[cob_interp] syntax error at line {}: extern {} has too many parameters (max {})\n", line_no, name, MAX_EXTERN_ARGS));
                    return Err(());
                }
                p = skip_blank(s, p);
                if at(s, p) == b',' {
                    p = skip_blank(s, p + 1);
                    continue;
                }
                break;
            }
            if at(s, p) != b')' {
                ewrite(&format!("[cob_interp] syntax error at line {}: expected ')' to close extern {}(\n", line_no, name));
                return Err(());
            }
        }
        let p = skip_blank(s, p + 1);
        let kw = KW_FROM.as_bytes();
        if !s.get(p..).map_or(false, |r| r.starts_with(kw)) || is_ident_char(at(s, p + kw.len())) {
            ewrite(&format!("[cob_interp] syntax error at line {}: expected 'from \"<library>\"' after extern {}(...)\n", line_no, name));
            return Err(());
        }
        let p = skip_blank(s, p + kw.len());
        let lib = match parse_string_literal(s, p) {
            Some((lib, np)) if !lib.is_empty() => {
                let q = skip_blank(s, np);
                if at(s, q) != 0 {
                    ewrite(&format!("[cob_interp] syntax error at line {}: unexpected text after extern statement: {}\n", line_no, rest(s, q)));
                    return Err(());
                }
                lib
            }
            _ => {
                ewrite(&format!("[cob_interp] syntax error at line {}: expected a non-empty library path string after 'from'\n", line_no));
                return Err(());
            }
        };
        if !self.extern_decl.iter().any(|n| *n == name) {
            self.extern_decl.push(name.clone());
        }
        Ok((name, lib))
    }

    fn parse_recipe_header(&self, after_kw: &[u8], line_no: i32) -> Result<(String, Vec<String>), ()> {
        let s = after_kw;
        let p = skip_blank(s, 0);
        let Some((name, p)) = parse_identifier(s, p) else {
            ewrite(&format!("[cob_interp] syntax error at line {}: expected a name after 'recipe'\n", line_no));
            return Err(());
        };
        if is_reserved_word(&name) {
            ewrite(&format!("[cob_interp] syntax error at line {}: '{}' is a Cob keyword and cannot name a recipe\n", line_no, name));
            return Err(());
        }
        let p = skip_blank(s, p);
        if at(s, p) != b'(' {
            ewrite(&format!("[cob_interp] syntax error at line {}: expected '(' after recipe {}\n", line_no, name));
            return Err(());
        }
        let mut p = skip_blank(s, p + 1);
        let mut params: Vec<String> = Vec::new();
        if at(s, p) != b')' {
            loop {
                let Some((param, np)) = parse_identifier(s, p) else {
                    ewrite(&format!("[cob_interp] syntax error at line {}: expected a parameter name in recipe {}\n", line_no, name));
                    return Err(());
                };
                p = np;
                if params.iter().any(|x| *x == param) {
                    ewrite(&format!("[cob_interp] syntax error at line {}: recipe {} has two parameters named '{}'\n", line_no, name, param));
                    return Err(());
                }
                if params.len() >= MAX_EXTERN_ARGS {
                    ewrite(&format!("[cob_interp] syntax error at line {}: recipe {} has too many parameters (max {})\n", line_no, name, MAX_EXTERN_ARGS));
                    return Err(());
                }
                params.push(param);
                p = skip_blank(s, p);
                if at(s, p) == b',' {
                    p = skip_blank(s, p + 1);
                    continue;
                }
                break;
            }
            if at(s, p) != b')' {
                ewrite(&format!("[cob_interp] syntax error at line {}: expected ',' or ')' in the parameters of recipe {}\n", line_no, name));
                return Err(());
            }
        }
        let p = skip_blank(s, p + 1);
        if at(s, p) != b':' {
            ewrite(&format!("[cob_interp] syntax error at line {}: expected ':' at the end of the recipe line\n", line_no));
            return Err(());
        }
        let p = skip_blank(s, p + 1);
        if at(s, p) != 0 {
            ewrite(&format!("[cob_interp] syntax error at line {}: unexpected text after ':' on the recipe line\n", line_no));
            return Err(());
        }
        Ok((name, params))
    }

    // ------------------------------------------------------------------
    // shuck
    // ------------------------------------------------------------------
    fn load_library(&mut self, name: &str, line_no: i32) -> Result<Program, ()> {
        if self.shuck_stack.iter().any(|n| n == name) {
            ewrite(&format!("[cob_interp] error at line {}: circular shuck detected for '{}'\n", line_no, name));
            return Err(());
        }
        if self.shuck_stack.len() >= MAX_SHUCK_DEPTH {
            ewrite(&format!("[cob_interp] error at line {}: shuck nesting too deep (max {})\n", line_no, MAX_SHUCK_DEPTH));
            return Err(());
        }
        let path_a = format!("{}{}", name, SOURCE_EXTENSION);
        let path_b = format!("{0}{1}{2}{1}{2}{3}", FARMER_MODULES_DIR, PATH_SEP, name, SOURCE_EXTENSION);
        let mut path_c = String::new();
        let mut data = fsutil::read_all(&path_a).or_else(|| fsutil::read_all(&path_b));
        if data.is_none() {
            if let Some(dir) = stdlib_dir() {
                path_c = format!("{}{}{}{}", dir.display(), PATH_SEP, name, SOURCE_EXTENSION);
                data = fsutil::read_all(&path_c);
            }
        }
        // wrappers compiled into this build (the browser has no files at all)
        if data.is_none() {
            if let Some((_, src)) = self.embedded_wrappers.iter().find(|(n, _)| *n == name) {
                data = Some(src.as_bytes().to_vec());
            }
        }
        let Some(data) = data else {
            let mut msg = format!(
                "[cob_interp] error at line {}: shuck '{}' failed -- tried '{}' and '{}'",
                line_no, name, path_a, path_b
            );
            if !path_c.is_empty() {
                msg.push_str(&format!(" and '{}'", path_c));
            }
            msg.push('\n');
            ewrite(&msg);
            return Err(());
        };
        let source = fsutil::as_c_text(data);
        self.shuck_stack.push(name.to_string());
        let result = (|| {
            let lines = split_lines(&source, 1)?;
            let mut pos = 0usize;
            self.parse_block(&lines, &mut pos, -1)
        })();
        self.shuck_stack.pop();
        result
    }

    // ------------------------------------------------------------------
    // blocks
    // ------------------------------------------------------------------
    fn parse_if_chain(&mut self, lines: &[PhysLine], pos: &mut usize, kw_len: usize, kw_name: &str) -> Result<Stmt, ()> {
        let line_no = lines[*pos].line_no;
        let this_indent = lines[*pos].indent;
        self.parse_line = line_no;
        let cond = self.parse_block_header(&lines[*pos].content[kw_len..], line_no, kw_name)?;
        *pos += 1;
        let body = self.parse_block(lines, pos, this_indent)?;
        let mut else_body: Program = Vec::new();
        if *pos < lines.len() && lines[*pos].indent == this_indent {
            let next = lines[*pos].content.clone();
            let elif = KW_ELIF.as_bytes();
            let els = KW_ELSE.as_bytes();
            if next.starts_with(elif) && at(&next, elif.len()) == b' ' {
                let nested = self.parse_if_chain(lines, pos, elif.len(), KW_ELIF)?;
                else_body.push(nested);
            } else if next.starts_with(els) {
                let p = skip_blank(&next, els.len());
                if at(&next, p) == b':' && at(&next, skip_blank(&next, p + 1)) == 0 {
                    *pos += 1;
                    else_body = self.parse_block(lines, pos, this_indent)?;
                }
            }
        }
        Ok(Stmt::If(cond, body, else_body))
    }

    pub(crate) fn parse_block(&mut self, lines: &[PhysLine], pos: &mut usize, parent_indent: i32) -> Result<Program, ()> {
        let mut out: Program = Vec::new();
        while *pos < lines.len() {
            let indent = lines[*pos].indent;
            if indent <= parent_indent {
                break;
            }
            let trimmed: Vec<u8> = lines[*pos].content.clone();
            let t = &trimmed[..];
            let line_no = lines[*pos].line_no;
            self.parse_line = line_no;

            let kw = KW_POP.as_bytes();
            if t.starts_with(kw) && matches!(at(t, kw.len()), b'(' | b' ') {
                let e = self.parse_pop_statement(t, line_no)?;
                out.push(Stmt::Pop(e));
                *pos += 1;
                continue;
            }

            let kw = KW_SET.as_bytes();
            if t.starts_with(kw) && at(t, kw.len()) == b' ' {
                let (name, e) = self.parse_set_statement(&t[kw.len()..], line_no)?;
                out.push(Stmt::Set(name, e));
                *pos += 1;
                continue;
            }

            let kw = KW_WHILE.as_bytes();
            if t.starts_with(kw) && at(t, kw.len()) == b' ' {
                let cond = self.parse_block_header(&t[kw.len()..], line_no, KW_WHILE)?;
                *pos += 1;
                let body = self.parse_block(lines, pos, indent)?;
                out.push(Stmt::While(cond, body));
                continue;
            }

            let kw = KW_IF.as_bytes();
            if t.starts_with(kw) && at(t, kw.len()) == b' ' {
                let s = self.parse_if_chain(lines, pos, kw.len(), KW_IF)?;
                out.push(s);
                continue;
            }

            let els = KW_ELSE.as_bytes();
            let elif = KW_ELIF.as_bytes();
            if (t.starts_with(els) && matches!(at(t, els.len()), b':' | b' ' | 0))
                || (t.starts_with(elif) && at(t, elif.len()) == b' ')
            {
                ewrite(&format!(
                    "[cob_interp] syntax error at line {}: '{}' without a matching 'if' at the same indentation\n",
                    line_no,
                    if at(t, 2) == b's' { KW_ELSE } else { KW_ELIF }
                ));
                return Err(());
            }

            let kw = KW_TRASH.as_bytes();
            if t.starts_with(kw) && at(t, kw.len()) == b'(' {
                let name = self.parse_trash_statement(&t[kw.len()..], line_no)?;
                out.push(Stmt::Trash(name));
                *pos += 1;
                continue;
            }

            let kw = KW_EXTERN.as_bytes();
            if t.starts_with(kw) && at(t, kw.len()) == b' ' {
                let (name, lib) = self.parse_extern_statement(&t[kw.len()..], line_no)?;
                out.push(Stmt::Extern(name, lib));
                *pos += 1;
                continue;
            }

            let kw = KW_RECIPE.as_bytes();
            if t.starts_with(kw) && at(t, kw.len()) == b' ' {
                if parent_indent != -1 {
                    ewrite(&format!("[cob_interp] syntax error at line {}: a recipe can only be defined at the top level of a program, not inside another block\n", line_no));
                    return Err(());
                }
                let (rname, rparams) = self.parse_recipe_header(&t[kw.len()..], line_no)?;
                *pos += 1;
                self.recipe_depth += 1;
                let body = self.parse_block(lines, pos, indent);
                self.recipe_depth -= 1;
                let body = body?;
                if body.is_empty() {
                    ewrite(&format!("[cob_interp] syntax error at line {}: recipe {} needs an indented body\n", line_no, rname));
                    return Err(());
                }
                out.push(Stmt::Recipe(Rc::new(Recipe { name: rname, params: rparams, body, line: line_no })));
                continue;
            }

            let kw = KW_SERVE.as_bytes();
            if t.starts_with(kw) && matches!(at(t, kw.len()), b' ' | 0) {
                if self.recipe_depth == 0 {
                    ewrite(&format!("[cob_interp] syntax error at line {}: 'serve' can only be used inside a recipe\n", line_no));
                    return Err(());
                }
                let p = skip_blank(t, kw.len());
                let e = if at(t, p) == 0 {
                    Expr::Num(0)
                } else {
                    let Some((e, np)) = self.parse_expr(t, p) else {
                        ewrite(&format!("[cob_interp] syntax error at line {}: bad expression after 'serve'\n", line_no));
                        return Err(());
                    };
                    let q = skip_blank(t, np);
                    if at(t, q) != 0 {
                        ewrite(&format!("[cob_interp] syntax error at line {}: unexpected text after the serve expression: {}\n", line_no, rest(t, q)));
                        return Err(());
                    }
                    e
                };
                out.push(Stmt::Serve(e));
                *pos += 1;
                continue;
            }

            let kw = KW_SHUCK.as_bytes();
            if t.starts_with(kw) && at(t, kw.len()) == b' ' {
                let p = skip_blank(t, kw.len());
                let Some((lib_name, _)) = parse_identifier(t, p) else {
                    ewrite(&format!("[cob_interp] syntax error at line {}: expected a library name after 'shuck'\n", line_no));
                    return Err(());
                };
                let lib_prog = self.load_library(&lib_name, line_no)?;
                if parent_indent != -1 && program_has_recipe(&lib_prog) {
                    ewrite(&format!("[cob_interp] error at line {}: shuck '{}' defines recipes, so it must be at the top level of the program\n", line_no, lib_name));
                    return Err(());
                }
                out.extend(lib_prog);
                *pos += 1;
                continue;
            }

            // a call standing alone
            if is_ident_start(at(t, 0)) {
                let mut q = 0;
                while is_ident_char(at(t, q)) {
                    q += 1;
                }
                q = skip_blank(t, q);
                if at(t, q) == b'(' {
                    let Some((e, np)) = self.parse_expr(t, 0) else {
                        ewrite(&format!("[cob_interp] syntax error at line {}: malformed call: {}\n", line_no, lossy(t)));
                        return Err(());
                    };
                    let p = skip_blank(t, np);
                    if at(t, p) != 0 {
                        ewrite(&format!("[cob_interp] syntax error at line {}: unexpected text after the call: {}\n", line_no, rest(t, p)));
                        return Err(());
                    }
                    if !matches!(e, Expr::Call(_) | Expr::ExternCall(_)) {
                        ewrite(&format!("[cob_interp] syntax error at line {}: only a call to a recipe or a library function can stand alone as a statement: {}\n", line_no, lossy(t)));
                        return Err(());
                    }
                    out.push(Stmt::Call(e));
                    *pos += 1;
                    continue;
                }
            }

            ewrite(&format!("[cob_interp] syntax error at line {}: unrecognized statement: {}\n", line_no, lossy(t)));
            return Err(());
        }
        Ok(out)
    }

    /// Parses a whole source text. Returns the program and whether a
    /// `_makecache=false` directive disabled caching.
    pub fn parse_source(&mut self, source: &[u8]) -> Result<(Program, bool), ()> {
        let (src, disable) = match strip_makecache_directive(source) {
            Some(off) => (&source[off..], true),
            None => (source, false),
        };
        let lines = split_lines(src, if disable { 2 } else { 1 })?;
        let mut pos = 0usize;
        let prog = self.parse_block(&lines, &mut pos, -1)?;
        Ok((prog, disable))
    }
}

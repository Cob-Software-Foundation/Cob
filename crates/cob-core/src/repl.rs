//! The REPL (`cob_interp` with no file, or `-i` after one).
//!
//! Reads Cob a line at a time. A line ending in ':' starts a block: keep
//! typing indented lines and finish with a blank line. A bare expression
//! prints its value, like Python; a recipe call that ends without `serve`
//! prints nothing. Variables and recipes persist between inputs. Commands
//! start with ':' (try :help). Prompts and the banner only show on a real
//! terminal, so `cob_interp --repl < script.txt` is scriptable.

use std::io::IsTerminal;

use cob_abi::cstdio::{flush_stdout, prompt, read_line, write_stdout};
use cob_abi::{ewrite, lossy};

use crate::ast::*;
use crate::consts::*;
use crate::fsutil;
use crate::parse::split_lines;
use crate::session::{Session, Value};

fn out(s: &str) {
    write_stdout(s.as_bytes());
}

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

/// numbers plain, strings in quotes (so "5" and 5 look different)
pub fn print_value(v: &Value) {
    match v {
        Value::Str(s) => {
            let mut o: Vec<u8> = vec![b'"'];
            for &c in s {
                match c {
                    b'"' => o.extend_from_slice(b"\\\""),
                    b'\\' => o.extend_from_slice(b"\\\\"),
                    b'\n' => o.extend_from_slice(b"\\n"),
                    b'\t' => o.extend_from_slice(b"\\t"),
                    _ => o.push(c),
                }
            }
            o.push(b'"');
            o.push(b'\n');
            write_stdout(&o);
        }
        Value::Int(n) => out(&format!("{}\n", n)),
    }
}

fn is_statement_line(t: &[u8]) -> bool {
    [
        KW_POP, KW_SET, KW_WHILE, KW_IF, KW_ELIF, KW_ELSE, KW_SHUCK, KW_RECIPE, KW_SERVE, KW_EXTERN, KW_TRASH,
    ]
    .iter()
    .any(|kw| t.starts_with(kw.as_bytes()) && !is_ident_char(at(t, kw.len())))
}

/// `x = 5` -- a common slip; Cob wants `set x = 5`
fn looks_like_assignment(t: &[u8]) -> bool {
    if !is_ident_start(at(t, 0)) {
        return false;
    }
    let mut q = 0;
    while is_ident_char(at(t, q)) {
        q += 1;
    }
    q = skip_blank(t, q);
    at(t, q) == b'=' && at(t, q + 1) != b'='
}

impl Session {
    /// Registers + runs an already-parsed program.
    pub fn repl_run_program(&mut self, prog: &Program, lenient: bool) {
        if self.recipes_validate(prog, true).is_err() || self.check_calls(prog, lenient).is_err() {
            return;
        }
        if !self.no_gc && program_uses_harvest_or_trash(prog) {
            ewrite("[cob_interp] error: harvest()/trash() need the REPL started with --no-gc\n");
            return;
        }
        self.recipes_register(prog);
        self.execute(prog);
        self.ret = Value::Int(0);
    }

    fn repl_eval_text(&mut self, src: &[u8]) {
        let t0 = skip_blank(src, 0);
        let t = &src[t0..];
        let multiline = match t.iter().position(|&b| b == b'\n') {
            Some(i) => i + 1 < t.len(),
            None => false,
        };

        // 1. a lone expression prints its value
        if !multiline && !is_statement_line(t) {
            self.parse_line = 0; // REPL input has no line numbers to report
            if let Some((e, p)) = self.parse_expr(t, 0) {
                let p = skip_blank(t, p);
                if matches!(at(t, p), 0 | b'\n') {
                    let is_call = matches!(e, Expr::Call(_));
                    if self.check_calls_expr(&e, false).is_err() {
                        return;
                    }
                    if !self.no_gc && e.uses_harvest() {
                        ewrite("[cob_interp] error: harvest() needs the REPL started with --no-gc\n");
                        return;
                    }
                    self.fell_off = false;
                    let v = self.eval_expr(&e);
                    if !(is_call && self.fell_off) {
                        print_value(&v);
                    }
                    return;
                }
            }
        }
        // 2. otherwise one or more statements
        if !multiline && looks_like_assignment(t) {
            let mut tl = t.len();
            while tl > 0 && (t[tl - 1] == b'\n' || t[tl - 1] == b'\r') {
                tl -= 1;
            }
            ewrite(&format!(
                "[cob_interp] syntax error: unrecognized statement: {}\n  hint: assign with `set`, e.g. `set x = 5`\n",
                lossy(&t[..tl])
            ));
            return;
        }
        let Ok(lines) = split_lines(src, 1) else { return };
        self.recipe_depth = 0;
        let mut pos = 0usize;
        let Ok(prog) = self.parse_block(&lines, &mut pos, -1) else { return };
        self.repl_run_program(&prog, true);
    }

    fn repl_help() {
        out("Type Cob at the prompt. A line ending in ':' starts a block -- finish it with a blank line.\n");
        out("A bare expression prints its value:   1 + 2    name    greet(\"Ada\")\n");
        out("Commands:\n");
        out("  :help             this text\n");
        out("  :vars             the variables you have set\n");
        out("  :recipes          the recipes you have defined (also :funcs)\n");
        out("  :load <file.cob>  run a file inside this session\n");
        out("  :reset            forget all variables, recipes and loaded libraries\n");
        out("  :quit             leave (or press Ctrl-D)\n");
        out("Libraries: shuck porch | cornmath | kernels | almanac | granary | pantry | barn | silo\n");
    }

    /// Returns true to leave the REPL.
    fn repl_command(&mut self, cmd: &[u8]) -> bool {
        let mut arg = 0;
        while at(cmd, arg) != 0 && at(cmd, arg) != b' ' && at(cmd, arg) != b'\t' {
            arg += 1;
        }
        let arg = skip_blank(cmd, arg);
        let mut n = arg;
        while n > 0 && (cmd[n - 1] == b' ' || cmd[n - 1] == b'\t') {
            n -= 1;
        }
        let word = &cmd[..n];
        let arg_bytes = &cmd[arg.min(cmd.len())..];
        let is = |w: &str| word == w.as_bytes();
        if is(":quit") || is(":q") || is(":exit") {
            return true;
        }
        if is(":help") || is(":h") || is(":?") {
            Self::repl_help();
            return false;
        }
        if is(":vars") {
            if self.vars.items.is_empty() {
                out("(no variables yet)\n");
            }
            for (name, v) in &self.vars.items {
                out(&format!("{} = ", name));
                print_value(v);
            }
            return false;
        }
        if is(":recipes") || is(":funcs") {
            if self.funcs.is_empty() {
                out("(no recipes yet)\n");
            }
            for f in &self.funcs {
                out(&format!("recipe {}({})\n", f.def.name, f.def.params.join(", ")));
            }
            return false;
        }
        if is(":reset") {
            self.reset();
            out("(reset)\n");
            return false;
        }
        if is(":load") {
            if arg_bytes.is_empty() {
                ewrite("[cob_interp] usage: :load <file.cob>\n");
                return false;
            }
            let path = fsutil::path_from_bytes(arg_bytes);
            let Some(data) = fsutil::read_all(&path) else {
                ewrite(&format!("[cob_interp] error: could not read '{}'\n", lossy(arg_bytes)));
                return false;
            };
            self.recipe_depth = 0;
            if let Ok((prog, _)) = self.parse_source(&fsutil::as_c_text(data)) {
                self.repl_run_program(&prog, false);
            }
            return false;
        }
        ewrite(&format!("[cob_interp] unknown command '{}' -- try :help\n", lossy(cmd)));
        false
    }

    pub fn repl(&mut self, banner: bool) -> i32 {
        let interactive = std::io::stdin().is_terminal();
        if interactive && banner {
            out(&format!("Cob {} REPL -- :help for help, :quit or Ctrl-D to leave\n", VERSION_STRING));
        }
        loop {
            prompt(if interactive { b"cob> " } else { b"" });
            let Some(line) = read_line() else {
                if interactive {
                    out("\n");
                }
                break;
            };
            let t0 = skip_blank(&line, 0);
            let t = &line[t0..];
            if t.is_empty() {
                continue;
            }
            if t[0] == b':' {
                let quit = self.repl_command(t);
                flush_stdout();
                if quit {
                    break;
                }
                continue;
            }
            let mut len = t.len();
            while len > 0 && (t[len - 1] == b' ' || t[len - 1] == b'\t') {
                len -= 1;
            }
            let starts_block = len > 0 && t[len - 1] == b':';
            let mut text = line.clone();
            text.push(b'\n');
            if starts_block {
                // a block: read indented lines until a blank line
                loop {
                    prompt(if interactive { b"...  " } else { b"" });
                    let Some(next) = read_line() else { break };
                    if at(&next, skip_blank(&next, 0)) == 0 {
                        break;
                    }
                    text.extend_from_slice(&next);
                    text.push(b'\n');
                }
            }
            self.repl_eval_text(&text);
            flush_stdout();
        }
        0
    }
}

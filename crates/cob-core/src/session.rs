//! Everything one interpreter run owns: variables, memory handles, the recipe
//! registry, extern declarations and bound native functions. (In the C
//! version these were file-level globals; the REPL keeps one `Session` alive
//! across lines, a script run creates one and drops it.)

use std::rc::Rc;

use cob_abi::{ewrite, lossy, CobFFIFunc};

use crate::ast::*;
#[cfg(not(target_arch = "wasm32"))]
use crate::consts::*;

#[derive(Clone, Debug, PartialEq)]
pub enum Value {
    Int(i64),
    Str(Vec<u8>),
}

#[derive(Default)]
pub struct VarTable {
    pub items: Vec<(String, Value)>,
}

impl VarTable {
    pub fn new() -> Self {
        VarTable { items: Vec::new() }
    }
    pub fn get(&self, name: &str) -> Option<Value> {
        self.items.iter().find(|(n, _)| n == name).map(|(_, v)| v.clone())
    }
    pub fn set(&mut self, name: &str, value: Value) {
        if let Some(slot) = self.items.iter_mut().find(|(n, _)| n == name) {
            slot.1 = value;
        } else {
            self.items.push((name.to_string(), value));
        }
    }
}

/// `harvest()` / `trash()` memory handles: 1-based ids, never raw pointers.
#[derive(Default)]
pub struct HandleTable {
    /// `None` once freed.
    pub items: Vec<Option<Vec<u8>>>,
}

impl HandleTable {
    /// Returns the new id (>= 1), or 0 if the allocation failed.
    pub fn alloc(&mut self, bytes: usize) -> i64 {
        let mut buf: Vec<u8> = Vec::new();
        if buf.try_reserve_exact(bytes.max(1)).is_err() {
            return 0;
        }
        buf.resize(bytes, 0);
        self.items.push(Some(buf));
        self.items.len() as i64
    }
    /// Err if the id is out of range or already freed.
    pub fn free(&mut self, id: i64) -> Result<(), ()> {
        if id < 1 || id as usize > self.items.len() {
            return Err(());
        }
        let slot = &mut self.items[id as usize - 1];
        if slot.is_none() {
            return Err(());
        }
        *slot = None;
        Ok(())
    }
}

pub struct FuncEntry {
    pub def: Rc<Recipe>,
    pub locals: Vec<String>,
}

/// What keeps a bound native function's library loaded (nothing in the browser,
/// where modules are linked in statically).
#[cfg(not(target_arch = "wasm32"))]
pub type LibHandle = libloading::Library;
#[cfg(target_arch = "wasm32")]
pub type LibHandle = ();

pub struct ExternBinding {
    pub name: String,
    pub func: Option<CobFFIFunc>,
    /// Kept alive for as long as `func` may be called.
    pub lib: Option<LibHandle>,
}

pub struct Session {
    pub vars: VarTable,
    pub handles: HandleTable,
    pub no_gc: bool,
    // functions
    pub locals: Option<VarTable>,
    pub call_depth: usize,
    pub ret: Value,
    pub fell_off: bool,
    pub funcs: Vec<FuncEntry>,
    // C API / FFI
    pub extern_decl: Vec<String>,
    pub externs: Vec<ExternBinding>,
    missing_libs: Vec<String>,
    /// Standard-library functions linked into this build, bound by name
    /// (the browser build: it cannot dlopen).
    pub static_externs: &'static [(&'static str, CobFFIFunc)],
    /// `.cob` wrapper sources compiled into this build, tried by `shuck` after
    /// the files on disk (the browser build has no disk).
    pub embedded_wrappers: &'static [(&'static str, &'static str)],
    // parser state
    pub(crate) parse_line: i32,
    pub(crate) recipe_depth: i32,
    pub(crate) shuck_stack: Vec<String>,
}

impl Session {
    pub fn new(no_gc: bool) -> Session {
        Session {
            vars: VarTable::new(),
            handles: HandleTable::default(),
            no_gc,
            locals: None,
            call_depth: 0,
            ret: Value::Int(0),
            fell_off: false,
            funcs: Vec::new(),
            extern_decl: Vec::new(),
            externs: Vec::new(),
            missing_libs: Vec::new(),
            static_externs: &[],
            embedded_wrappers: &[],
            parse_line: 0,
            recipe_depth: 0,
            shuck_stack: Vec::new(),
        }
    }

    // ---- recipe registry ---------------------------------------------
    pub fn func_find(&self, name: &str) -> Option<&FuncEntry> {
        self.funcs.iter().find(|f| f.def.name == name)
    }
    pub fn func_exists(&self, name: &str) -> bool {
        self.func_find(name).is_some()
    }

    /// Registers (or, with an existing name, replaces) one recipe.
    fn func_register(&mut self, d: &Rc<Recipe>) {
        let entry = FuncEntry { def: d.clone(), locals: recipe_locals(d) };
        if let Some(slot) = self.funcs.iter_mut().find(|f| f.def.name == d.name) {
            *slot = entry;
        } else {
            self.funcs.push(entry);
        }
    }

    /// Checks the top-level recipes of `top` BEFORE anything is registered, so
    /// a failure leaves the registry untouched.
    pub fn recipes_validate(&self, top: &Program, allow_redefine: bool) -> Result<(), ()> {
        for (i, s) in top.iter().enumerate() {
            let Stmt::Recipe(r) = s else { continue };
            if self.extern_decl.iter().any(|n| *n == r.name) {
                report(r.line, "error", &format!("recipe '{}' clashes with an extern of the same name", r.name));
                return Err(());
            }
            if !allow_redefine && self.func_exists(&r.name) {
                report(r.line, "error", &format!("recipe '{}' is already defined", r.name));
                return Err(());
            }
            for o in &top[..i] {
                if let Stmt::Recipe(or) = o {
                    if or.name == r.name {
                        report(r.line, "error", &format!("recipe '{}' is already defined (line {})", r.name, or.line));
                        return Err(());
                    }
                }
            }
        }
        Ok(())
    }

    pub fn recipes_register(&mut self, top: &Program) {
        for s in top {
            if let Stmt::Recipe(r) = s {
                self.func_register(r);
            }
        }
    }

    // ---- C API / FFI (CEP-2): run-time side ---------------------------
    #[cfg(not(target_arch = "wasm32"))]
    fn lib_open(path: &[u8]) -> Result<libloading::Library, String> {
        let path_s = String::from_utf8_lossy(path).into_owned();
        let mut last_err = String::from("unknown error");
        let mut try_open = |p: &str| -> Option<libloading::Library> {
            match open_library(p) {
                Ok(l) => Some(l),
                Err(e) => {
                    last_err = e;
                    None
                }
            }
        };
        let has_sep = path_s.contains('/') || (cfg!(windows) && path_s.contains('\\'));
        let mut h = None;
        if !has_sep {
            let local = if cfg!(windows) { format!(".\\{}", path_s) } else { format!("./{}", path_s) };
            h = try_open(&local);
        }
        if h.is_none() {
            h = try_open(&path_s);
        }
        // a bare module name ("porch"): look in the standard library directory
        if h.is_none() && !has_sep && !path_s.contains('.') {
            if let Some(sp) = stdlib_lib_path(&path_s) {
                h = try_open(&sp.to_string_lossy());
            }
        }
        h.ok_or(last_err)
    }

    fn extern_slot(&mut self, name: &str) -> usize {
        if let Some(i) = self.externs.iter().position(|b| b.name == name) {
            // the same `extern` line may run again: drop the previous library handle
            self.externs[i].func = None;
            self.externs[i].lib = None;
            i
        } else {
            self.externs.push(ExternBinding { name: name.to_string(), func: None, lib: None });
            self.externs.len() - 1
        }
    }

    /// Binds `name` to its function in `lib_path`. Never fails hard: on any
    /// problem a warning is printed and calls evaluate to 0.
    #[cfg(not(target_arch = "wasm32"))]
    pub fn extern_bind(&mut self, name: &str, lib_path: &[u8]) {
        let idx = self.extern_slot(name);
        let lib_s = lossy(lib_path);
        if self.missing_libs.iter().any(|m| *m == lib_s) {
            return; // already reported: stay a silent stub
        }
        let lib = match Self::lib_open(lib_path) {
            Ok(l) => l,
            Err(why) => {
                if cfg!(windows) {
                    ewrite(&format!("[cob_interp] warning: could not load library '{}' (needed by extern {} and the other functions it declares); calls to them will return 0\n", lib_s, name));
                } else {
                    ewrite(&format!("[cob_interp] warning: could not load library '{}' ({}); calls to extern {} and the other functions it declares will return 0\n", lib_s, why, name));
                }
                if self.missing_libs.len() < 16 {
                    self.missing_libs.push(lib_s);
                }
                return;
            }
        };
        let sym = unsafe { lib.get::<CobFFIFunc>(name.as_bytes()) };
        match sym {
            Ok(f) => {
                let func: CobFFIFunc = *f;
                self.externs[idx].func = Some(func);
                self.externs[idx].lib = Some(lib);
            }
            Err(_) => {
                ewrite(&format!("[cob_interp] warning: extern {} -- symbol '{}' not found in '{}'; calls to it will return 0\n", name, name, lib_s));
            }
        }
    }

    /// The browser sandbox: modules that are linked in bind by name; anything
    /// else is a permanent stub, announced once per library on stdout.
    #[cfg(target_arch = "wasm32")]
    pub fn extern_bind(&mut self, name: &str, lib_path: &[u8]) {
        let idx = self.extern_slot(name);
        if let Some((_, f)) = self.static_externs.iter().find(|(n, _)| *n == name) {
            self.externs[idx].func = Some(*f);
            return;
        }
        let lib_s = lossy(lib_path);
        if self.missing_libs.iter().any(|m| *m == lib_s) {
            return;
        }
        if self.missing_libs.len() < 32 {
            self.missing_libs.push(lib_s.clone());
        }
        cob_abi::cstdio::write_stdout(
            format!(
                "\u{26A0}\u{FE0F} [COB Warning]: Native extensions (extern) cannot be loaded in the web sandbox; calls to '{}' are bypassed and evaluate to 0.\n\
                 \u{1F449} Download the native compiler at cob.pixel-pulse.dev/download.html to run scripts with native C extensions on your desktop!\n",
                lib_s
            )
            .as_bytes(),
        );
        cob_abi::cstdio::flush_stdout();
    }

    pub fn extern_find(&self, name: &str) -> Option<&ExternBinding> {
        self.externs.iter().find(|b| b.name == name)
    }
}

#[cfg(all(unix, not(target_arch = "wasm32")))]
fn open_library(path: &str) -> Result<libloading::Library, String> {
    use libloading::os::unix::{Library, RTLD_LOCAL, RTLD_NOW};
    unsafe { Library::open(Some(path), RTLD_NOW | RTLD_LOCAL) }
        .map(libloading::Library::from)
        .map_err(|e| e.to_string())
}

#[cfg(all(not(unix), not(target_arch = "wasm32")))]
fn open_library(path: &str) -> Result<libloading::Library, String> {
    unsafe { libloading::Library::new(path) }.map_err(|e| e.to_string())
}

/// `[cob_interp] <what> [at line N]: message` -- the shared diagnostic shape.
pub fn report(line: i32, what: &str, msg: &str) {
    if line > 0 {
        ewrite(&format!("[cob_interp] {} at line {}: {}\n", what, line, msg));
    } else {
        ewrite(&format!("[cob_interp] {}: {}\n", what, msg));
    }
}

impl Session {
    /// `:reset` -- forget variables, handles, recipes, externs and loaded libraries.
    pub fn reset(&mut self) {
        self.vars = VarTable::new();
        self.handles = HandleTable::default();
        self.funcs.clear();
        self.externs.clear();
        self.missing_libs.clear();
        self.extern_decl.clear();
    }
}

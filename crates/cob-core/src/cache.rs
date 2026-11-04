//! `.strawberry` -- the binary AST cache. The on-disk format is unchanged from
//! the C implementation (magic `COBSTRW10`), so caches are interchangeable
//! and popcorn_comp reads the same files.

use std::rc::Rc;

use crate::ast::*;
use crate::consts::*;

// ---- writing ---------------------------------------------------------------

struct Out(Vec<u8>);

impl Out {
    fn u8(&mut self, v: u8) {
        self.0.push(v);
    }
    fn u32(&mut self, v: u32) {
        self.0.extend_from_slice(&v.to_le_bytes());
    }
    fn i64(&mut self, v: i64) {
        self.0.extend_from_slice(&(v as u64).to_le_bytes());
    }
    fn bytes(&mut self, b: &[u8]) {
        self.u32(b.len() as u32);
        self.0.extend_from_slice(b);
    }
}

fn write_expr(o: &mut Out, e: &Expr) {
    match e {
        Expr::Num(n) => {
            o.u8(0);
            o.i64(*n);
        }
        Expr::Str(s) => {
            o.u8(1);
            o.bytes(s);
        }
        Expr::Var(v) => {
            o.u8(2);
            o.bytes(v.as_bytes());
        }
        Expr::Bin(op, l, r) => {
            o.u8(3);
            o.u8(*op);
            write_expr(o, l);
            write_expr(o, r);
        }
        Expr::Harvest(x) => {
            o.u8(4);
            write_expr(o, x);
        }
        Expr::ExternCall(c) | Expr::Call(c) => {
            o.u8(if matches!(e, Expr::ExternCall(_)) { 5 } else { 6 });
            o.bytes(c.name.as_bytes());
            o.u32(c.args.len() as u32);
            for a in &c.args {
                write_expr(o, a);
            }
        }
    }
}

fn write_cond(o: &mut Out, c: &Cond) {
    o.u8(c.op as u8);
    write_expr(o, &c.left);
    if c.op != CondOp::Truthy {
        if let Some(r) = &c.right {
            write_expr(o, r);
        }
    }
}

fn write_program(o: &mut Out, p: &Program) {
    o.u32(p.len() as u32);
    for s in p {
        write_stmt(o, s);
    }
}

fn write_stmt(o: &mut Out, s: &Stmt) {
    match s {
        Stmt::Pop(e) => {
            o.u8(0);
            write_expr(o, e);
        }
        Stmt::Set(n, e) => {
            o.u8(1);
            o.bytes(n.as_bytes());
            write_expr(o, e);
        }
        Stmt::While(c, b) => {
            o.u8(2);
            write_cond(o, c);
            write_program(o, b);
        }
        Stmt::Trash(n) => {
            o.u8(3);
            o.bytes(n.as_bytes());
        }
        Stmt::Extern(n, lib) => {
            o.u8(4);
            o.bytes(n.as_bytes());
            o.bytes(lib);
        }
        Stmt::If(c, a, b) => {
            o.u8(5);
            write_cond(o, c);
            write_program(o, a);
            write_program(o, b);
        }
        Stmt::Recipe(r) => {
            o.u8(6);
            o.bytes(r.name.as_bytes());
            o.u32(r.params.len() as u32);
            for p in &r.params {
                o.bytes(p.as_bytes());
            }
            write_program(o, &r.body);
        }
        Stmt::Serve(e) => {
            o.u8(7);
            write_expr(o, e);
        }
        Stmt::Call(e) => {
            o.u8(8);
            write_expr(o, e);
        }
    }
}

pub fn encode(prog: &Program) -> Vec<u8> {
    let mut o = Out(Vec::new());
    o.0.extend_from_slice(STRAWBERRY_MAGIC);
    write_program(&mut o, prog);
    o.0
}

// ---- reading ---------------------------------------------------------------

struct Rd<'a> {
    d: &'a [u8],
    pos: usize,
}

type R<T> = Result<T, ()>;

impl<'a> Rd<'a> {
    fn u8(&mut self) -> R<u8> {
        let v = *self.d.get(self.pos).ok_or(())?;
        self.pos += 1;
        Ok(v)
    }
    fn u32(&mut self) -> R<u32> {
        let b = self.d.get(self.pos..self.pos + 4).ok_or(())?;
        self.pos += 4;
        Ok(u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
    }
    fn i64(&mut self) -> R<i64> {
        let b = self.d.get(self.pos..self.pos + 8).ok_or(())?;
        self.pos += 8;
        let mut a = [0u8; 8];
        a.copy_from_slice(b);
        Ok(u64::from_le_bytes(a) as i64)
    }
    fn bytes(&mut self) -> R<Vec<u8>> {
        let len = self.u32()? as usize;
        let end = self.pos.checked_add(len).ok_or(())?;
        let b = self.d.get(self.pos..end).ok_or(())?.to_vec();
        self.pos = end;
        Ok(b)
    }
    fn string(&mut self) -> R<String> {
        Ok(String::from_utf8_lossy(&self.bytes()?).into_owned())
    }
}

fn read_expr(r: &mut Rd) -> R<Expr> {
    let kind = r.u8()?;
    Ok(match kind {
        0 => Expr::Num(r.i64()?),
        1 => Expr::Str(r.bytes()?),
        2 => Expr::Var(r.string()?),
        3 => {
            let op = r.u8()?;
            let l = read_expr(r)?;
            let rr = read_expr(r)?;
            Expr::Bin(op, Box::new(l), Box::new(rr))
        }
        4 => Expr::Harvest(Box::new(read_expr(r)?)),
        5 | 6 => {
            let name = r.string()?;
            let argc = r.u32()? as usize;
            if argc > MAX_EXTERN_ARGS {
                return Err(());
            }
            let mut args = Vec::new();
            for _ in 0..argc {
                args.push(read_expr(r)?);
            }
            let c = CallExpr { name, args, line: 0 };
            if kind == 5 { Expr::ExternCall(c) } else { Expr::Call(c) }
        }
        _ => return Err(()),
    })
}

fn read_cond(r: &mut Rd) -> R<Cond> {
    let op = match r.u8()? {
        0 => CondOp::Truthy,
        1 => CondOp::Eq,
        2 => CondOp::Ne,
        3 => CondOp::Lt,
        4 => CondOp::Gt,
        5 => CondOp::Le,
        6 => CondOp::Ge,
        _ => return Err(()),
    };
    let left = read_expr(r)?;
    let right = if op != CondOp::Truthy { Some(read_expr(r)?) } else { None };
    Ok(Cond { left, op, right })
}

fn read_stmt(r: &mut Rd) -> R<Stmt> {
    Ok(match r.u8()? {
        0 => Stmt::Pop(read_expr(r)?),
        1 => {
            let n = r.string()?;
            Stmt::Set(n, read_expr(r)?)
        }
        2 => {
            let c = read_cond(r)?;
            Stmt::While(c, read_program(r)?)
        }
        3 => Stmt::Trash(r.string()?),
        4 => {
            let n = r.string()?;
            Stmt::Extern(n, r.bytes()?)
        }
        5 => {
            let c = read_cond(r)?;
            let a = read_program(r)?;
            let b = read_program(r)?;
            Stmt::If(c, a, b)
        }
        6 => {
            let name = r.string()?;
            let np = r.u32()? as usize;
            if np > MAX_EXTERN_ARGS {
                return Err(());
            }
            let mut params = Vec::new();
            for _ in 0..np {
                params.push(r.string()?);
            }
            let body = read_program(r)?;
            Stmt::Recipe(Rc::new(Recipe { name, params, body, line: 0 }))
        }
        7 => Stmt::Serve(read_expr(r)?),
        8 => Stmt::Call(read_expr(r)?),
        _ => return Err(()),
    })
}

fn read_program(r: &mut Rd) -> R<Program> {
    let count = r.u32()?;
    let mut p = Vec::new();
    for _ in 0..count {
        p.push(read_stmt(r)?);
    }
    Ok(p)
}

/// Decodes a cache image; `Err` for a wrong magic or a damaged file.
pub fn decode(data: &[u8]) -> Result<Program, ()> {
    if data.len() < STRAWBERRY_MAGIC.len() || &data[..STRAWBERRY_MAGIC.len()] != STRAWBERRY_MAGIC {
        return Err(());
    }
    let mut r = Rd { d: data, pos: STRAWBERRY_MAGIC.len() };
    read_program(&mut r)
}

pub fn load(path: &str) -> Result<Program, ()> {
    let data = crate::fsutil::read_all(path).ok_or(())?;
    decode(&data)
}

pub fn write(path: &str, prog: &Program) {
    if !crate::fsutil::write_all(path, &encode(prog)) {
        cob_abi::ewrite(&format!("[cob_interp] warning: failed to write fast-boot cache '{}'\n", path));
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn roundtrip() {
        let mut s = crate::Session::new(false);
        let (prog, _) = s
            .parse_source(b"recipe f(a):\n    serve a + 1\nset x = f(2)\nif x == 3:\n    pop(\"ok\")\nelse:\n    pop(x)\n")
            .unwrap();
        let back = decode(&encode(&prog)).unwrap();
        assert_eq!(encode(&back), encode(&prog));
    }

    #[test]
    fn rejects_garbage() {
        assert!(decode(b"nope").is_err());
        let mut img = STRAWBERRY_MAGIC.to_vec();
        img.extend_from_slice(&[1, 0, 0, 0, 0xff]);
        assert!(decode(&img).is_err());
    }
}

/* =====================================================================
 * popcorn_comp.c
 * ---------------------------------------------------------------------
 * Project Obsidian Falcon / Cob Language Toolchain
 * The Cob native compiler: `popcorn_comp <file.strawberry> -o <output>
 *                            [--no-gc] [--cc <path>] [--emit-c <path.c>]`
 *
 * ---------------------------------------------------------------------
 * HOW THIS WORKS (v0.0.3 architecture -- spawns a real C compiler)
 * ---------------------------------------------------------------------
 * Earlier versions of popcorn_comp statically linked TCC's own compiler
 * source (libtcc.c) directly into this binary. That was dropped by
 * project decision in favor of a conventional design: popcorn_comp
 * transpiles a .strawberry AST to plain C, writes it to a temp file,
 * then spawns a real C compiler (gcc/clang/cc -- native or cross) as a
 * subprocess to turn that C into a native executable.
 *
 * Why the switch: TCC ships a genuine embeddable library API
 * (tcc_new/tcc_compile_string/tcc_output_file) designed for exactly
 * the in-process use case, but only ever targets the ONE platform it
 * was itself compiled for -- true cross-compilation meant bundling a
 * separately-built libtcc per target. GCC has no embeddable API at
 * all (it's cc1/as/ld orchestrated as separate processes no matter
 * what), so "embed GCC the way we embedded TCC" isn't a smaller
 * version of the same trick -- that trick doesn't exist for GCC.
 * Spawning a real compiler trades "no external executables" for
 * "works with whatever real, well-tested compiler is already the
 * standard tool for the target platform" -- the same tradeoff tools
 * like Zig's C backend or Cython make.
 *
 * ---------------------------------------------------------------------
 * CHOOSING WHICH COMPILER TO SPAWN
 * ---------------------------------------------------------------------
 * Resolved in this order (see resolve_compiler() below):
 *   1. --cc <path>            explicit override, always wins
 *   2. $CobCC                 environment variable override
 *   3. $CobOS / $CobArch      looked up in a small table of known
 *                             cross-compiler binary names -- the same
 *                             ones .github/workflows/build.yml already
 *                             installs to cross-build the Cob tools
 *                             themselves (e.g. x86_64-w64-mingw32-gcc)
 *   4. "cc", then "gcc"       plain PATH lookup, for the common case of
 *                             compiling natively on a machine that
 *                             already has a C toolchain
 *
 * IMPORTANT HONESTY NOTE: the cross-compiler binary names in step 3
 * are Linux-amd64-*hosted* tools -- they run on a Linux build machine
 * and *emit* code for another platform, but are NOT compilers that
 * will run on an actual Windows/ARM machine. A "self-contained
 * popcorn_comp that works with zero setup" on those platforms means
 * bundling a genuinely native-hosted toolchain for that platform (e.g.
 * a WinLibs-style standalone MinGW-w64 build that runs ON Windows) --
 * that's real follow-up work, not something step 3 provides by itself.
 * What step 3 IS useful for: cross-compiling from a build server that
 * already has these cross-gcc packages installed, which is exactly
 * what build.yml's CI runner has.
 *
 * ---------------------------------------------------------------------
 * STRINGS (v0.0.4) -- every Cob variable is emitted as a CobValue, a
 * small tagged struct (int or string) instead of a plain `long`, with
 * every operator becoming a call into a small hand-written runtime
 * (cob_add/cob_sub/cob_eq/etc.) embedded at the top of the generated
 * C file. This exists specifically to keep compiled behavior matching
 * interpreted behavior -- same results, same type-mismatch warnings,
 * same fallback-to-0 semantics -- rather than a faster but
 * lower-fidelity approach like per-variable static type inference.
 * The one deliberate difference: runtime warnings from compiled code
 * are tagged "[cob]" rather than "[cob_interp]", since a compiled
 * binary is its own standalone program, not the interpreter.
 *
 * License: PolyForm Noncommercial License 1.0.0 - see LICENSE.md
 * ===================================================================== */

/* Must come before any system headers: readlink() (used to find this
 * executable's own path on Linux, for locating a bundled zig/ folder)
 * isn't visible under strict -std=c99 on glibc without requesting
 * POSIX explicitly. */
#if !defined(_MSC_VER) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "common.h"
#include "file_io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

#if defined(_WIN32)
#  include <windows.h>
#elif defined(__APPLE__)
#  include <mach-o/dyld.h>
#  include <unistd.h>
#else
#  include <unistd.h> /* readlink */
#endif

/* ---------------------------------------------------------------------
 * AST (read-only mirror of cob_interp.c's node set -- see that file's
 * top-of-file comment for the full language semantics). Only what's
 * needed to walk and transpile the tree lives here.
 * ------------------------------------------------------------------- */
/* v0.0.5 added EXPR_SQL_OPEN..EXPR_WINDOW_TEXTBOX (13 kinds) between
 * EXPR_HARVEST and what cob_interp.c now calls EXPR_EXTERN_CALL (CEP-2).
 * This reader never implemented any of those 13 -- a script using one
 * fails cleanly via read_expr()'s `default: return -1` (see the magic-
 * sharing comment above) -- but EXPR_EXTERN_CALL's on-the-wire tag is
 * still *19th* (index 18), so the 13 are named here as plain spacers,
 * purely to keep that numbering, and never given a `case` of their own. */
typedef enum {
    EXPR_NUM, EXPR_STR, EXPR_VAR, EXPR_BINOP, EXPR_HARVEST,
    POPCORN_UNSUPPORTED_SQL_OPEN, POPCORN_UNSUPPORTED_SQL_EXEC,
    POPCORN_UNSUPPORTED_SQL_QUERY, POPCORN_UNSUPPORTED_SQL_CLOSE,
    POPCORN_UNSUPPORTED_TCL_EVAL, POPCORN_UNSUPPORTED_TK_EVAL,
    POPCORN_UNSUPPORTED_WINDOW_OPEN, POPCORN_UNSUPPORTED_WINDOW_LABEL,
    POPCORN_UNSUPPORTED_WINDOW_WAIT, POPCORN_UNSUPPORTED_WINDOW_CLOSE,
    POPCORN_UNSUPPORTED_WINDOW_BUTTON, POPCORN_UNSUPPORTED_WINDOW_SLIDER,
    POPCORN_UNSUPPORTED_WINDOW_TEXTBOX,
    /* C API / FFI (CEP-2): a call to a name bound by an `extern`
     * statement elsewhere in the program. var = the C symbol name,
     * args[0..argc-1] = the call's arguments. */
    EXPR_EXTERN_CALL
} ExprKind;
typedef struct Expr {
    ExprKind kind;
    long num;
    char *str_lit; size_t str_lit_len;
    char *var;
    char op;
    struct Expr *left;
    struct Expr *right;
    struct Expr **args; int argc;   /* EXPR_EXTERN_CALL */
} Expr;

typedef enum { COND_TRUTHY, COND_EQ, COND_NE, COND_LT, COND_GT, COND_LE, COND_GE } CondOp;
typedef struct { Expr *left; CondOp op; Expr *right; } Cond;

/* STMT_EXTERN (CEP-2) is appended last, same as cob_interp.c -- no
 * numbering gap to bridge here, unlike ExprKind above. */
/* STMT_IF mirrors cob_interp.c (appended last; `elif` is already desugared
 * to a nested if in else_body by the interpreter's parser). */
typedef enum { STMT_POP, STMT_SET, STMT_WHILE, STMT_TRASH, STMT_EXTERN, STMT_IF } StmtKind;
typedef struct Program { struct Stmt *items; size_t count, capacity; } Program;
typedef struct Stmt {
    StmtKind kind;
    Expr *pop_expr;
    char *set_name; Expr *set_expr;
    Cond *while_cond; Program while_body;
    char *trash_name;
    char *ext_name; char *ext_lib;   /* STMT_EXTERN */
    Cond *if_cond; Program if_body; Program else_body;   /* STMT_IF */
} Stmt;

static void program_init(Program *p) { p->items = NULL; p->count = 0; p->capacity = 0; }
static int program_push(Program *p, Stmt s) {
    if (p->count == p->capacity) {
        size_t new_cap = (p->capacity == 0) ? 8 : p->capacity * 2;
        Stmt *grown = (Stmt *)realloc(p->items, new_cap * sizeof(Stmt));
        if (!grown) return -1;
        p->items = grown; p->capacity = new_cap;
    }
    p->items[p->count++] = s;
    return 0;
}

/* ---------------------------------------------------------------------
 * .strawberry READER (must stay byte-for-byte compatible with
 * cob_interp.c's writer). See cob_interp.c for the authoritative
 * format documentation; the ExprKind/StmtKind handling below is
 * duplicated in miniature here deliberately, so this tool doesn't need
 * to link the whole interpreter just to read one file format.
 * v3 added EXPR_STR (string literals) and changed STMT_POP to hold a
 * general Expr instead of a bare literal -- both mirrored below.
 *
 * STRAWBERRY_MAGIC/_LEN themselves now come from common.h instead of a
 * second private #define here -- see that header's comment for why
 * (this exact file went silently out of sync with cob_interp.c's copy
 * twice before that change). v4 (SQLite/Tcl/Tk), v5 (_cobwindow), and
 * v6 (raygui widgets) only ever *appended* new ExprKind values after
 * EXPR_HARVEST -- they never touched the byte layout of anything this
 * reader already implements, so a script using one of those newer,
 * unsupported keywords still fails cleanly via read_expr()'s
 * `default: return -1` below (reported as "corrupt or truncated"), not
 * a crash. That's what makes it safe for this reader to share one
 * magic constant with a writer that implements a larger keyword set
 * than it does.
 * ------------------------------------------------------------------- */

typedef struct { const unsigned char *data; size_t len, pos; } ByteReader;

static int reader_u8(ByteReader *r, uint8_t *out) {
    if (r->pos + 1 > r->len) return -1;
    *out = r->data[r->pos++];
    return 0;
}
static int reader_u32(ByteReader *r, uint32_t *out) {
    if (r->pos + 4 > r->len) return -1;
    *out = (uint32_t)r->data[r->pos] | ((uint32_t)r->data[r->pos+1] << 8) |
           ((uint32_t)r->data[r->pos+2] << 16) | ((uint32_t)r->data[r->pos+3] << 24);
    r->pos += 4;
    return 0;
}
static int reader_i64(ByteReader *r, int64_t *out) {
    if (r->pos + 8 > r->len) return -1;
    uint64_t u = 0; int i;
    for (i = 0; i < 8; i++) u |= ((uint64_t)r->data[r->pos + i]) << (8 * i);
    r->pos += 8;
    *out = (int64_t)u;
    return 0;
}
static int reader_bytes_alloc(ByteReader *r, char **out_str, size_t *out_len) {
    uint32_t len;
    if (reader_u32(r, &len) != 0) return -1;
    if (r->pos + len > r->len) return -1;
    char *s = (char *)malloc((size_t)len + 1);
    if (!s) return -1;
    memcpy(s, r->data + r->pos, len);
    s[len] = '\0';
    r->pos += len;
    *out_str = s;
    if (out_len) *out_len = len;
    return 0;
}

static Expr *expr_new(ExprKind k) { Expr *e = (Expr *)calloc(1, sizeof(Expr)); if (e) e->kind = k; return e; }

static int read_expr(ByteReader *r, Expr **out) {
    uint8_t kind;
    if (reader_u8(r, &kind) != 0) return -1;
    switch (kind) {
        case EXPR_NUM: {
            int64_t v;
            if (reader_i64(r, &v) != 0) return -1;
            *out = expr_new(EXPR_NUM);
            if (!*out) return -1;
            (*out)->num = (long)v;
            return 0;
        }
        case EXPR_STR: {
            char *text; size_t len;
            if (reader_bytes_alloc(r, &text, &len) != 0) return -1;
            *out = expr_new(EXPR_STR);
            if (!*out) { free(text); return -1; }
            (*out)->str_lit = text; (*out)->str_lit_len = len;
            return 0;
        }
        case EXPR_VAR: {
            char *name;
            if (reader_bytes_alloc(r, &name, NULL) != 0) return -1;
            *out = expr_new(EXPR_VAR);
            if (!*out) { free(name); return -1; }
            (*out)->var = name;
            return 0;
        }
        case EXPR_BINOP: {
            uint8_t op; Expr *l, *rr;
            if (reader_u8(r, &op) != 0) return -1;
            if (read_expr(r, &l) != 0) return -1;
            if (read_expr(r, &rr) != 0) return -1;
            *out = expr_new(EXPR_BINOP);
            if (!*out) return -1;
            (*out)->op = (char)op; (*out)->left = l; (*out)->right = rr;
            return 0;
        }
        case EXPR_HARVEST: {
            Expr *inner;
            if (read_expr(r, &inner) != 0) return -1;
            *out = expr_new(EXPR_HARVEST);
            if (!*out) return -1;
            (*out)->left = inner;
            return 0;
        }
        case EXPR_EXTERN_CALL: {
            char *name; uint32_t n, j; Expr **args = NULL;
            if (reader_bytes_alloc(r, &name, NULL) != 0) return -1;
            if (reader_u32(r, &n) != 0) { free(name); return -1; }
            if (n > 0) {
                args = (Expr **)calloc(n, sizeof(Expr *));
                if (!args) { free(name); return -1; }
            }
            for (j = 0; j < n; j++) {
                if (read_expr(r, &args[j]) != 0) { free(args); free(name); return -1; }
            }
            *out = expr_new(EXPR_EXTERN_CALL);
            if (!*out) { free(args); free(name); return -1; }
            (*out)->var = name; (*out)->args = args; (*out)->argc = (int)n;
            return 0;
        }
        default: return -1;
    }
}
static int read_cond(ByteReader *r, Cond **out) {
    uint8_t op;
    if (reader_u8(r, &op) != 0) return -1;
    Cond *c = (Cond *)calloc(1, sizeof(Cond));
    if (!c) return -1;
    c->op = (CondOp)op;
    if (read_expr(r, &c->left) != 0) { free(c); return -1; }
    if (op != COND_TRUTHY) {
        if (read_expr(r, &c->right) != 0) { free(c); return -1; }
    }
    *out = c;
    return 0;
}
static int read_program(ByteReader *r, Program *out);
static int read_stmt(ByteReader *r, Stmt *out) {
    uint8_t kind;
    if (reader_u8(r, &kind) != 0) return -1;
    memset(out, 0, sizeof(*out));
    out->kind = (StmtKind)kind;
    switch (out->kind) {
        case STMT_POP: return read_expr(r, &out->pop_expr);
        case STMT_SET:
            if (reader_bytes_alloc(r, &out->set_name, NULL) != 0) return -1;
            return read_expr(r, &out->set_expr);
        case STMT_TRASH: return reader_bytes_alloc(r, &out->trash_name, NULL);
        case STMT_EXTERN:
            if (reader_bytes_alloc(r, &out->ext_name, NULL) != 0) return -1;
            return reader_bytes_alloc(r, &out->ext_lib, NULL);
        case STMT_WHILE:
            if (read_cond(r, &out->while_cond) != 0) return -1;
            return read_program(r, &out->while_body);
        case STMT_IF:
            if (read_cond(r, &out->if_cond) != 0) return -1;
            if (read_program(r, &out->if_body) != 0) return -1;
            return read_program(r, &out->else_body);
        default: return -1;
    }
}
static int read_program(ByteReader *r, Program *out) {
    uint32_t count, i;
    program_init(out);
    if (reader_u32(r, &count) != 0) return -1;
    for (i = 0; i < count; i++) {
        Stmt s;
        if (read_stmt(r, &s) != 0) return -1;
        if (program_push(out, s) != 0) return -1;
    }
    return 0;
}

static int cob_load_strawberry(const char *path, Program *out_prog) {
    CobFileBuffer file;
    if (cob_file_read_all(path, &file) != COB_FILE_OK) {
        fprintf(stderr, "[popcorn_comp] error: could not read '%s'\n", path);
        return -1;
    }
    ByteReader r; r.data = (const unsigned char *)file.data; r.len = file.size; r.pos = 0;
    if (r.len < STRAWBERRY_MAGIC_LEN || memcmp(r.data, STRAWBERRY_MAGIC, STRAWBERRY_MAGIC_LEN) != 0) {
        fprintf(stderr, "[popcorn_comp] error: '%s' is not a valid .strawberry file "
                         "(expected magic \"%s\")\n", path, STRAWBERRY_MAGIC);
        cob_file_free(&file);
        return -1;
    }
    r.pos = STRAWBERRY_MAGIC_LEN;
    int rc = read_program(&r, out_prog);
    cob_file_free(&file);
    if (rc != 0) fprintf(stderr, "[popcorn_comp] error: '%s' is corrupt or truncated\n", path);
    return rc;
}

/* ---------------------------------------------------------------------
 * DYNAMIC STRING BUFFER for building the generated C source
 * ------------------------------------------------------------------- */
typedef struct { char *data; size_t len, cap; } StrBuf;
static void sb_init(StrBuf *b) { b->data = NULL; b->len = 0; b->cap = 0; }
static int sb_ensure(StrBuf *b, size_t extra) {
    if (b->len + extra + 1 <= b->cap) return 0;
    size_t new_cap = (b->cap == 0) ? 1024 : b->cap * 2;
    while (new_cap < b->len + extra + 1) new_cap *= 2;
    char *grown = (char *)realloc(b->data, new_cap);
    if (!grown) return -1;
    b->data = grown; b->cap = new_cap;
    return 0;
}
static int sb_append(StrBuf *b, const char *s) {
    size_t len = strlen(s);
    if (sb_ensure(b, len) != 0) return -1;
    memcpy(b->data + b->len, s, len + 1);
    b->len += len;
    return 0;
}
static int sb_appendf(StrBuf *b, const char *fmt, ...) {
    /* 4096, not the smaller size this used to be: the CEP-2 FFI adapter
     * template (see emit_extern_adapters()) is a several-line C function
     * body built in one call, well past what a tighter buffer could
     * safely hold. vsnprintf() never overflows `tmp` either way -- it
     * truncates and reports the length it WOULD have written -- but a
     * silently truncated piece of generated C is a real correctness bug
     * (broken/invalid output that fails far away, inside the spawned
     * compiler, with no indication why), so a would-be-truncated write
     * is treated as a hard error here instead of silently proceeding. */
    char tmp[4096];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(tmp)) return -1;
    return sb_append(b, tmp);
}

/* ---------------------------------------------------------------------
 * VARIABLE NAME COLLECTION (so every Cob variable becomes exactly one
 * declared C `long cob_var_<name>;` at the top of main())
 * ------------------------------------------------------------------- */
typedef struct { char **names; size_t count, capacity; } NameSet;
static void nameset_init(NameSet *ns) { ns->names = NULL; ns->count = 0; ns->capacity = 0; }
static int nameset_add(NameSet *ns, const char *name) {
    size_t i;
    for (i = 0; i < ns->count; i++) if (strcmp(ns->names[i], name) == 0) return 0;
    if (ns->count == ns->capacity) {
        size_t new_cap = (ns->capacity == 0) ? 8 : ns->capacity * 2;
        char **grown = (char **)realloc(ns->names, new_cap * sizeof(char *));
        if (!grown) return -1;
        ns->names = grown; ns->capacity = new_cap;
    }
    ns->names[ns->count++] = (char *)name; /* borrowed, not owned */
    return 0;
}
static void collect_expr(const Expr *e, NameSet *ns) {
    if (!e) return;
    if (e->kind == EXPR_VAR) nameset_add(ns, e->var);
    if (e->kind == EXPR_BINOP) { collect_expr(e->left, ns); collect_expr(e->right, ns); }
    if (e->kind == EXPR_HARVEST) collect_expr(e->left, ns);
    if (e->kind == EXPR_EXTERN_CALL) {
        int j;
        for (j = 0; j < e->argc; j++) collect_expr(e->args[j], ns);
    }
}
static void collect_cond(const Cond *c, NameSet *ns) { collect_expr(c->left, ns); collect_expr(c->right, ns); }
static void collect_program(const Program *p, NameSet *ns) {
    size_t i;
    for (i = 0; i < p->count; i++) {
        const Stmt *s = &p->items[i];
        switch (s->kind) {
            case STMT_POP: collect_expr(s->pop_expr, ns); break;
            case STMT_SET: nameset_add(ns, s->set_name); collect_expr(s->set_expr, ns); break;
            case STMT_TRASH: nameset_add(ns, s->trash_name); break;
            case STMT_WHILE: collect_cond(s->while_cond, ns); collect_program(&s->while_body, ns); break;
            case STMT_IF:
                collect_cond(s->if_cond, ns);
                collect_program(&s->if_body, ns);
                collect_program(&s->else_body, ns);
                break;
            default: break;
        }
    }
}
static int program_uses_harvest_or_trash(const Program *p); /* fwd */

/* ---------------------------------------------------------------------
 * C API / FFI (CEP-2) -- EXTERN DECLARATION TABLE
 *
 * Unlike cob_interp, which re-resolves (dlopen/dlsym) every time a
 * STMT_EXTERN actually runs -- so the SAME name really can rebind to a
 * DIFFERENT library across, say, two iterations of a while loop --
 * popcorn_comp links statically, once, at compile time (CEP-2 3.3/4:
 * "a real static/dynamic link at compile time, no dlopen in the
 * compiled binary at all"). A compiled binary has no equivalent of
 * "rebind this name" at runtime, so collect_externs() treats every
 * `extern <name> ... from "<lib>"` anywhere in the program (including
 * inside a while body, and including the same line reached many times)
 * as ONE static declaration per distinct name: repeating the exact same
 * (name, library) pair is fine and deduplicated silently, but the same
 * name declared against two DIFFERENT libraries is a compile error --
 * there would be no principled way to pick which library's symbol the
 * single static forward declaration should actually bind to.
 * ------------------------------------------------------------------- */
static char *popcorn_strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}
typedef struct { char *name; char *lib; } ExternDecl;
typedef struct { ExternDecl *items; size_t count, capacity; } ExternSet;
static void externset_init(ExternSet *es) { es->items = NULL; es->count = 0; es->capacity = 0; }
static void externset_free(ExternSet *es) {
    size_t i;
    for (i = 0; i < es->count; i++) { free(es->items[i].name); free(es->items[i].lib); }
    free(es->items);
    es->items = NULL; es->count = es->capacity = 0;
}
/* Returns 0 (added or harmlessly deduplicated), or -1 on a genuine
 * name/library conflict (already reported to stderr) or OOM. */
static int externset_add(ExternSet *es, const char *name, const char *lib) {
    size_t i;
    for (i = 0; i < es->count; i++) {
        if (strcmp(es->items[i].name, name) != 0) continue;
        if (strcmp(es->items[i].lib, lib) == 0) return 0; /* same decl seen again, fine */
        fprintf(stderr,
            "[popcorn_comp] error: extern '%s' is declared against two different libraries\n"
            "  ('%s' and '%s') -- popcorn_comp links statically, so every occurrence of the\n"
            "  same extern name in one program must name the same library\n",
            name, es->items[i].lib, lib);
        return -1;
    }
    if (es->count == es->capacity) {
        size_t new_cap = es->capacity ? es->capacity * 2 : 8;
        ExternDecl *grown = (ExternDecl *)realloc(es->items, new_cap * sizeof(ExternDecl));
        if (!grown) return -1;
        es->items = grown; es->capacity = new_cap;
    }
    es->items[es->count].name = popcorn_strdup(name);
    es->items[es->count].lib  = popcorn_strdup(lib);
    if (!es->items[es->count].name || !es->items[es->count].lib) {
        free(es->items[es->count].name); free(es->items[es->count].lib);
        return -1;
    }
    es->count++;
    return 0;
}
static int collect_externs(const Program *p, ExternSet *es) {
    size_t i;
    for (i = 0; i < p->count; i++) {
        const Stmt *s = &p->items[i];
        if (s->kind == STMT_EXTERN) {
            if (externset_add(es, s->ext_name, s->ext_lib) != 0) return -1;
        } else if (s->kind == STMT_WHILE) {
            if (collect_externs(&s->while_body, es) != 0) return -1;
        } else if (s->kind == STMT_IF) {
            if (collect_externs(&s->if_body, es) != 0) return -1;
            if (collect_externs(&s->else_body, es) != 0) return -1;
        }
    }
    return 0;
}

/* A library named with a path/extension ("math_ext.so", "./a.so",
 * "C:\\ext\\a.dll") is passed to the compiler as a literal link input,
 * exactly like the CEP-2 example; a bare name with neither ("m", for
 * libm) becomes a conventional -lm flag. This is the one heuristic
 * needed to cover both forms CEP-2's own spec text uses as examples
 * ("-lmath_ext, or the literal path given in the from clause"). */
static int extern_lib_looks_like_path(const char *lib) {
    return strchr(lib, '.') != NULL || strchr(lib, '/') != NULL || strchr(lib, '\\') != NULL;
}


/* ---------------------------------------------------------------------
 * TRANSPILER: AST -> C source text
 * ------------------------------------------------------------------- */
static int emit_c_string_literal(StrBuf *b, const char *text, size_t len) {
    size_t i;
    if (sb_append(b, "\"") != 0) return -1;
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];
        switch (c) {
            case '"':  if (sb_append(b, "\\\"") != 0) return -1; break;
            case '\\': if (sb_append(b, "\\\\") != 0) return -1; break;
            case '\n': if (sb_append(b, "\\n") != 0) return -1; break;
            case '\t': if (sb_append(b, "\\t") != 0) return -1; break;
            default:
                if (c < 0x20 || c == 0x7F) { if (sb_appendf(b, "\\x%02x\"\"", c) != 0) return -1; }
                else { char one[2] = { (char)c, '\0' }; if (sb_append(b, one) != 0) return -1; }
        }
    }
    return sb_append(b, "\"");
}
static int emit_expr(StrBuf *b, const Expr *e) {
    switch (e->kind) {
        case EXPR_NUM: return sb_appendf(b, "cob_int(%ldL)", e->num);
        case EXPR_STR:
            if (sb_append(b, "cob_str(") != 0) return -1;
            if (emit_c_string_literal(b, e->str_lit, e->str_lit_len) != 0) return -1;
            return sb_append(b, ")");
        case EXPR_VAR: return sb_appendf(b, "cob_copy(&cob_var_%s)", e->var);
        case EXPR_BINOP: {
            const char *fn;
            switch (e->op) {
                case '+': fn = "cob_add"; break;
                case '-': fn = "cob_sub"; break;
                case '*': fn = "cob_mul"; break;
                case '/': fn = "cob_divop"; break;
                default: return -1;
            }
            if (sb_appendf(b, "%s(", fn) != 0) return -1;
            if (emit_expr(b, e->left) != 0) return -1;
            if (sb_append(b, ", ") != 0) return -1;
            if (emit_expr(b, e->right) != 0) return -1;
            return sb_append(b, ")");
        }
        case EXPR_HARVEST:
            if (sb_append(b, "cob_int((long)(intptr_t)malloc((size_t)cob_harvest_bytes(") != 0) return -1;
            if (emit_expr(b, e->left) != 0) return -1;
            return sb_append(b, ")))");
        case EXPR_EXTERN_CALL: {
            /* cob_call_<name>(...) is the per-name adapter transpile()
             * emits once near the top of the file (see "FFI CODEGEN"
             * below) -- it does the CobValue<->CobFFIValue conversion
             * and the actual call. argc==0 passes NULL/0 rather than an
             * empty compound literal, which C99 doesn't allow. */
            int j;
            if (e->argc == 0) return sb_appendf(b, "cob_call_%s(NULL, 0)", e->var);
            if (sb_appendf(b, "cob_call_%s((CobValue[]){", e->var) != 0) return -1;
            for (j = 0; j < e->argc; j++) {
                if (j > 0 && sb_append(b, ", ") != 0) return -1;
                if (emit_expr(b, e->args[j]) != 0) return -1;
            }
            return sb_appendf(b, "}, %d)", e->argc);
        }
        default: return -1;
    }
}
static const char *cond_op_fn(CondOp op) {
    switch (op) {
        case COND_EQ: return "cob_eq"; case COND_NE: return "cob_ne";
        case COND_LT: return "cob_lt"; case COND_GT: return "cob_gt";
        case COND_LE: return "cob_le"; case COND_GE: return "cob_ge";
        default: return NULL;
    }
}
static int emit_cond(StrBuf *b, const Cond *c) {
    if (c->op == COND_TRUTHY) {
        if (sb_append(b, "cob_truthy(") != 0) return -1;
        if (emit_expr(b, c->left) != 0) return -1;
        return sb_append(b, ")");
    }
    if (sb_appendf(b, "%s(", cond_op_fn(c->op)) != 0) return -1;
    if (emit_expr(b, c->left) != 0) return -1;
    if (sb_append(b, ", ") != 0) return -1;
    if (emit_expr(b, c->right) != 0) return -1;
    return sb_append(b, ")");
}
static int emit_program(StrBuf *b, const Program *p) {
    size_t i;
    for (i = 0; i < p->count; i++) {
        const Stmt *s = &p->items[i];
        switch (s->kind) {
            case STMT_POP:
                if (sb_append(b, "cob_pop(") != 0) return -1;
                if (emit_expr(b, s->pop_expr) != 0) return -1;
                if (sb_append(b, ");\n") != 0) return -1;
                break;
            case STMT_SET:
                /* free the old value first -- e.g. reassigning a string
                 * variable in a loop must not leak the previous string,
                 * same discipline as cob_interp's vars_set(). */
                if (sb_appendf(b, "cob_free(&cob_var_%s);\ncob_var_%s = ", s->set_name, s->set_name) != 0) return -1;
                if (emit_expr(b, s->set_expr) != 0) return -1;
                if (sb_append(b, ";\n") != 0) return -1;
                break;
            case STMT_TRASH:
                if (sb_appendf(b,
                        "if (cob_var_%s.is_str) fprintf(stderr, \"[cob] warning: trash(%s) -- variable holds a string, not a handle\\n\");\n"
                        "else { free((void*)(intptr_t)cob_var_%s.i); cob_var_%s.i = 0; }\n",
                        s->trash_name, s->trash_name, s->trash_name, s->trash_name) != 0) return -1;
                break;
            case STMT_WHILE:
                if (sb_append(b, "while (") != 0) return -1;
                if (emit_cond(b, s->while_cond) != 0) return -1;
                if (sb_append(b, ") {\n") != 0) return -1;
                if (emit_program(b, &s->while_body) != 0) return -1;
                if (sb_append(b, "}\n") != 0) return -1;
                break;
            case STMT_IF:
                if (sb_append(b, "if (") != 0) return -1;
                if (emit_cond(b, s->if_cond) != 0) return -1;
                if (sb_append(b, ") {\n") != 0) return -1;
                if (emit_program(b, &s->if_body) != 0) return -1;
                if (s->else_body.count > 0) {
                    if (sb_append(b, "} else {\n") != 0) return -1;
                    if (emit_program(b, &s->else_body) != 0) return -1;
                }
                if (sb_append(b, "}\n") != 0) return -1;
                break;
            case STMT_EXTERN:
                /* No-op here: unlike cob_interp's dlopen/dlsym, which
                 * genuinely happens at this point in execution, the
                 * compiled program's binding is entirely static -- the
                 * forward declaration and the real library link both
                 * already happened before this program started running
                 * (see transpile()'s FFI CODEGEN section and main()'s
                 * link-argument construction). Nothing to emit at the
                 * position of the statement itself. */
                break;
        }
    }
    return 0;
}

static int program_uses_harvest_or_trash(const Program *p) {
    size_t i;
    for (i = 0; i < p->count; i++) {
        const Stmt *s = &p->items[i];
        if (s->kind == STMT_TRASH) return 1;
        if (s->kind == STMT_SET) {
            const Expr *e = s->set_expr;
            /* shallow-ish check reusing collect_expr's traversal shape */
            if (e && (e->kind == EXPR_HARVEST ||
                (e->kind == EXPR_BINOP && (e->left->kind == EXPR_HARVEST || e->right->kind == EXPR_HARVEST))))
                return 1;
        }
        if (s->kind == STMT_WHILE && program_uses_harvest_or_trash(&s->while_body)) return 1;
        if (s->kind == STMT_IF && (program_uses_harvest_or_trash(&s->if_body) ||
                                   program_uses_harvest_or_trash(&s->else_body))) return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------------
 * COBVALUE RUNTIME -- emitted verbatim at the top of every generated
 * program. Cob variables can hold an int or a string (as of v0.0.4),
 * but generated C needs one concrete type per variable, so every Cob
 * variable becomes a CobValue (a small tagged union) instead of a
 * plain `long`, and every operator becomes a call into one of these
 * helpers instead of a raw C operator. This mirrors cob_interp.c's
 * eval_expr/eval_cond semantics deliberately, including the exact
 * warning wording, so a program's compiled behavior matches its
 * interpreted behavior -- same results, same warnings, same fallback
 * values on a type mismatch.
 * ------------------------------------------------------------------- */
static const char *COB_RUNTIME_PREAMBLE =
"typedef struct { int is_str; long i; char *s; } CobValue;\n"
"static char *cob_strdup_(const char *s) { size_t n = strlen(s) + 1; char *p = (char *)malloc(n); if (p) memcpy(p, s, n); return p; }\n"
"static CobValue cob_int(long n) { CobValue v; v.is_str = 0; v.i = n; v.s = NULL; return v; }\n"
"static CobValue cob_str(const char *s) { CobValue v; v.is_str = 1; v.i = 0; v.s = cob_strdup_(s); return v; }\n"
"static CobValue cob_str_take(char *s) { CobValue v; v.is_str = 1; v.i = 0; v.s = s; return v; }\n"
"static void cob_free(CobValue *v) { if (v->is_str) { free(v->s); v->s = NULL; } }\n"
"static CobValue cob_copy(const CobValue *v) { return v->is_str ? cob_str(v->s ? v->s : \"\") : cob_int(v->i); }\n"
"static void cob_long_to_str(long n, char *buf, size_t bufsize) { snprintf(buf, bufsize, \"%ld\", n); }\n"
"static CobValue cob_add(CobValue l, CobValue r) {\n"
"    if (l.is_str || r.is_str) {\n"
"        char lbuf[32], rbuf[32];\n"
"        const char *ls = l.is_str ? l.s : (cob_long_to_str(l.i, lbuf, sizeof(lbuf)), lbuf);\n"
"        const char *rs = r.is_str ? r.s : (cob_long_to_str(r.i, rbuf, sizeof(rbuf)), rbuf);\n"
"        size_t ln = strlen(ls), rn = strlen(rs);\n"
"        char *cat = (char *)malloc(ln + rn + 1);\n"
"        CobValue result;\n"
"        if (cat) { memcpy(cat, ls, ln); memcpy(cat + ln, rs, rn); cat[ln + rn] = '\\0'; result = cob_str_take(cat); }\n"
"        else result = cob_int(0);\n"
"        cob_free(&l); cob_free(&r);\n"
"        return result;\n"
"    }\n"
"    return cob_int(l.i + r.i);\n"
"}\n"
"static CobValue cob_sub(CobValue l, CobValue r) {\n"
"    if (l.is_str || r.is_str) { fprintf(stderr, \"[cob] warning: operator '-' requires two numbers, got a string; result treated as 0\\n\"); cob_free(&l); cob_free(&r); return cob_int(0); }\n"
"    return cob_int(l.i - r.i);\n"
"}\n"
"static CobValue cob_mul(CobValue l, CobValue r) {\n"
"    if (l.is_str || r.is_str) { fprintf(stderr, \"[cob] warning: operator '*' requires two numbers, got a string; result treated as 0\\n\"); cob_free(&l); cob_free(&r); return cob_int(0); }\n"
"    return cob_int(l.i * r.i);\n"
"}\n"
"static CobValue cob_divop(CobValue l, CobValue r) {\n"
"    if (l.is_str || r.is_str) { fprintf(stderr, \"[cob] warning: operator '/' requires two numbers, got a string; result treated as 0\\n\"); cob_free(&l); cob_free(&r); return cob_int(0); }\n"
"    if (r.i == 0) { fprintf(stderr, \"[cob] warning: division by zero, result treated as 0\\n\"); return cob_int(0); }\n"
"    return cob_int(l.i / r.i);\n"
"}\n"
"static long cob_to_long_for_harvest(CobValue v) {\n"
"    long n;\n"
"    if (v.is_str) { fprintf(stderr, \"[cob] warning: harvest() needs a number, got a string; treated as 0\\n\"); n = 0; cob_free(&v); }\n"
"    else n = v.i;\n"
"    return n;\n"
"}\n"
"static long cob_harvest_bytes(CobValue v) {\n"
"    long bytes = cob_to_long_for_harvest(v);\n"
"    if (bytes < 0) { fprintf(stderr, \"[cob] warning: harvest() with negative size, treated as 0\\n\"); bytes = 0; }\n"
"    return bytes;\n"
"}\n"
"static int cob_cmp_raw(CobValue l, CobValue r, int *mismatch) {\n"
"    int result;\n"
"    *mismatch = 0;\n"
"    if (l.is_str != r.is_str) { *mismatch = 1; result = 0; }\n"
"    else if (l.is_str) result = strcmp(l.s, r.s);\n"
"    else result = (l.i > r.i) - (l.i < r.i);\n"
"    cob_free(&l); cob_free(&r);\n"
"    return result;\n"
"}\n"
"static int cob_mismatch_warn(void) { fprintf(stderr, \"[cob] warning: comparing a number to a string; result is false\\n\"); return 0; }\n"
"static int cob_eq(CobValue l, CobValue r) { int m; int c = cob_cmp_raw(l, r, &m); return m ? cob_mismatch_warn() : (c == 0); }\n"
"static int cob_ne(CobValue l, CobValue r) { int m; int c = cob_cmp_raw(l, r, &m); return m ? cob_mismatch_warn() : (c != 0); }\n"
"static int cob_lt(CobValue l, CobValue r) { int m; int c = cob_cmp_raw(l, r, &m); return m ? cob_mismatch_warn() : (c < 0); }\n"
"static int cob_gt(CobValue l, CobValue r) { int m; int c = cob_cmp_raw(l, r, &m); return m ? cob_mismatch_warn() : (c > 0); }\n"
"static int cob_le(CobValue l, CobValue r) { int m; int c = cob_cmp_raw(l, r, &m); return m ? cob_mismatch_warn() : (c <= 0); }\n"
"static int cob_ge(CobValue l, CobValue r) { int m; int c = cob_cmp_raw(l, r, &m); return m ? cob_mismatch_warn() : (c >= 0); }\n"
"static int cob_truthy(CobValue v) { return v.is_str ? (v.s != NULL && v.s[0] != '\\0') : (v.i != 0); }\n"
"static void cob_pop(CobValue v) {\n"
"    if (v.is_str) { fwrite(v.s, 1, strlen(v.s), stdout); }\n"
"    else { char buf[32]; int n = snprintf(buf, sizeof(buf), \"%ld\", v.i); if (n > 0) fwrite(buf, 1, (size_t)n, stdout); }\n"
"    fputc('\\n', stdout);\n"
"    cob_free(&v);\n"
"}\n";

/* ---------------------------------------------------------------------
 * C API / FFI (CEP-2) -- FFI CODEGEN
 *
 * The native .so/.dll's exported functions use include/cob.h's CobValue
 * (tag + union of long/const char*). The generated program already has
 * its OWN struct named CobValue (the `{is_str; long i; char *s;}` shape
 * COB_RUNTIME_PREAMBLE defines, used for every Cob variable) -- #include
 * "cob.h" directly here would collide on that name, and would also
 * require the spawned compiler to be able to find Cob's include/
 * directory, which an end user's own toolchain has no reason to know
 * about. Instead, this emits a second, *locally* named struct --
 * CobFFIValue -- with the exact same tag/union layout as cob.h's
 * CobValue, field for field. A C struct's calling-convention ABI is
 * determined by that concrete layout, not by which translation unit
 * picked which name for the type, so a CobFFIValue passed by value here
 * is laid out identically to, and is safe to pass to, a function that
 * was compiled elsewhere against the real cob.h's CobValue -- the same
 * principle any two C codebases rely on when they independently define
 * a byte-identical struct to cross a link boundary without sharing a
 * header. COB_FFI_INT/COB_FFI_STR are given explicit values (0/1) to
 * guarantee they match cob.h's COB_INT/COB_STR numerically, since nboth
 * enums are compiled separately and nothing but matching values ties
 * them together.
 * ------------------------------------------------------------------- */
static const char *COB_FFI_PREAMBLE =
"typedef enum { COB_FFI_INT = 0, COB_FFI_STR = 1 } CobFFIValueKind;\n"
"typedef struct { CobFFIValueKind kind; union { long i; const char *s; } as; } CobFFIValue;\n";

/* One `extern CobFFIValue <name>(CobFFIValue*, int);` forward declaration
 * (the real link happens via the compiler's command line -- see
 * build_extern_link_args() in main()) plus one small adapter per
 * distinct extern name, each used by every EXPR_EXTERN_CALL call site
 * for that name (see emit_expr's EXPR_EXTERN_CALL case). The adapter:
 *   1. converts each popcorn-internal CobValue argument to a CobFFIValue
 *      and FREES that CobValue afterward -- operators like cob_add()
 *      already free their operands the same way, so an extern call
 *      consuming its arguments is consistent with the rest of this
 *      runtime, and matches cob_interp.c's real eval_expr(), which
 *      value_free()s each evaluated argument right after the call.
 *   2. calls the native function once
 *   3. converts the CobFFIValue result back -- cob_str() copies a
 *      returned string immediately (matching cob.h's documented
 *      "Cob copies it and never frees it" contract), cob_int() is a
 *      plain value copy either way.
 * argc==0 calls pass a NULL args pointer; the loop below never
 * dereferences it when argc is 0, so that's safe. */
static int emit_extern_adapters(StrBuf *b, const ExternSet *es) {
    size_t i;
    if (es->count == 0) return 0;
    if (sb_append(b, COB_FFI_PREAMBLE) != 0) return -1;
    for (i = 0; i < es->count; i++) {
        const char *name = es->items[i].name;
        if (sb_appendf(b, "extern CobFFIValue %s(CobFFIValue *, int);\n", name) != 0) return -1;
        /* Build cob_ffi_buf[] aliasing each argument's string pointer
         * WITHOUT freeing the source CobValue yet -- argv[i].as.s must
         * stay valid for the duration of the call (cob.h's documented
         * contract), so cob_free() only runs on each original CobValue
         * AFTER the native call returns, mirroring cob_interp.c's real
         * eval_expr() (which evaluates all args, calls b->fn(), and only
         * THEN value_free()s them). Freeing first and aliasing into the
         * already-freed buffer was an earlier bug here: the native call
         * would read freed memory and see garbage. */
        if (sb_appendf(b,
            "static CobValue cob_call_%s(CobValue *cob_ffi_args, int cob_ffi_argc) {\n"
            "    CobFFIValue cob_ffi_buf[%s];\n"
            "    int cob_ffi_i;\n"
            "    CobFFIValue cob_ffi_r;\n"
            "    CobValue cob_ffi_result;\n"
            "    for (cob_ffi_i = 0; cob_ffi_i < cob_ffi_argc; cob_ffi_i++) {\n"
            "        if (cob_ffi_args[cob_ffi_i].is_str) {\n"
            "            cob_ffi_buf[cob_ffi_i].kind = COB_FFI_STR;\n"
            "            cob_ffi_buf[cob_ffi_i].as.s = cob_ffi_args[cob_ffi_i].s;\n"
            "        } else {\n"
            "            cob_ffi_buf[cob_ffi_i].kind = COB_FFI_INT;\n"
            "            cob_ffi_buf[cob_ffi_i].as.i = cob_ffi_args[cob_ffi_i].i;\n"
            "        }\n"
            "    }\n"
            "    cob_ffi_r = %s(cob_ffi_argc > 0 ? cob_ffi_buf : NULL, cob_ffi_argc);\n"
            "    cob_ffi_result = cob_ffi_r.kind == COB_FFI_STR ? cob_str(cob_ffi_r.as.s ? cob_ffi_r.as.s : \"\") : cob_int(cob_ffi_r.as.i);\n"
            "    for (cob_ffi_i = 0; cob_ffi_i < cob_ffi_argc; cob_ffi_i++) cob_free(&cob_ffi_args[cob_ffi_i]);\n"
            "    return cob_ffi_result;\n"
            "}\n",
            name, "cob_ffi_argc > 0 ? (size_t)cob_ffi_argc : 1", name) != 0) return -1;
    }
    return 0;
}

/* Builds the full, self-contained C translation unit for `prog`. `externs`
 * is the already-collected, conflict-checked extern table (see
 * collect_externs()) -- pass an empty ExternSet for a program that
 * doesn't use `extern` at all, which leaves the generated C identical
 * to what it was before CEP-2 (no CobFFIValue, no adapters emitted). */
static char *transpile(const Program *prog, const ExternSet *externs) {
    StrBuf b; sb_init(&b);
    NameSet ns; nameset_init(&ns);
    collect_program(prog, &ns);

    if (sb_append(&b,
        "#include <stdio.h>\n"
        "#include <stdlib.h>\n"
        "#include <stddef.h>\n"
        "#include <stdint.h>\n"
        "#include <string.h>\n") != 0) goto fail;
    if (sb_append(&b, COB_RUNTIME_PREAMBLE) != 0) goto fail;
    if (emit_extern_adapters(&b, externs) != 0) goto fail;
    if (sb_append(&b, "int main(void) {\n") != 0) goto fail;

    size_t i;
    for (i = 0; i < ns.count; i++) {
        if (sb_appendf(&b, "CobValue cob_var_%s = cob_int(0);\n", ns.names[i]) != 0) goto fail;
    }
    if (emit_program(&b, prog) != 0) goto fail;
    for (i = 0; i < ns.count; i++) {
        if (sb_appendf(&b, "cob_free(&cob_var_%s);\n", ns.names[i]) != 0) goto fail;
    }
    if (sb_append(&b, "return 0;\n}\n") != 0) goto fail;

    free(ns.names);
    return b.data;
fail:
    free(ns.names);
    free(b.data);
    return NULL;
}

/* ---------------------------------------------------------------------
 * MAIN
 * ------------------------------------------------------------------- */
/* ---------------------------------------------------------------------
 * COMPILER RESOLUTION -- deciding which real C compiler to spawn
 * ------------------------------------------------------------------- */

/* ---------------------------------------------------------------------
 * COMPILER RESOLUTION -- deciding which real C compiler to spawn
 * ---------------------------------------------------------------------
 * Default backend is Zig's `zig cc` (a Clang-based drop-in C compiler
 * that ships bundled libc/CRT files for essentially every OS/arch it
 * supports, all in ONE Zig install). That's a genuinely better fit
 * for CobOS/CobArch cross-compiling than a table of separately
 * installed cross-gcc binaries: instead of needing a different
 * toolchain package per target, cross-compiling is just a `-target
 * <triple>` flag Zig already knows how to satisfy on its own.
 * See COB_ZIG_TARGETS below for the (CobOS, CobArch) -> Zig triple
 * mapping. Linux targets default to musl libc rather than glibc,
 * producing fully static binaries with no runtime libc dependency --
 * a better match for "standalone executable" than a dynamically
 * linked glibc binary would be.
 * ------------------------------------------------------------------- */

typedef struct { const char *os; const char *arch; const char *triple; } ZigTargetEntry;
static const ZigTargetEntry COB_ZIG_TARGETS[] = {
    { "windows", "amd64", "x86_64-windows-gnu" },
    { "windows", "386",   "x86-windows-gnu" },
    { "windows", "arm64", "aarch64-windows-gnu" },
    /* windows/arm (32-bit ARM) intentionally not supported -- cut per
     * project decision; it was also the entry we were least confident
     * about (32-bit ARM Windows is a niche LLVM target). */
    { "linux",   "amd64", "x86_64-linux-musl" },
    { "linux",   "386",   "x86-linux-musl" },
    { "linux",   "arm64", "aarch64-linux-musl" },
    { "linux",   "arm",   "arm-linux-musleabihf" },
    { "darwin",  "amd64", "x86_64-macos" },
    { "darwin",  "arm64", "aarch64-macos" },
    { NULL, NULL, NULL }
};

/* ---------------------------------------------------------------------
 * BUNDLED ZIG DETECTION
 * ---------------------------------------------------------------------
 * Official releases ship a "zig/" folder (a full Zig distribution --
 * the "zig"/"zig.exe" binary plus the "lib/" directory it needs at
 * runtime for its bundled libc headers and standard library) right
 * next to the popcorn_comp binary. This is checked before falling
 * back to a plain "zig cc" on PATH, so a downloaded release works
 * standalone with no separate Zig install required. Local/dev builds
 * (no bundled zig/ folder present) transparently fall back to PATH.
 * ------------------------------------------------------------------- */

/* Returns a newly malloc'd string containing the directory this
 * executable lives in (no trailing separator), or NULL if it can't be
 * determined. Deliberately does NOT rely on argv[0], which can be
 * unreliable (e.g. resolved via a PATH search, or a relative path
 * that's no longer valid after a chdir) -- uses the platform's real
 * "path to the running executable" API instead. */
/* Used only to build the CEP-2 extern-library rpath (see
 * build_extern_link_args()) -- NULL on any failure is handled by simply
 * skipping that rpath, not by failing the whole compile. */
static char *cob_getcwd(void) {
    char buf[4096];
#if defined(_WIN32)
    if (!_getcwd(buf, (int)sizeof(buf))) return NULL;
#else
    if (!getcwd(buf, sizeof(buf))) return NULL;
#endif
    {
        size_t len = strlen(buf);
        char *result = (char *)malloc(len + 1);
        if (!result) return NULL;
        memcpy(result, buf, len + 1);
        return result;
    }
}

static char *get_executable_dir(void) {
    char buf[4096];
    char *last_sep;

#if defined(_WIN32)
    DWORD len = GetModuleFileNameA(NULL, buf, (DWORD)sizeof(buf));
    if (len == 0 || len >= sizeof(buf)) return NULL;
#elif defined(__APPLE__)
    uint32_t size = (uint32_t)sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) != 0) return NULL;
#else
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len <= 0) return NULL;
    buf[len] = '\0';
#endif

    last_sep = strrchr(buf, '/');
#if defined(_WIN32)
    { char *bs = strrchr(buf, '\\'); if (bs && (!last_sep || bs > last_sep)) last_sep = bs; }
#endif
    if (!last_sep) return NULL;
    *last_sep = '\0';

    {
        size_t dir_len = strlen(buf);
        char *result = (char *)malloc(dir_len + 1);
        if (!result) return NULL;
        memcpy(result, buf, dir_len + 1);
        return result;
    }
}

/* Looks for <exe_dir>/zig/zig (or zig.exe on Windows). If found,
 * returns a newly malloc'd, already-quoted invocation like
 * "\"/path/to/zig/zig\" cc" ready to have arguments appended. Returns
 * NULL if no bundled Zig is present (caller falls back to PATH). */
static char *find_bundled_zig_cc(void) {
    char *dir = get_executable_dir();
    char path[4200];
    char *result;
    size_t path_len;

    if (!dir) return NULL;
#if defined(_WIN32)
    snprintf(path, sizeof(path), "%s\\zig\\zig.exe", dir);
#else
    snprintf(path, sizeof(path), "%s/zig/zig", dir);
#endif
    free(dir);

    if (!cob_file_exists(path)) return NULL;

    path_len = strlen(path);
    result = (char *)malloc(path_len + 8); /* 2 quotes + " cc" + NUL */
    if (!result) return NULL;
    snprintf(result, path_len + 8, "\"%s\" cc", path);
    return result;
}

/* Returns a full compiler *command* (may include arguments, e.g.
 * "zig cc -target x86_64-windows-gnu", or a bundled path like
 * "\"/path/to/zig/zig\" cc -target ...") to invoke, per the resolution
 * order documented at the top of this file. The returned string is
 * either heap-owned (caller must free(), whenever *out_owned is 1) or
 * a static literal (caller must NOT free()). Returns NULL on an
 * unresolvable request (already reported to stderr). */
static char *resolve_compiler(const char *cli_cc_override, int *out_owned) {
    const char *env_cc;
    const char *want_os, *want_arch;
    char *base_cmd;
    int base_owned;
    int i;

    *out_owned = 0;

    if (cli_cc_override && cli_cc_override[0] != '\0') {
        return (char *)cli_cc_override;
    }

    env_cc = getenv("CobCC");
    if (env_cc && env_cc[0] != '\0') {
        return (char *)env_cc;
    }

    /* Base zig invocation: a bundled zig/ folder next to this
     * executable if present (official releases), otherwise plain
     * "zig cc" relying on PATH (local/dev builds, or a system-wide
     * Zig install). */
    base_cmd = find_bundled_zig_cc();
    if (base_cmd) {
        base_owned = 1;
    } else {
        base_cmd = (char *)"zig cc";
        base_owned = 0;
    }

    want_os = getenv("CobOS");
    want_arch = getenv("CobArch");
    if (want_os && want_arch) {
        for (i = 0; COB_ZIG_TARGETS[i].os != NULL; i++) {
            if (strcmp(COB_ZIG_TARGETS[i].os, want_os) == 0 &&
                strcmp(COB_ZIG_TARGETS[i].arch, want_arch) == 0) {
                size_t cap = strlen(base_cmd) + 64;
                char *cmd = (char *)malloc(cap);
                if (!cmd) { if (base_owned) free(base_cmd); return NULL; }
                snprintf(cmd, cap, "%s -target %s", base_cmd, COB_ZIG_TARGETS[i].triple);
                if (base_owned) free(base_cmd);
                *out_owned = 1;
                return cmd;
            }
        }
        /* CobOS/CobArch given but not in our table -- rather than
         * silently falling back to a native compile that would
         * produce the WRONG platform's binary, refuse clearly. */
        fprintf(stderr,
            "[popcorn_comp] error: no known Zig target for CobOS=%s CobArch=%s.\n"
            "  Pass --cc <path> or set $CobCC to the compiler you want used.\n",
            want_os, want_arch);
        if (base_owned) free(base_cmd);
        return NULL;
    }

    /* No explicit target requested: base_cmd (bundled or PATH zig) is
     * already exactly the native compile command we want. */
    *out_owned = base_owned;
    return base_cmd;
}

/* Runs `compiler -std=c99 -O2 -o out_path in_path`, quoting both paths
 * for the shell. Returns 0 on success (subprocess exited 0 AND the
 * output file actually exists -- exit-code alone isn't fully reliable
 * for catching every failure mode across shells/platforms). */
static int cob_spawn_compile(const char *compiler, const char *in_path, const char *out_path,
                              const char *extra_link_args) {
    /* 2048 -> 8192: extra_link_args (one -lname or one quoted literal
     * path per distinct extern library, CEP-2) can add up across a
     * program with several different native extensions. */
    char cmd[8192];
    int written;
    char *q_in, *q_out;
    size_t in_len = strlen(in_path), out_len = strlen(out_path);
    int rc;

    /* Simple, portable shell-quoting: wrap in double quotes and escape
     * any embedded double quotes/backslashes. Compiler-supplied paths
     * here are always ones popcorn_comp itself generated (a temp file
     * path and the user's -o argument), never arbitrary network input,
     * but we quote anyway as standard practice for anything crossing
     * into a shell command line. */
    q_in = (char *)malloc(in_len * 2 + 3);
    q_out = (char *)malloc(out_len * 2 + 3);
    if (!q_in || !q_out) { free(q_in); free(q_out); return -1; }
    {
        size_t i, j = 0;
        q_in[j++] = '"';
        for (i = 0; i < in_len; i++) {
            if (in_path[i] == '"' || in_path[i] == '\\') q_in[j++] = '\\';
            q_in[j++] = in_path[i];
        }
        q_in[j++] = '"'; q_in[j] = '\0';
    }
    {
        size_t i, j = 0;
        q_out[j++] = '"';
        for (i = 0; i < out_len; i++) {
            if (out_path[i] == '"' || out_path[i] == '\\') q_out[j++] = '\\';
            q_out[j++] = out_path[i];
        }
        q_out[j++] = '"'; q_out[j] = '\0';
    }

    written = snprintf(cmd, sizeof(cmd), "%s -std=c99 -O2 -o %s %s%s%s",
        compiler, q_out, q_in,
        (extra_link_args && extra_link_args[0]) ? " " : "",
        extra_link_args ? extra_link_args : "");
    free(q_in); free(q_out);
    if (written < 0 || (size_t)written >= sizeof(cmd)) {
        fprintf(stderr, "[popcorn_comp] error: compiler command line too long (too many extern libraries?)\n");
        return -1;
    }

    fprintf(stderr, "[popcorn_comp] spawning: %s\n", cmd);
#if defined(_WIN32)
    /* cmd.exe strips only the first and last '"' of a command line that
     * starts with a quote, not matching pairs -- if there's more than one
     * quoted token (e.g. a quoted compiler path plus quoted -o/in args),
     * that corrupts the line. Wrapping the whole thing in one more pair
     * of quotes is the standard workaround. Only triggers when the
     * compiler string itself is pre-quoted, i.e. the bundled-Zig
     * auto-resolve path (find_bundled_zig_cc); --cc/CobCC strings are
     * untouched. */
    if (cmd[0] == '"') {
        size_t wrapped_len = strlen(cmd) + 3;
        char *wrapped = (char *)malloc(wrapped_len);
        if (wrapped) {
            snprintf(wrapped, wrapped_len, "\"%s\"", cmd);
            rc = system(wrapped);
            free(wrapped);
        } else {
            rc = system(cmd);
        }
    } else {
        rc = system(cmd);
    }
#else
    rc = system(cmd);
#endif
    if (rc != 0) return -1;
    if (!cob_file_exists(out_path)) return -1;
    return 0;
}

/* ---------------------------------------------------------------------
 * C API / FFI (CEP-2) -- LINK ARGUMENTS
 *
 * One token per distinct extern library (see extern_lib_looks_like_path()
 * above for which form it takes), space-separated, ready to append
 * straight onto the spawned compiler's command line. Returns a malloc'd
 * string (possibly "", never NULL, on success), or NULL on OOM/overflow.
 * ------------------------------------------------------------------- */
/* $CobOS, if set, already picks the cross-compile target elsewhere
 * (resolve_compiler()); fall back to the host popcorn_comp itself was
 * built for when it's not set, i.e. a native (non-cross) build. */
static int target_is_windows(void) {
    const char *want_os = getenv("CobOS");
    if (want_os) return strcmp(want_os, "windows") == 0;
#if defined(_WIN32)
    return 1;
#else
    return 0;
#endif
}

static int sb_append_quoted(StrBuf *b, const char *s) {
    size_t len = strlen(s), j;
    if (sb_append(b, "\"") != 0) return -1;
    for (j = 0; j < len; j++) {
        if (s[j] == '"' || s[j] == '\\') { if (sb_append(b, "\\") != 0) return -1; }
        { char one[2] = { s[j], '\0' }; if (sb_append(b, one) != 0) return -1; }
    }
    return sb_append(b, "\"");
}

/* out_cwd: popcorn_comp's own working directory at compile time, already
 * resolved by main() via getcwd() -- NULL if that failed (rpath is then
 * just skipped; the binary still links and runs, only "the library sitting
 * next to the .cob script" stops being found automatically). */
static char *build_extern_link_args(const ExternSet *es, const char *out_cwd) {
    StrBuf b; sb_init(&b);
    size_t i;
    int any_path_lib = 0;
    NameSet seen_libs; nameset_init(&seen_libs); /* dedupe identical libraries across distinct names */

    for (i = 0; i < es->count; i++) {
        const char *lib = es->items[i].lib;
        int already = 0, j;
        for (j = 0; j < (int)seen_libs.count; j++) if (strcmp(seen_libs.names[j], lib) == 0) { already = 1; break; }
        if (already) continue;
        if (nameset_add(&seen_libs, lib) != 0) { free(seen_libs.names); free(b.data); return NULL; }

        if (b.len > 0 && sb_append(&b, " ") != 0) { free(seen_libs.names); free(b.data); return NULL; }
        if (extern_lib_looks_like_path(lib)) {
            any_path_lib = 1;
            if (sb_append_quoted(&b, lib) != 0) { free(seen_libs.names); free(b.data); return NULL; }
        } else {
            if (sb_appendf(&b, "-l%s", lib) != 0) { free(seen_libs.names); free(b.data); return NULL; }
        }
    }
    free(seen_libs.names);

    /* A bare filename like "math_ext.so" (no directory component) links
     * fine, but the resulting binary's DT_NEEDED is just that bare name,
     * which the dynamic linker then searches for on the SYSTEM library
     * path at run time -- not next to the compiled executable, and not
     * in whatever directory popcorn_comp happened to run from. Pointing
     * an rpath at that build-time directory makes "the .so sitting next
     * to the .cob script" resolve automatically for the common case of
     * running the compiled binary from that same directory, without
     * requiring $LD_LIBRARY_PATH -- real linking, per CEP-2, just with
     * one extra flag so it actually finds the library by default. Native
     * ELF/Mach-O POSIX targets only: PE's DLL search order already
     * includes the executable's own directory, so Windows needs nothing
     * extra here (and -Wl,-rpath isn't a thing MinGW ld accepts the
     * same way). */
    if (any_path_lib && out_cwd && !target_is_windows()) {
        if (b.len > 0 && sb_append(&b, " ") != 0) { free(b.data); return NULL; }
        if (sb_append(&b, "-Wl,-rpath,") != 0) { free(b.data); return NULL; }
        if (sb_append_quoted(&b, out_cwd) != 0) { free(b.data); return NULL; }
    }

    if (!b.data) { b.data = (char *)calloc(1, 1); if (!b.data) return NULL; } /* "" for zero externs */
    return b.data;
}

/* ---------------------------------------------------------------------
 * MAIN
 * ------------------------------------------------------------------- */
static void print_usage(const char *argv0) {
    fprintf(stderr,
        "Usage: %s <file%s> -o <output> [--no-gc] [--cc <compiler>] [--emit-c <path.c>]\n"
        "       %s --license\n"
        "       %s --version\n"
        "  --no-gc        required if the program uses harvest()/trash()\n"
        "  --cc PATH      explicit compiler to spawn (default: resolved from\n"
        "                 CobCC / CobOS+CobArch / PATH -- see file header comment)\n"
        "  --emit-c PATH  also write the generated C source for inspection\n"
        "  --license      print the short license notice and exit\n"
        "\n"
        "Environment:\n"
        "  CobCC    explicit compiler command/path override\n"
        "  CobOS    windows|linux|darwin  (looked up in a small cross-compiler table)\n"
        "  CobArch  amd64|386|arm64|arm\n",
        argv0, COB_CACHE_EXTENSION, argv0, argv0);
}

int main(int argc, char **argv) {
    const char *in_path = NULL;
    const char *out_path = NULL;
    const char *emit_c_path = NULL;
    const char *cc_override = NULL;
    int no_gc = 0;
    int i;

    if (cob_check_hidden_version_flag(argc, argv)) return 0;
    if (cob_check_hidden_license_flag(argc, argv)) return 0;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "[popcorn_comp] error: -o needs an argument\n"); return 1; }
            out_path = argv[++i];
        } else if (strcmp(argv[i], "--no-gc") == 0) {
            no_gc = 1;
        } else if (strcmp(argv[i], "--cc") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "[popcorn_comp] error: --cc needs an argument\n"); return 1; }
            cc_override = argv[++i];
        } else if (strcmp(argv[i], "--emit-c") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "[popcorn_comp] error: --emit-c needs an argument\n"); return 1; }
            emit_c_path = argv[++i];
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "[popcorn_comp] error: unrecognized flag '%s'\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        } else if (in_path == NULL) {
            in_path = argv[i];
        } else {
            fprintf(stderr, "[popcorn_comp] error: unexpected extra argument '%s'\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }
    if (in_path == NULL || out_path == NULL) {
        print_usage(argv[0]);
        return 1;
    }

    int compiler_owned;
    char *compiler = resolve_compiler(cc_override, &compiler_owned);
    if (!compiler) return 1; /* resolve_compiler() already printed why */

    /* --- load and check the bytecode --- */
    Program prog;
    if (cob_load_strawberry(in_path, &prog) != 0) return 1;

    if (!no_gc && program_uses_harvest_or_trash(&prog)) {
        fprintf(stderr,
            "[popcorn_comp] error: this program uses harvest()/trash() -- rerun with --no-gc\n");
        return 1;
    }

    /* --- C API / FFI (CEP-2): collect extern declarations, conflict-check --- */
    ExternSet externs; externset_init(&externs);
    if (collect_externs(&prog, &externs) != 0) {
        externset_free(&externs); /* externset_add() already reported the conflict, if any */
        return 1;
    }
    char *cwd = cob_getcwd(); /* NULL is fine -- see build_extern_link_args() */
    char *extra_link_args = build_extern_link_args(&externs, cwd);
    free(cwd);
    if (!extra_link_args) {
        fprintf(stderr, "[popcorn_comp] error: out of memory building extern link arguments\n");
        externset_free(&externs);
        return 1;
    }

    /* --- transpile to C --- */
    char *c_source = transpile(&prog, &externs);
    externset_free(&externs);
    if (!c_source) {
        fprintf(stderr, "[popcorn_comp] error: out of memory generating C source\n");
        free(extra_link_args);
        return 1;
    }

    /* The generated C always needs to live in a real file on disk for
     * a spawned compiler to read (unlike libtcc's tcc_compile_string,
     * a subprocess can't be handed an in-memory string directly). Use
     * --emit-c's path if given so it doubles as the compile input and
     * the inspectable artifact; otherwise use a temp file next to the
     * requested output and clean it up afterward. */
    char temp_c_path[600];
    int using_emit_c_as_input = (emit_c_path != NULL);
    if (using_emit_c_as_input) {
        snprintf(temp_c_path, sizeof(temp_c_path), "%s", emit_c_path);
    } else {
        snprintf(temp_c_path, sizeof(temp_c_path), "%s.popcorn_tmp.c", out_path);
    }

    if (cob_file_write_all(temp_c_path, c_source, strlen(c_source)) != COB_FILE_OK) {
        fprintf(stderr, "[popcorn_comp] error: could not write generated C source to '%s'\n", temp_c_path);
        free(c_source);
        free(extra_link_args);
        return 1;
    }
    free(c_source);

    /* --- spawn the real compiler --- */
    if (cob_spawn_compile(compiler, temp_c_path, out_path, extra_link_args) != 0) {
        fprintf(stderr,
            "[popcorn_comp] error: '%s' failed to compile the generated C source\n"
            "  (rerun with --emit-c out.c to inspect it, or check that '%s' is a valid,\n"
            "  installed compiler on PATH%s)\n",
            compiler, compiler,
            (extra_link_args[0] ? ", and that every extern library resolves at link time" : ""));
        if (!using_emit_c_as_input) remove(temp_c_path);
        free(extra_link_args);
        return 1;
    }

    if (!using_emit_c_as_input) remove(temp_c_path);

    fprintf(stderr, "[popcorn_comp] wrote %s (compiler: %s)\n", out_path, compiler);
    free(extra_link_args);
    if (compiler_owned) free(compiler);
    return 0;
}

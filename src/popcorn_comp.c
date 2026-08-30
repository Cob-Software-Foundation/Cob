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
typedef enum { EXPR_NUM, EXPR_VAR, EXPR_BINOP, EXPR_HARVEST } ExprKind;
typedef struct Expr {
    ExprKind kind;
    long num;
    char *var;
    char op;
    struct Expr *left;
    struct Expr *right;
} Expr;

typedef enum { COND_TRUTHY, COND_EQ, COND_NE, COND_LT, COND_GT, COND_LE, COND_GE } CondOp;
typedef struct { Expr *left; CondOp op; Expr *right; } Cond;

typedef enum { STMT_POP, STMT_SET, STMT_WHILE, STMT_TRASH } StmtKind;
typedef struct Program { struct Stmt *items; size_t count, capacity; } Program;
typedef struct Stmt {
    StmtKind kind;
    char *pop_text; size_t pop_text_len;
    char *set_name; Expr *set_expr;
    Cond *while_cond; Program while_body;
    char *trash_name;
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
 * .strawberry READER (format v2 -- must stay byte-for-byte compatible
 * with cob_interp.c's writer). See cob_interp.c for the authoritative
 * format documentation; duplicated in miniature here deliberately.
 * ------------------------------------------------------------------- */
#define STRAWBERRY_MAGIC      "COBSTRW2"
#define STRAWBERRY_MAGIC_LEN  8

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
        case STMT_POP: return reader_bytes_alloc(r, &out->pop_text, &out->pop_text_len);
        case STMT_SET:
            if (reader_bytes_alloc(r, &out->set_name, NULL) != 0) return -1;
            return read_expr(r, &out->set_expr);
        case STMT_TRASH: return reader_bytes_alloc(r, &out->trash_name, NULL);
        case STMT_WHILE:
            if (read_cond(r, &out->while_cond) != 0) return -1;
            return read_program(r, &out->while_body);
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
        fprintf(stderr, "[popcorn_comp] error: '%s' is not a valid .strawberry v2 file\n", path);
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
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
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
}
static void collect_cond(const Cond *c, NameSet *ns) { collect_expr(c->left, ns); collect_expr(c->right, ns); }
static void collect_program(const Program *p, NameSet *ns) {
    size_t i;
    for (i = 0; i < p->count; i++) {
        const Stmt *s = &p->items[i];
        switch (s->kind) {
            case STMT_SET: nameset_add(ns, s->set_name); collect_expr(s->set_expr, ns); break;
            case STMT_TRASH: nameset_add(ns, s->trash_name); break;
            case STMT_WHILE: collect_cond(s->while_cond, ns); collect_program(&s->while_body, ns); break;
            default: break;
        }
    }
}
static int program_uses_harvest_or_trash(const Program *p); /* fwd */

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
        case EXPR_NUM: return sb_appendf(b, "%ldL", e->num);
        case EXPR_VAR: return sb_appendf(b, "cob_var_%s", e->var);
        case EXPR_BINOP:
            if (sb_append(b, "(") != 0) return -1;
            if (emit_expr(b, e->left) != 0) return -1;
            if (sb_appendf(b, " %c ", e->op) != 0) return -1;
            if (emit_expr(b, e->right) != 0) return -1;
            return sb_append(b, ")");
        case EXPR_HARVEST:
            if (sb_append(b, "((long)(intptr_t)malloc((size_t)(") != 0) return -1;
            if (emit_expr(b, e->left) != 0) return -1;
            return sb_append(b, ")))");
    }
    return -1;
}
static const char *cond_op_str(CondOp op) {
    switch (op) {
        case COND_EQ: return "=="; case COND_NE: return "!=";
        case COND_LT: return "<";  case COND_GT: return ">";
        case COND_LE: return "<="; case COND_GE: return ">=";
        default: return NULL;
    }
}
static int emit_cond(StrBuf *b, const Cond *c) {
    if (c->op == COND_TRUTHY) return emit_expr(b, c->left);
    if (sb_append(b, "(") != 0) return -1;
    if (emit_expr(b, c->left) != 0) return -1;
    if (sb_appendf(b, " %s ", cond_op_str(c->op)) != 0) return -1;
    if (emit_expr(b, c->right) != 0) return -1;
    return sb_append(b, ")");
}
static int emit_program(StrBuf *b, const Program *p) {
    size_t i;
    for (i = 0; i < p->count; i++) {
        const Stmt *s = &p->items[i];
        switch (s->kind) {
            case STMT_POP:
                if (sb_append(b, "printf(") != 0) return -1;
                if (emit_c_string_literal(b, s->pop_text, s->pop_text_len) != 0) return -1;
                if (sb_append(b, ");\nprintf(\"\\n\");\n") != 0) return -1;
                break;
            case STMT_SET:
                if (sb_appendf(b, "cob_var_%s = ", s->set_name) != 0) return -1;
                if (emit_expr(b, s->set_expr) != 0) return -1;
                if (sb_append(b, ";\n") != 0) return -1;
                break;
            case STMT_TRASH:
                if (sb_appendf(b, "free((void*)(intptr_t)cob_var_%s); cob_var_%s = 0;\n",
                                s->trash_name, s->trash_name) != 0) return -1;
                break;
            case STMT_WHILE:
                if (sb_append(b, "while (") != 0) return -1;
                if (emit_cond(b, s->while_cond) != 0) return -1;
                if (sb_append(b, ") {\n") != 0) return -1;
                if (emit_program(b, &s->while_body) != 0) return -1;
                if (sb_append(b, "}\n") != 0) return -1;
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
    }
    return 0;
}

/* Builds the full, self-contained C translation unit for `prog`. */
static char *transpile(const Program *prog) {
    StrBuf b; sb_init(&b);
    NameSet ns; nameset_init(&ns);
    collect_program(prog, &ns);

    if (sb_append(&b,
        "#include <stdio.h>\n"
        "#include <stdlib.h>\n"
        "#include <stddef.h>\n"
        "#include <stdint.h>\n"
        "int main(void) {\n") != 0) goto fail;

    size_t i;
    for (i = 0; i < ns.count; i++) {
        if (sb_appendf(&b, "long cob_var_%s = 0;\n", ns.names[i]) != 0) goto fail;
    }
    if (emit_program(&b, prog) != 0) goto fail;
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
static int cob_spawn_compile(const char *compiler, const char *in_path, const char *out_path) {
    char cmd[2048];
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

    snprintf(cmd, sizeof(cmd), "%s -std=c99 -O2 -o %s %s", compiler, q_out, q_in);
    free(q_in); free(q_out);

    fprintf(stderr, "[popcorn_comp] spawning: %s\n", cmd);
    rc = system(cmd);
    if (rc != 0) return -1;
    if (!cob_file_exists(out_path)) return -1;
    return 0;
}

/* ---------------------------------------------------------------------
 * MAIN
 * ------------------------------------------------------------------- */
static void print_usage(const char *argv0) {
    fprintf(stderr,
        "Usage: %s <file%s> -o <output> [--no-gc] [--cc <compiler>] [--emit-c <path.c>]\n"
        "  --no-gc        required if the program uses harvest()/trash()\n"
        "  --cc PATH      explicit compiler to spawn (default: resolved from\n"
        "                 CobCC / CobOS+CobArch / PATH -- see file header comment)\n"
        "  --emit-c PATH  also write the generated C source for inspection\n"
        "\n"
        "Environment:\n"
        "  CobCC    explicit compiler command/path override\n"
        "  CobOS    windows|linux|darwin  (looked up in a small cross-compiler table)\n"
        "  CobArch  amd64|386|arm64|arm\n",
        argv0, COB_CACHE_EXTENSION);
}

int main(int argc, char **argv) {
    const char *in_path = NULL;
    const char *out_path = NULL;
    const char *emit_c_path = NULL;
    const char *cc_override = NULL;
    int no_gc = 0;
    int i;

    if (cob_check_hidden_version_flag(argc, argv)) return 0;

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

    /* --- transpile to C --- */
    char *c_source = transpile(&prog);
    if (!c_source) {
        fprintf(stderr, "[popcorn_comp] error: out of memory generating C source\n");
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
        return 1;
    }
    free(c_source);

    /* --- spawn the real compiler --- */
    if (cob_spawn_compile(compiler, temp_c_path, out_path) != 0) {
        fprintf(stderr,
            "[popcorn_comp] error: '%s' failed to compile the generated C source\n"
            "  (rerun with --emit-c out.c to inspect it, or check that '%s' is a valid,\n"
            "  installed compiler on PATH)\n",
            compiler, compiler);
        if (!using_emit_c_as_input) remove(temp_c_path);
        return 1;
    }

    if (!using_emit_c_as_input) remove(temp_c_path);

    fprintf(stderr, "[popcorn_comp] wrote %s (compiler: %s)\n", out_path, compiler);
    if (compiler_owned) free(compiler);
    return 0;
}

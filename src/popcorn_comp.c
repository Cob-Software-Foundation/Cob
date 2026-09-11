/* =====================================================================
 * popcorn_comp.c
 * ---------------------------------------------------------------------
 * Project Obsidian Falcon / Cob Language Toolchain
 * The Cob native compiler: `popcorn_comp <file.strawberry> -o <output> [--no-gc]`
 *
 * ---------------------------------------------------------------------
 * HOW THIS WORKS
 * ---------------------------------------------------------------------
 * Rather than shelling out to gcc/clang or dynamically linking a system
 * copy of TCC, this binary statically links TCC's actual compiler
 * source (src/tcc/libtcc.c, included as one translation unit -- see
 * the Makefile) directly into itself, exactly as the project spec
 * requires. At runtime it:
 *
 *   1. Reads a .strawberry bytecode file (the same format cob_interp.c
 *      writes -- see the format comment near STRAWBERRY_MAGIC below;
 *      the two files intentionally keep independent copies of the
 *      *reader* so popcorn_comp has no dependency on cob_interp.c).
 *   2. Transpiles the AST directly to a small, self-contained C source
 *      string (no external runtime library needed -- pop() becomes
 *      printf, variables become plain C `long` locals, harvest/trash
 *      become malloc/free).
 *   3. Feeds that C source to the embedded TCC via tcc_compile_string()
 *      and writes a real native executable with tcc_output_file().
 *
 * ---------------------------------------------------------------------
 * ON CobOS / CobArch AND CROSS-COMPILATION -- READ THIS
 * ---------------------------------------------------------------------
 * TCC's code generator is a compile-time choice (TCC_TARGET_X86_64,
 * TCC_TARGET_I386, TCC_TARGET_ARM64, ... -- baked into libtcc.c when
 * *this popcorn_comp binary itself* was built). A single statically-
 * linked popcorn_comp binary can only ever emit code for the ONE
 * target it was compiled for. True "one binary cross-compiles to any
 * of 10 platforms" is not something a single static libtcc supports --
 * that would require bundling N separate libtcc builds (one per
 * target) inside one executable, which upstream TCC does not do.
 *
 * So: popcorn_comp reads CobOS/CobArch via getenv() as the spec
 * requires, and checks them against POPCORN_HOST_OS/POPCORN_HOST_ARCH
 * (this binary's own compiled-in target, from common.h). If they
 * match (or are unset, meaning "use the host"), it proceeds. If they
 * name a *different* target, it fails with a clear explanation rather
 * than silently producing a host binary mislabeled as something else.
 * Real cross-target support means building separate popcorn_comp
 * binaries per target -- exactly the same pattern build.yml already
 * uses for cob_interp -- each statically linking a libtcc built with
 * a different TCC_TARGET_* macro. That's future work, not a lie this
 * binary tells about itself.
 *
 * License: PolyForm Noncommercial License 1.0.0 - see LICENSE.md
 * ===================================================================== */

#include "common.h"
#include "file_io.h"
#include "libtcc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

/* Where popcorn_comp looks for libtcc1.a and TCC's bundled headers at
 * runtime. The Makefile passes this via -D pointing at src/tcc for
 * local/dev builds; a packaged release should instead ship a
 * "runtime/" directory next to the popcorn_comp binary and pass that
 * path here instead. */
#ifndef POPCORN_TCC_RUNTIME_DIR
#define POPCORN_TCC_RUNTIME_DIR "."
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
static void print_usage(const char *argv0) {
    fprintf(stderr,
        "Usage: %s <file%s> -o <output> [--no-gc] [--emit-c <path.c>]\n"
        "  --no-gc        required if the program uses harvest()/trash()\n"
        "  --emit-c PATH  also write the generated C source for inspection\n"
        "\n"
        "Environment:\n"
        "  CobOS    windows|linux|darwin  (must match this build's target)\n"
        "  CobArch  amd64|386|arm64       (must match this build's target)\n",
        argv0, COB_CACHE_EXTENSION);
}

int main(int argc, char **argv) {
    const char *in_path = NULL;
    const char *out_path = NULL;
    const char *emit_c_path = NULL;
    int no_gc = 0;
    int i;

    if (cob_check_hidden_version_flag(argc, argv)) return 0;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "[popcorn_comp] error: -o needs an argument\n"); return 1; }
            out_path = argv[++i];
        } else if (strcmp(argv[i], "--no-gc") == 0) {
            no_gc = 1;
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

    /* --- CobOS / CobArch validation (see the file-top comment) --- */
    const char *want_os = getenv("CobOS");
    const char *want_arch = getenv("CobArch");
    if (want_os && strcmp(want_os, COB_OS_NAME) != 0) {
        fprintf(stderr,
            "[popcorn_comp] error: CobOS=%s requested, but this popcorn_comp binary\n"
            "  was statically built for '%s' and cannot target another OS.\n"
            "  Build a separate popcorn_comp for '%s' (see .github/workflows/build.yml\n"
            "  for the same per-target pattern used for cob_interp).\n",
            want_os, COB_OS_NAME, want_os);
        return 1;
    }
    if (want_arch && strcmp(want_arch, COB_ARCH_NAME) != 0) {
        fprintf(stderr,
            "[popcorn_comp] error: CobArch=%s requested, but this popcorn_comp binary\n"
            "  was statically built for '%s' and cannot target another arch.\n",
            want_arch, COB_ARCH_NAME);
        return 1;
    }

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
    if (emit_c_path) {
        if (cob_file_write_all(emit_c_path, c_source, strlen(c_source)) != COB_FILE_OK) {
            fprintf(stderr, "[popcorn_comp] warning: could not write --emit-c file '%s'\n", emit_c_path);
        }
    }

    /* --- compile via the statically-linked TCC --- */
    TCCState *s = tcc_new();
    if (!s) {
        fprintf(stderr, "[popcorn_comp] error: tcc_new() failed\n");
        free(c_source);
        return 1;
    }
    /* TCC needs to find its own runtime archive (libtcc1.a) and headers
     * at runtime; point it at the tree this binary was built from.
     * Overridable via the POPCORN_TCC_RUNTIME_DIR environment variable
     * (used by CI, and by anyone running popcorn_comp from a location
     * other than a full source checkout) without needing a rebuild. A
     * packaged release ships a "runtime/" dir (libtcc1.a + include/)
     * next to the popcorn_comp binary and expects to be run from that
     * directory -- see README for the exact layout. */
    const char *runtime_dir = getenv("POPCORN_TCC_RUNTIME_DIR");
    if (!runtime_dir || runtime_dir[0] == '\0') runtime_dir = POPCORN_TCC_RUNTIME_DIR;
    tcc_set_lib_path(s, runtime_dir);
    tcc_set_output_type(s, TCC_OUTPUT_EXE);

    if (tcc_compile_string(s, c_source) < 0) {
        fprintf(stderr, "[popcorn_comp] error: internal error -- generated C source failed to compile\n");
        if (!emit_c_path) fprintf(stderr, "  (rerun with --emit-c out.c to inspect the generated source)\n");
        tcc_delete(s);
        free(c_source);
        return 1;
    }
    if (tcc_output_file(s, out_path) != 0) {
        fprintf(stderr, "[popcorn_comp] error: failed to write output executable '%s'\n", out_path);
        tcc_delete(s);
        free(c_source);
        return 1;
    }

    tcc_delete(s);
    free(c_source);

    fprintf(stderr, "[popcorn_comp] wrote %s (target: %s/%s)\n", out_path, COB_OS_NAME, COB_ARCH_NAME);
    return 0;
}

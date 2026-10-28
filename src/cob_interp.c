/* =====================================================================
 * cob_interp.c
 * ---------------------------------------------------------------------
 * Project Obsidian Falcon / Cob Language Toolchain
 * The Cob interpreter: `cob_interp [file.cob] [--no-cache] [--no-gc] [-i]`
 * With no file (or with --repl) it starts the interactive REPL.
 *
 * ---------------------------------------------------------------------
 * THE CORE LANGUAGE (everything built in)
 * ---------------------------------------------------------------------
 *   pop(<expr>)               writes text to stdout + newline.
 *   set <name> = <expr>       integer OR string variables; a variable's
 *                             "type" is whatever it was last set to.
 *   + - * /                   arithmetic; '+' with a string joins text.
 *   while <condition>:        loops (== != < > <= >=, or a truthiness test)
 *   if / elif / else          conditionals
 *   recipe <name>(<params>):  defines a function (top level only), like
 *                             Python's `def`. Parameters and every name a
 *                             recipe `set`s are local to it; other names
 *                             read the globals.
 *   serve [<expr>]            hands a value back from a recipe (like
 *                             `return`); a recipe that ends without it
 *                             gives 0. Calls: name(a, b), in any
 *                             expression or on a line of their own.
 *   shuck <library>           loads <library>.cob (current directory, then
 *                             cob_modules/<library>/, then the standard
 *                             library directory) and splices it in.
 *   extern <f>(<params>) from "<lib>"
 *                             binds a C function from a shared library
 *                             (CEP-2). The standard library is built on it.
 *   harvest(<bytes>) / trash(<var>)   raw memory, only with --no-gc.
 *   _MakeCache = False        first line: skip the .strawberry cache.
 *
 * EVERYTHING ELSE IS THE STANDARD LIBRARY (v0.0.9). Console input, text,
 * time, files, SQLite, windows and Tcl/Tk are no longer built into this
 * program: each is a shared library (.so / .dylib / .dll) in the standard
 * library directory, loaded with `shuck`. Every name in it is corn themed:
 *     shuck porch      porch_ask(), porch_ask_int()      (console input)
 *     shuck cornmath   husk_abs(), ear_max(), stalk_pow(), silk_mod() ...
 *     shuck kernels    kernel_len(), kernel_upper(), kernel_slice() ...
 *     shuck almanac    almanac_now(), almanac_ticks(), almanac_nap() ...
 *     shuck granary    granary_read(), granary_write() ...
 *     shuck pantry     pantry_open(), pantry_exec(), pantry_query() ...
 *     shuck barn       barn_open(), barn_label(), barn_button() ... (window)
 *     shuck tassel     tassel_tcl(), tassel_tk()
 *     shuck silo       porch + cornmath + kernels + almanac + granary
 * The directory is $COB_STDLIB, else <this program's directory>/lib.
 *
 * NOTE ON popcorn_comp: it has its own copy of this AST and its own C code
 * generator. Recipes, serve and every stdlib module work there too (it
 * links the same shared libraries). The cache tags line up with the enums
 * below; keep the two files in sync.
 *
 * grammar:  expr := term (('+'|'-') term)*
 *           term := primary (('*'|'/') primary)*
 *           primary := NUMBER | STRING | IDENT | 'harvest' '(' expr ')'
 *                      | IDENT '(' [expr (',' expr)*] ')'   (a call)
 * cond grammar:  expr (('=='|'!='|'<'|'>'|'<='|'>=') expr)?
 *
 * SAFETY: a `while` loop is capped at COB_MAX_LOOP_ITERATIONS, and
 * recipe calls at COB_MAX_CALL_DEPTH, so a buggy program cannot hang or
 * crash the interpreter.
 *
 * License: PolyForm Noncommercial License 1.0.0 - see LICENSE.md
 * ===================================================================== */

/* clock_gettime()/CLOCK_MONOTONIC and usleep() (used by the X11
 * _cobwindow backend's window_wait()) are POSIX.1-2008, not plain
 * C99 -- glibc/musl hide them under -std=c99 unless a feature-test
 * macro asks for them. Must come before any system header is
 * included (including transitively via common.h below). */
#define _POSIX_C_SOURCE 200809L

#include "common.h"
#ifdef COB_ENTERPRISE
#include "drm.h"   /* enterprise build only (make ENTERPRISE=1) */
#endif
#include "file_io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <stdarg.h>

/* REPL: is stdin a terminal? (prompts and the banner only show on a real
 * terminal, so `cob_interp --repl < script` is quiet and scriptable.) */
#if !defined(COB_WASM)
#  if defined(_WIN32)
#    include <io.h>
#    define COB_STDIN_IS_TTY() _isatty(_fileno(stdin))
#  else
#    include <unistd.h>
#    define COB_STDIN_IS_TTY() isatty(0)
#  endif
#endif

#include "cob.h"   /* CEP-2: CobValue / CobFFIFunc -- the native-extension ABI */

/* Dynamic loading for `extern` (CEP-2) -- and so for the whole standard
 * library: dlopen()/dlsym() on POSIX, LoadLibraryA()/GetProcAddress() on
 * Windows (declared by hand so <windows.h> is not needed). Not compiled
 * for COB_WASM, which has no dlopen(): the browser build binds the stdlib
 * modules that can run there (COB_STDLIB_STATIC) and stubs the rest. */
#if defined(COB_WASM)
/* nothing: the sandbox cannot load native code */
#elif defined(_WIN32)
#   if !defined(_WINDOWS_) && !defined(_INC_WINDOWS)
__declspec(dllimport) void *__stdcall LoadLibraryA(const char *);
__declspec(dllimport) void *__stdcall GetProcAddress(void *, const char *);
__declspec(dllimport) int   __stdcall FreeLibrary(void *);
#   endif
#else
#   include <dlfcn.h>
#endif

#define COB_MAX_LOOP_ITERATIONS 10000000UL
#define COB_MAX_EXTERN_ARGS 255
#define COB_MAX_SHUCK_DEPTH 16

/* ---------------------------------------------------------------------
 * VALUES -- a Cob value is either an integer or a string. Strings were
 * added after v0.0.3; anywhere a `long` used to flow through the
 * evaluator, a Value flows through it instead. A VAL_STR Value owns
 * its `s` pointer; value_free() must be called on any Value nobody
 * else is taking ownership of, to avoid leaking it.
 * ------------------------------------------------------------------- */
typedef enum { VAL_INT, VAL_STR } ValueKind;
typedef struct { ValueKind kind; long i; char *s; } Value;

static Value value_int(long n) { Value v; v.kind = VAL_INT; v.i = n; v.s = NULL; return v; }
static Value value_str_take(char *owned) { Value v; v.kind = VAL_STR; v.i = 0; v.s = owned; return v; }
static void value_free(Value *v) { if (v->kind == VAL_STR) { free(v->s); v->s = NULL; } }

/* ---------------------------------------------------------------------
 * AST: EXPRESSIONS
 * ------------------------------------------------------------------- */
typedef enum {
    EXPR_NUM, EXPR_STR, EXPR_VAR, EXPR_BINOP, EXPR_HARVEST,
    /* C API / FFI (CEP-2): a call to a name bound by an `extern`
     * statement (this is how every standard-library function is called).
     * The name lives in `var`, the arguments in args[0..argc-1]. */
    EXPR_EXTERN_CALL,
    /* Functions (v0.0.9, cache format COBSTRW10): a call to a `recipe`.
     * Same shape as EXPR_EXTERN_CALL. popcorn_comp.c mirrors both tags
     * (5 and 6) -- keep the two in sync. v0.0.9 removed the built-in
     * SQL/Tcl/Tk/window/input kinds, so the tags were renumbered with the
     * cache format bump. */
    EXPR_CALL
} ExprKind;

typedef struct Expr {
    ExprKind kind;
    long num;             /* EXPR_NUM */
    char *str_lit; size_t str_lit_len; /* EXPR_STR, owned */
    char *var;            /* EXPR_VAR, owned */
    char op;              /* EXPR_BINOP: '+' '-' '*' '/' */
    struct Expr *left;    /* EXPR_BINOP left; EXPR_HARVEST byte-count child;
                            * one-arg builtin's sole arg; two/three-arg
                            * builtin's first (handle) arg */
    struct Expr *right;   /* EXPR_BINOP right; two/three-arg builtin's
                            * second (SQL text / widget label) arg */
    struct Expr *arg3;    /* three-arg builtin's third arg (currently only
                            * window_slider's <max>) -- NULL otherwise */
    struct Expr **args;   /* EXPR_EXTERN_CALL: owned array of owned Exprs */
    int argc;             /* EXPR_EXTERN_CALL / EXPR_CALL: number of entries in args */
    int line;             /* EXPR_CALL: source line (diagnostics only; not cached) */
} Expr;
static int g_parse_line = 0;    /* line of the statement being parsed */
static int g_recipe_depth = 0;  /* >0 while parsing a recipe body (where `serve` is legal) */

static Expr *expr_new_num(long n) {
    Expr *e = (Expr *)calloc(1, sizeof(Expr));
    if (!e) return NULL;
    e->kind = EXPR_NUM; e->num = n;
    return e;
}
static Expr *expr_new_str(char *owned_text, size_t len) {
    Expr *e = (Expr *)calloc(1, sizeof(Expr));
    if (!e) return NULL;
    e->kind = EXPR_STR; e->str_lit = owned_text; e->str_lit_len = len;
    return e;
}
static Expr *expr_new_var(char *owned_name) {
    Expr *e = (Expr *)calloc(1, sizeof(Expr));
    if (!e) return NULL;
    e->kind = EXPR_VAR; e->var = owned_name;
    return e;
}
static Expr *expr_new_binop(char op, Expr *l, Expr *r) {
    Expr *e = (Expr *)calloc(1, sizeof(Expr));
    if (!e) return NULL;
    e->kind = EXPR_BINOP; e->op = op; e->left = l; e->right = r;
    return e;
}
static Expr *expr_new_harvest(Expr *bytes_expr) {
    Expr *e = (Expr *)calloc(1, sizeof(Expr));
    if (!e) return NULL;
    e->kind = EXPR_HARVEST; e->left = bytes_expr;
    return e;
}
/* CEP-2: takes ownership of `owned_name` and of `args` (and each Expr in it). */
static Expr *expr_new_named_call(ExprKind kind, char *owned_name, Expr **args, int argc) {
    Expr *e = (Expr *)calloc(1, sizeof(Expr));
    if (!e) return NULL;
    e->kind = kind; e->var = owned_name; e->args = args; e->argc = argc;
    e->line = g_parse_line;
    return e;
}
static void expr_free(Expr *e) {
    if (!e) return;
    if (e->kind == EXPR_EXTERN_CALL || e->kind == EXPR_CALL) {
        int i;
        free(e->var);
        for (i = 0; i < e->argc; i++) expr_free(e->args[i]);
        free(e->args);
    }
    if (e->kind == EXPR_STR) free(e->str_lit);
    if (e->kind == EXPR_VAR) free(e->var);
    if (e->kind == EXPR_BINOP) { expr_free(e->left); expr_free(e->right); }
    if (e->kind == EXPR_HARVEST) { expr_free(e->left); }
    free(e);
}
static int expr_uses_harvest(const Expr *e) {
    if (!e) return 0;
    if (e->kind == EXPR_HARVEST) return 1;
    if (e->kind == EXPR_BINOP) return expr_uses_harvest(e->left) || expr_uses_harvest(e->right);
    if (e->kind == EXPR_EXTERN_CALL || e->kind == EXPR_CALL) {
        int i;
        for (i = 0; i < e->argc; i++) if (expr_uses_harvest(e->args[i])) return 1;
        return 0;
    }
    return 0;
}

/* ---------------------------------------------------------------------
 * AST: CONDITIONS
 * ------------------------------------------------------------------- */
typedef enum { COND_TRUTHY, COND_EQ, COND_NE, COND_LT, COND_GT, COND_LE, COND_GE } CondOp;

typedef struct {
    Expr *left;
    CondOp op;
    Expr *right; /* NULL when op == COND_TRUTHY */
} Cond;

static void cond_free(Cond *c) {
    if (!c) return;
    expr_free(c->left);
    expr_free(c->right);
    free(c);
}

/* ---------------------------------------------------------------------
 * AST: STATEMENTS / PROGRAM
 * ------------------------------------------------------------------- */
/* STMT_EXTERN (CEP-2) is appended last so earlier kinds keep their numeric
 * values -- see the matching note on EXPR_EXTERN_CALL. */
/* STMT_IF is appended after STMT_EXTERN for the same reason. `elif` is
 * not its own kind: the parser desugars `elif` into an STMT_IF nested as
 * the sole statement of the previous if's else_body. */
/* Functions (v0.0.9) append STMT_RECIPE / STMT_SERVE / STMT_CALL the same way. */
typedef enum { STMT_POP, STMT_SET, STMT_WHILE, STMT_TRASH, STMT_EXTERN, STMT_IF,
               STMT_RECIPE, STMT_SERVE, STMT_CALL } StmtKind;

typedef struct Program {
    struct Stmt *items;
    size_t count, capacity;
} Program;

typedef struct Stmt {
    StmtKind kind;
    Expr *pop_expr;                            /* STMT_POP */
    char *set_name; Expr *set_expr;           /* STMT_SET */
    Cond *while_cond; Program while_body;     /* STMT_WHILE */
    char *trash_name;                         /* STMT_TRASH */
    char *ext_name; char *ext_lib;            /* STMT_EXTERN: C symbol name, library path */
    Cond *if_cond; Program if_body; Program else_body;  /* STMT_IF (else_body may be empty) */
    char *recipe_name; char **recipe_params; int recipe_nparams; Program recipe_body; int recipe_line; /* STMT_RECIPE */
    Expr *serve_expr;                           /* STMT_SERVE (a bare `serve` holds the number 0) */
    Expr *call_expr;                          /* STMT_CALL: a call used as a statement, result dropped */
} Stmt;

static void program_init(Program *p) { p->items = NULL; p->count = 0; p->capacity = 0; }

static int program_uses_harvest_or_trash(const Program *p) {
    size_t i;
    for (i = 0; i < p->count; i++) {
        const Stmt *s = &p->items[i];
        switch (s->kind) {
            case STMT_SET: if (expr_uses_harvest(s->set_expr)) return 1; break;
            case STMT_TRASH: return 1;
            case STMT_WHILE:
                if (program_uses_harvest_or_trash(&s->while_body)) return 1;
                break;
            case STMT_IF:
                if (program_uses_harvest_or_trash(&s->if_body)) return 1;
                if (program_uses_harvest_or_trash(&s->else_body)) return 1;
                break;
            case STMT_RECIPE:
                if (program_uses_harvest_or_trash(&s->recipe_body)) return 1;
                break;
            case STMT_POP: if (expr_uses_harvest(s->pop_expr)) return 1; break;
            case STMT_SERVE: if (expr_uses_harvest(s->serve_expr)) return 1; break;
            case STMT_CALL: if (expr_uses_harvest(s->call_expr)) return 1; break;
            default: break;
        }
    }
    return 0;
}

static void stmt_free_contents(Stmt *s) {
    switch (s->kind) {
        case STMT_POP: expr_free(s->pop_expr); break;
        case STMT_SET: free(s->set_name); expr_free(s->set_expr); break;
        case STMT_TRASH: free(s->trash_name); break;
        case STMT_EXTERN: free(s->ext_name); free(s->ext_lib); break;
        case STMT_WHILE: {
            cond_free(s->while_cond);
            size_t i;
            for (i = 0; i < s->while_body.count; i++) stmt_free_contents(&s->while_body.items[i]);
            free(s->while_body.items);
            break;
        }
        case STMT_RECIPE: {
            size_t i;
            int k;
            free(s->recipe_name);
            for (k = 0; k < s->recipe_nparams; k++) free(s->recipe_params[k]);
            free(s->recipe_params);
            for (i = 0; i < s->recipe_body.count; i++) stmt_free_contents(&s->recipe_body.items[i]);
            free(s->recipe_body.items);
            break;
        }
        case STMT_SERVE: expr_free(s->serve_expr); break;
        case STMT_CALL: expr_free(s->call_expr); break;
        case STMT_IF: {
            size_t i;
            cond_free(s->if_cond);
            for (i = 0; i < s->if_body.count; i++) stmt_free_contents(&s->if_body.items[i]);
            free(s->if_body.items);
            for (i = 0; i < s->else_body.count; i++) stmt_free_contents(&s->else_body.items[i]);
            free(s->else_body.items);
            break;
        }
    }
}

static void program_free(Program *p) {
    size_t i;
    for (i = 0; i < p->count; i++) stmt_free_contents(&p->items[i]);
    free(p->items);
    p->items = NULL; p->count = 0; p->capacity = 0;
}

static int program_push(Program *p, Stmt s) {
    if (p->count == p->capacity) {
        size_t new_cap = (p->capacity == 0) ? 8 : p->capacity * 2;
        Stmt *grown = (Stmt *)realloc(p->items, new_cap * sizeof(Stmt));
        if (!grown) return -1;
        p->items = grown;
        p->capacity = new_cap;
    }
    p->items[p->count++] = s;
    return 0;
}

/* Moves every statement from `src` into `dst` (ownership transfers --
 * `src` is left empty and safe to program_free() afterward, since it no
 * longer owns anything). Used by `shuck` to splice a loaded library's
 * top-level statements into the importing block. Returns 0/-1. */
static int program_append_move(Program *dst, Program *src) {
    size_t i;
    for (i = 0; i < src->count; i++) {
        if (program_push(dst, src->items[i]) != 0) {
            /* Roll the remaining un-moved statements back into src so
             * nothing leaks and the caller's cleanup still works. */
            size_t remaining = src->count - i;
            memmove(src->items, src->items + i, remaining * sizeof(Stmt));
            src->count = remaining;
            return -1;
        }
    }
    src->count = 0; /* ownership fully transferred */
    return 0;
}

/* ---------------------------------------------------------------------
 * VARIABLES (linear-scan symbol table -- Cob programs are small)
 * ------------------------------------------------------------------- */
typedef struct { char *name; Value value; } Var;
typedef struct { Var *items; size_t count, capacity; } VarTable;

static char *cob_strdup(const char *s) {
    size_t len = strlen(s) + 1;
    char *copy = (char *)malloc(len);
    if (copy) memcpy(copy, s, len);
    return copy;
}

/* Returns a fresh, independently-owned copy -- callers of vars_get are
 * always free to value_free() what they get back without touching the
 * table's own copy. */
static Value value_copy(const Value *v) {
    if (v->kind == VAL_STR) return value_str_take(cob_strdup(v->s ? v->s : ""));
    return value_int(v->i);
}

static void vars_init(VarTable *t) { t->items = NULL; t->count = 0; t->capacity = 0; }
static void vars_free(VarTable *t) {
    size_t i;
    for (i = 0; i < t->count; i++) { free(t->items[i].name); value_free(&t->items[i].value); }
    free(t->items);
    t->items = NULL; t->count = 0; t->capacity = 0;
}
static int vars_get(VarTable *t, const char *name, Value *out) {
    size_t i;
    for (i = 0; i < t->count; i++)
        if (strcmp(t->items[i].name, name) == 0) { *out = value_copy(&t->items[i].value); return 1; }
    return 0;
}
/* Takes ownership of `value` on success (stored directly, no copy) --
 * on failure (OOM growing the table), `value` is NOT consumed and the
 * caller still owns it and must free it. */
static int vars_set(VarTable *t, const char *name, Value value) {
    size_t i;
    for (i = 0; i < t->count; i++)
        if (strcmp(t->items[i].name, name) == 0) {
            value_free(&t->items[i].value);
            t->items[i].value = value;
            return 0;
        }
    if (t->count == t->capacity) {
        size_t new_cap = (t->capacity == 0) ? 8 : t->capacity * 2;
        Var *grown = (Var *)realloc(t->items, new_cap * sizeof(Var));
        if (!grown) return -1;
        t->items = grown;
        t->capacity = new_cap;
    }
    t->items[t->count].name = cob_strdup(name);
    if (!t->items[t->count].name) return -1;
    t->items[t->count].value = value;
    t->count++;
    return 0;
}

/* ---------------------------------------------------------------------
 * MEMORY HANDLES (backing harvest()/trash()). A "handle" is an opaque
 * 1-based integer index Cob code stores in an ordinary variable --
 * never a raw pointer, so a malformed Cob program can't corrupt memory
 * via arbitrary pointer arithmetic, only mismanage its own handles.
 * ------------------------------------------------------------------- */
typedef struct { void *ptr; size_t size; int freed; } Handle;
typedef struct { Handle *items; size_t count, capacity; } HandleTable;

static void handles_init(HandleTable *h) { h->items = NULL; h->count = 0; h->capacity = 0; }
static void handles_free_all(HandleTable *h) {
    size_t i;
    for (i = 0; i < h->count; i++) if (!h->items[i].freed) free(h->items[i].ptr);
    free(h->items);
    h->items = NULL; h->count = 0; h->capacity = 0;
}
/* Returns the new handle id (>= 1) on success, or 0 if the allocation
 * itself failed (mirrors malloc's NULL-on-failure convention). */
static long handle_alloc(HandleTable *h, size_t bytes) {
    void *ptr = malloc(bytes > 0 ? bytes : 1);
    if (!ptr) return 0;
    if (h->count == h->capacity) {
        size_t new_cap = (h->capacity == 0) ? 8 : h->capacity * 2;
        Handle *grown = (Handle *)realloc(h->items, new_cap * sizeof(Handle));
        if (!grown) { free(ptr); return 0; }
        h->items = grown;
        h->capacity = new_cap;
    }
    h->items[h->count].ptr = ptr;
    h->items[h->count].size = bytes;
    h->items[h->count].freed = 0;
    h->count++;
    return (long)h->count; /* 1-based id */
}
/* Returns 0 on success, -1 if the id is out of range or already freed
 * (reported by the caller as a warning, not treated as fatal). */
static int handle_free(HandleTable *h, long id) {
    size_t idx;
    if (id < 1 || (size_t)id > h->count) return -1;
    idx = (size_t)id - 1;
    if (h->items[idx].freed) return -1;
    free(h->items[idx].ptr);
    h->items[idx].ptr = NULL;
    h->items[idx].freed = 1;
    return 0;
}

/* Bundles everything a statement/expression might need to execute, so
 * we thread one pointer through eval_expr/eval_cond/execute instead of
 * growing the parameter list every time a new keyword needs state. */
typedef struct {
    VarTable *vars;
    HandleTable *handles;
    int no_gc; /* 1 if --no-gc was passed on the command line */
    /* Functions (v0.0.9) */
    VarTable *locals;  /* NULL at top level; the running recipe's variables otherwise */
    int call_depth;    /* how many recipes are currently running */
    Value ret;         /* value of the `serve` that is unwinding (VAL_INT 0 otherwise) */
    int fell_off;      /* set by the last recipe call: 1 if it ended without `serve` */
} EvalCtx;

/* ---------------------------------------------------------------------
 * SMALL PARSE HELPERS
 * ------------------------------------------------------------------- */
static const char *skip_blank(const char *p) {
    while (*p == ' ' || *p == '\t') p++;
    return p;
}
static int is_ident_start(char c) { return isalpha((unsigned char)c) || c == '_'; }
static int is_ident_char(char c)  { return isalnum((unsigned char)c) || c == '_'; }

static const char *parse_string_literal(const char *src, char **out_text, size_t *out_len) {
    const char *p; char *buf; size_t buf_cap, buf_len;
    if (*src != '"') return NULL;
    p = src + 1;
    buf_cap = 32; buf_len = 0;
    buf = (char *)malloc(buf_cap);
    if (!buf) return NULL;
    while (*p != '\0' && *p != '"') {
        char c = *p;
        if (c == '\\' && *(p + 1) != '\0') {
            char esc = *(p + 1);
            switch (esc) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case '"': c = '"';  break;
                case '\\': c = '\\'; break;
                default: c = esc; break;
            }
            p += 2;
        } else {
            p += 1;
        }
        if (buf_len + 1 >= buf_cap) {
            size_t new_cap = buf_cap * 2;
            char *grown = (char *)realloc(buf, new_cap);
            if (!grown) { free(buf); return NULL; }
            buf = grown; buf_cap = new_cap;
        }
        buf[buf_len++] = c;
    }
    if (*p != '"') { free(buf); return NULL; }
    buf[buf_len] = '\0';
    *out_text = buf; *out_len = buf_len;
    return p + 1;
}

/* Parses a bare identifier at *p (caller has already skipped blanks).
 * Returns the position after it, or NULL if *p isn't an identifier
 * start. On success, *out_name is a newly malloc'd, NUL-terminated
 * copy the caller owns. */
static const char *parse_identifier(const char *p, char **out_name) {
    const char *start;
    size_t len;
    char *name;
    if (!is_ident_start(*p)) return NULL;
    start = p;
    while (is_ident_char(*p)) p++;
    len = (size_t)(p - start);
    name = (char *)malloc(len + 1);
    if (!name) return NULL;
    memcpy(name, start, len); name[len] = '\0';
    *out_name = name;
    return p;
}

/* ---------------------------------------------------------------------
 * EXPRESSION / CONDITION PARSER
 * ------------------------------------------------------------------- */
static const char *parse_expr(const char *p, Expr **out); /* fwd decl */

/* ---------------------------------------------------------------------
 * C API / FFI (CEP-2) -- PARSE-TIME SIDE
 *
 * An `extern f(x) from "lib"` line registers `f` here as it is parsed
 * (parse_block walks statements in source order), so a *later*
 * `f(...)` parses as a call instead of a variable followed by a stray
 * '('. Declare before use, same as everything else in Cob. The actual
 * dlopen()/dlsym() happens at execution time -- see cob_extern_bind().
 * ------------------------------------------------------------------- */
static char **g_extern_decl = NULL;
static size_t g_extern_decl_count = 0, g_extern_decl_cap = 0;

static int cob_extern_is_declared(const char *name) {
    size_t i;
    for (i = 0; i < g_extern_decl_count; i++)
        if (strcmp(g_extern_decl[i], name) == 0) return 1;
    return 0;
}
static int cob_extern_declare(const char *name) {
    if (cob_extern_is_declared(name)) return 0;
    if (g_extern_decl_count == g_extern_decl_cap) {
        size_t new_cap = g_extern_decl_cap ? g_extern_decl_cap * 2 : 8;
        char **grown = (char **)realloc(g_extern_decl, new_cap * sizeof(char *));
        if (!grown) return -1;
        g_extern_decl = grown; g_extern_decl_cap = new_cap;
    }
    g_extern_decl[g_extern_decl_count] = cob_strdup(name);
    if (!g_extern_decl[g_extern_decl_count]) return -1;
    g_extern_decl_count++;
    return 0;
}
static void cob_extern_decl_free(void) {
    size_t i;
    for (i = 0; i < g_extern_decl_count; i++) free(g_extern_decl[i]);
    free(g_extern_decl);
    g_extern_decl = NULL; g_extern_decl_count = g_extern_decl_cap = 0;
}

/* An extern may not shadow a statement keyword or a built-in call -- the
 * built-in would silently win at every call site, which is a worse
 * failure mode than refusing the declaration outright. */
static int cob_is_reserved_word(const char *name) {
    static const char *const reserved[] = {
        COB_KW_WHILE, COB_KW_SET, COB_KW_POP, COB_KW_SHUCK, COB_KW_HARVEST,
        COB_KW_TRASH, COB_KW_EXTERN, COB_KW_IF, COB_KW_ELIF, COB_KW_ELSE,
        COB_KW_RECIPE, COB_KW_SERVE
    };
    size_t i;
    for (i = 0; i < sizeof(reserved) / sizeof(reserved[0]); i++)
        if (strcmp(reserved[i], name) == 0) return 1;
    return 0;
}

/* `p` points just past the '(' of a call: an already-declared extern
 * (EXPR_EXTERN_CALL) or a call to a `recipe` (EXPR_CALL). Takes ownership of
 * `name`. Returns the position after ')' or NULL. */
static const char *parse_named_call(ExprKind kind, char *name, const char *p, Expr **out) {
    Expr **args = NULL; int argc = 0, cap = 0, i;
    p = skip_blank(p);
    if (*p != ')') {
        for (;;) {
            Expr *arg;
            p = parse_expr(p, &arg);
            if (!p) goto fail;
            if (argc >= COB_MAX_EXTERN_ARGS) { expr_free(arg); goto fail; }
            if (argc == cap) {
                int new_cap = cap ? cap * 2 : 4;
                Expr **grown = (Expr **)realloc(args, (size_t)new_cap * sizeof(Expr *));
                if (!grown) { expr_free(arg); goto fail; }
                args = grown; cap = new_cap;
            }
            args[argc++] = arg;
            p = skip_blank(p);
            if (*p == ',') { p = skip_blank(p + 1); continue; }
            break;
        }
        if (*p != ')') goto fail;
    }
    *out = expr_new_named_call(kind, name, args, argc);
    if (!*out) goto fail;
    return p + 1;
fail:
    for (i = 0; i < argc; i++) expr_free(args[i]);
    free(args); free(name);
    return NULL;
}

static const char *parse_primary(const char *p, Expr **out) {
    p = skip_blank(p);

    /* string literal -- reuses the same escape-decoding logic pop()
     * has always used for its argument. Making this a primary means
     * pop() (which now parses a general expr, not just a literal),
     * set, and comparisons can all use string literals the same way. */
    if (*p == '"') {
        char *text; size_t len;
        const char *after = parse_string_literal(p, &text, &len);
        if (!after) return NULL;
        *out = expr_new_str(text, len);
        if (!*out) { free(text); return NULL; }
        return after;
    }

    /* harvest(<bytes>) -- only special-cased when directly followed by
     * '(' after optional blanks; "harvest" with no parens would just be
     * an ordinary (if oddly named) variable reference. */
    {
        size_t kw_len = strlen(COB_KW_HARVEST);
        if (strncmp(p, COB_KW_HARVEST, kw_len) == 0) {
            const char *after_kw = skip_blank(p + kw_len);
            if (*after_kw == '(') {
                Expr *bytes_expr;
                const char *after_open = skip_blank(after_kw + 1);
                const char *after_expr = parse_expr(after_open, &bytes_expr);
                if (!after_expr) return NULL;
                after_expr = skip_blank(after_expr);
                if (*after_expr != ')') { expr_free(bytes_expr); return NULL; }
                *out = expr_new_harvest(bytes_expr);
                if (!*out) { expr_free(bytes_expr); return NULL; }
                return after_expr + 1;
            }
        }
    }

    if (isdigit((unsigned char)*p) || (*p == '-' && isdigit((unsigned char)*(p + 1)))) {
        char *end;
        long v = strtol(p, &end, 10);
        *out = expr_new_num(v);
        return *out ? end : NULL;
    }
    if (is_ident_start(*p)) {
        char *name;
        const char *after = parse_identifier(p, &name);
        if (!after) return NULL;
        /* CEP-2: a declared extern directly followed by '(' is a native call */
        if (cob_extern_is_declared(name)) {
            const char *paren = skip_blank(after);
            if (*paren == '(') return parse_named_call(EXPR_EXTERN_CALL, name, paren + 1, out);
        }
        /* v0.0.9: any other name directly followed by '(' is a call to a
         * `recipe`. (A bare name followed by '(' was never valid Cob, so this
         * takes nothing away.) Whether the recipe exists is checked after the
         * whole program is parsed, so a call may appear above its recipe. */
        {
            const char *paren = skip_blank(after);
            if (*paren == '(') return parse_named_call(EXPR_CALL, name, paren + 1, out);
        }
        *out = expr_new_var(name);
        if (!*out) { free(name); return NULL; }
        return after;
    }
    return NULL;
}

static const char *parse_term(const char *p, Expr **out) {
    Expr *left;
    p = parse_primary(p, &left);
    if (!p) return NULL;
    for (;;) {
        const char *q = skip_blank(p);
        if (*q == '*' || *q == '/') {
            char op = *q; q++;
            Expr *right;
            const char *after = parse_primary(q, &right);
            if (!after) { expr_free(left); return NULL; }
            left = expr_new_binop(op, left, right);
            if (!left) return NULL;
            p = after;
        } else break;
    }
    *out = left;
    return p;
}

static const char *parse_expr(const char *p, Expr **out) {
    Expr *left;
    p = parse_term(p, &left);
    if (!p) return NULL;
    for (;;) {
        const char *q = skip_blank(p);
        if (*q == '+' || *q == '-') {
            char op = *q; q++;
            Expr *right;
            const char *after = parse_term(q, &right);
            if (!after) { expr_free(left); return NULL; }
            left = expr_new_binop(op, left, right);
            if (!left) return NULL;
            p = after;
        } else break;
    }
    *out = left;
    return p;
}

static int parse_comparator(const char **p, CondOp *out) {
    const char *q = skip_blank(*p);
    if (q[0] == '=' && q[1] == '=') { *out = COND_EQ; *p = q + 2; return 0; }
    if (q[0] == '!' && q[1] == '=') { *out = COND_NE; *p = q + 2; return 0; }
    if (q[0] == '<' && q[1] == '=') { *out = COND_LE; *p = q + 2; return 0; }
    if (q[0] == '>' && q[1] == '=') { *out = COND_GE; *p = q + 2; return 0; }
    if (q[0] == '<') { *out = COND_LT; *p = q + 1; return 0; }
    if (q[0] == '>') { *out = COND_GT; *p = q + 1; return 0; }
    return -1;
}

static const char *parse_condition(const char *p, Cond **out) {
    Expr *left; CondOp op; Expr *right = NULL;
    p = parse_expr(p, &left);
    if (!p) return NULL;
    if (parse_comparator(&p, &op) == 0) {
        p = parse_expr(p, &right);
        if (!p) { expr_free(left); return NULL; }
    } else {
        op = COND_TRUTHY;
    }
    Cond *c = (Cond *)calloc(1, sizeof(Cond));
    if (!c) { expr_free(left); expr_free(right); return NULL; }
    c->left = left; c->op = op; c->right = right;
    *out = c;
    return p;
}

/* ---------------------------------------------------------------------
 * FUNCTIONS (v0.0.9) -- registry, checking and calls
 *
 * `recipe` statements are HOISTED: every top-level recipe is registered before
 * the program starts running, so a recipe may be called from above where it
 * is written (and recipes may call each other freely). popcorn_comp does the
 * same, because C functions are hoisted too.
 *
 * Scoping, kept deliberately simple and identical in both tools:
 *   - a recipe's PARAMETERS and every name it assigns with `set` are LOCAL
 *     to it, for the whole body (assigning to a name makes it local, as in
 *     Python). They start at 0 and vanish when the recipe returns.
 *   - every other name is read from the global (top-level) variables.
 *   - a recipe cannot assign a global; it hands results back with `serve`.
 * ------------------------------------------------------------------- */
typedef struct { const Stmt *def; char **locals; int nlocals; } FuncEntry;
static FuncEntry *g_funcs = NULL;
static size_t g_func_count = 0, g_func_cap = 0;

static FuncEntry *func_find(const char *name) {
    size_t i;
    for (i = 0; i < g_func_count; i++)
        if (strcmp(g_funcs[i].def->recipe_name, name) == 0) return &g_funcs[i];
    return NULL;
}
static int cob_func_exists(const char *name) { return func_find(name) != NULL; }

static void funcs_free(void) {
    size_t i;
    int k;
    for (i = 0; i < g_func_count; i++) {
        for (k = 0; k < g_funcs[i].nlocals; k++) free(g_funcs[i].locals[k]);
        free(g_funcs[i].locals);
    }
    free(g_funcs);
    g_funcs = NULL; g_func_count = g_func_cap = 0;
}

static int names_add(char ***arr, int *n, int *cap, const char *name) {
    int i;
    for (i = 0; i < *n; i++) if (strcmp((*arr)[i], name) == 0) return 0;
    if (*n == *cap) {
        int new_cap = *cap ? *cap * 2 : 8;
        char **grown = (char **)realloc(*arr, (size_t)new_cap * sizeof(char *));
        if (!grown) return -1;
        *arr = grown; *cap = new_cap;
    }
    (*arr)[*n] = cob_strdup(name);
    if (!(*arr)[*n]) return -1;
    (*n)++;
    return 0;
}
static int recipe_collect_sets(const Program *p, char ***arr, int *n, int *cap) {
    size_t i;
    for (i = 0; i < p->count; i++) {
        const Stmt *s = &p->items[i];
        if (s->kind == STMT_SET && names_add(arr, n, cap, s->set_name) != 0) return -1;
        if (s->kind == STMT_WHILE && recipe_collect_sets(&s->while_body, arr, n, cap) != 0) return -1;
        if (s->kind == STMT_IF) {
            if (recipe_collect_sets(&s->if_body, arr, n, cap) != 0) return -1;
            if (recipe_collect_sets(&s->else_body, arr, n, cap) != 0) return -1;
        }
    }
    return 0;
}

/* Registers (or, with an existing name, replaces) one recipe. */
static int func_register(const Stmt *d) {
    char **locals = NULL;
    int n = 0, cap = 0, k;
    FuncEntry *f;
    for (k = 0; k < d->recipe_nparams; k++)
        if (names_add(&locals, &n, &cap, d->recipe_params[k]) != 0) goto oom;
    if (recipe_collect_sets(&d->recipe_body, &locals, &n, &cap) != 0) goto oom;
    f = func_find(d->recipe_name);
    if (f) {
        for (k = 0; k < f->nlocals; k++) free(f->locals[k]);
        free(f->locals);
    } else {
        if (g_func_count == g_func_cap) {
            size_t new_cap = g_func_cap ? g_func_cap * 2 : 8;
            FuncEntry *grown = (FuncEntry *)realloc(g_funcs, new_cap * sizeof(FuncEntry));
            if (!grown) goto oom;
            g_funcs = grown; g_func_cap = new_cap;
        }
        f = &g_funcs[g_func_count++];
    }
    f->def = d; f->locals = locals; f->nlocals = n;
    return 0;
oom:
    for (k = 0; k < n; k++) free(locals[k]);
    free(locals);
    return -1;
}

static void cob_report(int line, const char *what, const char *fmt, ...) {
    va_list ap;
    if (line > 0) fprintf(stderr, "[cob_interp] %s at line %d: ", what, line);
    else          fprintf(stderr, "[cob_interp] %s: ", what);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* Checks the top-level recipes of `top` BEFORE anything is registered, so a
 * failure leaves the registry untouched. allow_redefine is the REPL's
 * "typing a recipe again replaces it" rule. */
static int cob_recipes_validate(const Program *top, int allow_redefine) {
    size_t i, j;
    for (i = 0; i < top->count; i++) {
        const Stmt *s = &top->items[i];
        if (s->kind != STMT_RECIPE) continue;
        if (cob_extern_is_declared(s->recipe_name)) {
            cob_report(s->recipe_line, "error", "recipe '%s' clashes with an extern of the same name", s->recipe_name);
            return -1;
        }
        if (!allow_redefine && func_find(s->recipe_name)) {
            cob_report(s->recipe_line, "error", "recipe '%s' is already defined", s->recipe_name);
            return -1;
        }
        for (j = 0; j < i; j++) {
            const Stmt *o = &top->items[j];
            if (o->kind == STMT_RECIPE && strcmp(o->recipe_name, s->recipe_name) == 0) {
                cob_report(s->recipe_line, "error", "recipe '%s' is already defined (line %d)", s->recipe_name, o->recipe_line);
                return -1;
            }
        }
    }
    return 0;
}
static int cob_recipes_register(const Program *top) {
    size_t i;
    for (i = 0; i < top->count; i++) {
        if (top->items[i].kind != STMT_RECIPE) continue;
        if (func_register(&top->items[i]) != 0) {
            fprintf(stderr, "[cob_interp] out of memory registering recipe '%s'\n", top->items[i].recipe_name);
            return -1;
        }
    }
    return 0;
}
static int program_has_recipe(const Program *p) {
    size_t i;
    for (i = 0; i < p->count; i++) if (p->items[i].kind == STMT_RECIPE) return 1;
    return 0;
}

/* Call checking: every EXPR_CALL must name a recipe and pass the right number
 * of arguments. `g_check_top` is the program being checked: its own recipes
 * count even though they are not registered yet. `lenient` (REPL) lets a
 * call to a recipe that does not exist yet through -- it may be typed later. */
/* Which standard-library module a (corn-themed) function name belongs to,
 * so a missing `shuck` gets a helpful message instead of a bare "undefined". */
static const char *cob_stdlib_module_for(const char *name) {
    static const struct { const char *prefix; const char *module; } table[] = {
        { "porch_", "porch" }, { "kernel_", "kernels" }, { "almanac_", "almanac" },
        { "granary_", "granary" }, { "pantry_", "pantry" }, { "barn_", "barn" },
        { "tassel_", "tassel" }, { "husk_", "cornmath" }, { "ear_", "cornmath" },
        { "stalk_", "cornmath" }, { "silk_", "cornmath" }, { "scatter_", "cornmath" }
    };
    size_t i;
    for (i = 0; i < sizeof(table) / sizeof(table[0]); i++)
        if (strncmp(name, table[i].prefix, strlen(table[i].prefix)) == 0) return table[i].module;
    return NULL;
}
/* Names that were built-in keywords before v0.0.9 and are standard-library
 * functions now -- so an upgrading program gets told exactly what to write. */
static const char *cob_moved_builtin(const char *name, const char **module) {
    static const struct { const char *old; const char *now; const char *module; } moved[] = {
        { "input", "porch_ask", "porch" }, { "input_int", "porch_ask_int", "porch" },
        { "sql_open", "pantry_open", "pantry" }, { "sql_exec", "pantry_exec", "pantry" },
        { "sql_query", "pantry_query", "pantry" }, { "sql_close", "pantry_close", "pantry" },
        { "window_open", "barn_open", "barn" }, { "window_label", "barn_label", "barn" },
        { "window_wait", "barn_wait", "barn" }, { "window_close", "barn_close", "barn" },
        { "window_button", "barn_button", "barn" }, { "window_slider", "barn_slider", "barn" },
        { "window_textbox", "barn_textbox", "barn" },
        { "tcl_eval", "tassel_tcl", "tassel" }, { "tk_eval", "tassel_tk", "tassel" }
    };
    size_t i;
    for (i = 0; i < sizeof(moved) / sizeof(moved[0]); i++)
        if (strcmp(name, moved[i].old) == 0) { *module = moved[i].module; return moved[i].now; }
    return NULL;
}
static const Program *g_check_top = NULL;
static int check_lookup(const char *name, int *nparams) {
    size_t i;
    FuncEntry *f;
    if (g_check_top) {
        for (i = 0; i < g_check_top->count; i++) {
            const Stmt *s = &g_check_top->items[i];
            if (s->kind == STMT_RECIPE && strcmp(s->recipe_name, name) == 0) { *nparams = s->recipe_nparams; return 1; }
        }
    }
    f = func_find(name);
    if (f) { *nparams = f->def->recipe_nparams; return 1; }
    return 0;
}
static int check_expr(const Expr *e, int lenient) {
    int i;
    if (!e) return 0;
    if (e->kind == EXPR_CALL) {
        int np;
        if (!check_lookup(e->var, &np)) {
            if (!lenient) {
                const char *mod = cob_stdlib_module_for(e->var), *moved_mod = NULL;
                const char *moved_to = cob_moved_builtin(e->var, &moved_mod);
                if (moved_to) cob_report(e->line, "error", "'%s' is now %s() in the standard library: add `shuck %s` at the top and call %s(...)", e->var, moved_to, moved_mod, moved_to);
                else if (mod) cob_report(e->line, "error", "'%s' is not defined -- it is in the standard library: add `shuck %s` at the top", e->var, mod);
                else cob_report(e->line, "error", "call to '%s', which is not a recipe or a library function", e->var);
                return -1;
            }
        } else if (np != e->argc) {
            cob_report(e->line, "error", "recipe '%s' takes %d argument%s but %d %s given",
                       e->var, np, np == 1 ? "" : "s", e->argc, e->argc == 1 ? "was" : "were");
            return -1;
        }
    }
    if (check_expr(e->left, lenient) || check_expr(e->right, lenient) || check_expr(e->arg3, lenient)) return -1;
    for (i = 0; i < e->argc; i++) if (check_expr(e->args[i], lenient)) return -1;
    return 0;
}
/* `lenient` (REPL) applies ONLY inside recipe bodies, where a call may name
 * a recipe that will be typed later; anything that runs immediately must
 * already resolve. */
static int check_program(const Program *p, int lenient, int in_recipe) {
    size_t i;
    int l = in_recipe ? lenient : 0;
    for (i = 0; i < p->count; i++) {
        const Stmt *s = &p->items[i];
        switch (s->kind) {
            case STMT_POP: if (check_expr(s->pop_expr, l)) return -1; break;
            case STMT_SET: if (check_expr(s->set_expr, l)) return -1; break;
            case STMT_SERVE: if (check_expr(s->serve_expr, l)) return -1; break;
            case STMT_CALL: if (check_expr(s->call_expr, l)) return -1; break;
            case STMT_WHILE:
                if (check_expr(s->while_cond->left, l) || check_expr(s->while_cond->right, l)) return -1;
                if (check_program(&s->while_body, lenient, in_recipe)) return -1;
                break;
            case STMT_IF:
                if (check_expr(s->if_cond->left, l) || check_expr(s->if_cond->right, l)) return -1;
                if (check_program(&s->if_body, lenient, in_recipe) || check_program(&s->else_body, lenient, in_recipe)) return -1;
                break;
            case STMT_RECIPE: if (check_program(&s->recipe_body, lenient, 1)) return -1; break;
            case STMT_TRASH: case STMT_EXTERN: break;
        }
    }
    return 0;
}
static int cob_check_calls(const Program *top, int lenient) {
    int rc;
    g_check_top = top;
    rc = check_program(top, lenient, 0);
    g_check_top = NULL;
    return rc;
}

#if !defined(COB_WASM) /* only the REPL checks a lone expression */
static int cob_check_calls_expr(const Expr *e, int lenient) {
    g_check_top = NULL;
    return check_expr(e, lenient);
}
#endif

/* ---------------------------------------------------------------------
 * EVALUATION
 * ------------------------------------------------------------------- */
static int execute(const Program *prog, EvalCtx *ctx); /* fwd: a call runs a recipe body */
static Value call_recipe(const Expr *e, EvalCtx *ctx);

/* A variable lookup inside a recipe sees its locals first, then the globals. */
static int ctx_get(EvalCtx *ctx, const char *name, Value *out) {
    if (ctx->locals && vars_get(ctx->locals, name, out)) return 1;
    return vars_get(ctx->vars, name, out);
}
/* Renders an int as decimal text for string concatenation (e.g.
 * "score: " + 7). Buffer is comfortably large for any long. */
static void long_to_str(long n, char *buf, size_t bufsize) {
    snprintf(buf, bufsize, "%ld", n);
}

/* ---------------------------------------------------------------------
 * C API / FFI (CEP-2) -- RUN-TIME SIDE
 *
 * `extern f(x) from "lib"` resolves EAGERLY, at the point the statement
 * runs (not on first call): a missing library or a misspelled symbol
 * should fail at the line that named it, not at some later, possibly
 * never-executed call site. On failure a warning goes to stderr and `f`
 * is bound to a stub that evaluates to 0 -- "warn and return a harmless
 * default, never crash the interpreter". The whole standard library
 * (porch, kernels, pantry, barn ...) is loaded through this.
 *
 * Every native function has the one signature in include/cob.h
 * (CobFFIFunc), so a call is always: evaluate args -> CobValue[] ->
 * one call through one function pointer -> convert the result back.
 *
 * In the COB_WASM sandbox there is no dlopen(). Standard library modules
 * that can run in a browser (porch, cornmath, kernels, almanac) are
 * compiled in (COB_STDLIB_STATIC) and bound by name; any other extern is a
 * permanent stub, and the sandbox notice is printed once per library.
 * ------------------------------------------------------------------- */
#if defined(COB_STDLIB_STATIC)
/* Generated by the Makefile from the stdlib C sources: declarations plus
 * cob_static_table[] = { { "name", fn }, ... }. */
#include "stdlib_static.h"
static CobFFIFunc cob_static_find(const char *name) {
    size_t i;
    for (i = 0; i < sizeof(cob_static_table) / sizeof(cob_static_table[0]); i++)
        if (strcmp(cob_static_table[i].name, name) == 0) return cob_static_table[i].fn;
    return NULL;
}
#endif

typedef struct { char *name; CobFFIFunc fn; void *lib; } ExternBinding;
static ExternBinding *g_externs = NULL;
static size_t g_extern_count = 0, g_extern_cap = 0;

static ExternBinding *cob_extern_find(const char *name) {
    size_t i;
    for (i = 0; i < g_extern_count; i++)
        if (strcmp(g_externs[i].name, name) == 0) return &g_externs[i];
    return NULL;
}

#if !defined(COB_WASM)
static void *cob_lib_open(const char *path) {
    void *h = NULL;
    char local[1100];
#if defined(_WIN32)
    int has_sep = (strchr(path, '/') != NULL || strchr(path, '\\') != NULL);
    if (!has_sep && snprintf(local, sizeof(local), ".\\%s", path) < (int)sizeof(local))
        h = (void *)LoadLibraryA(local);
    if (!h) h = (void *)LoadLibraryA(path);
    /* a bare module name ("porch"): look in the standard library directory */
    if (!h && !has_sep && strchr(path, '.') == NULL) {
        char sp[1100];
        if (cob_stdlib_lib_path(path, sp, sizeof(sp)) == 0) h = (void *)LoadLibraryA(sp);
    }
#else
    /* dlopen() with a slash-free name searches the system library path,
     * not the current directory -- so `from "math_ext.so"` would never
     * find a file sitting next to the script. Try ./name first, then the
     * name as given (which is what makes `from "libm.so.6"` still work). */
    if (strchr(path, '/') == NULL && snprintf(local, sizeof(local), "./%s", path) < (int)sizeof(local))
        h = dlopen(local, RTLD_NOW | RTLD_LOCAL);
    if (!h) h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    /* a bare module name ("porch"): look in the standard library directory */
    if (!h && strchr(path, '/') == NULL && strchr(path, '.') == NULL) {
        char sp[1100];
        if (cob_stdlib_lib_path(path, sp, sizeof(sp)) == 0) h = dlopen(sp, RTLD_NOW | RTLD_LOCAL);
    }
#endif
    return h;
}
static CobFFIFunc cob_lib_sym(void *lib, const char *name) {
    CobFFIFunc fn = NULL;
    void *sym;
#if defined(_WIN32)
    sym = (void *)GetProcAddress(lib, name);
#else
    sym = dlsym(lib, name);
#endif
    /* memcpy, not a cast: object-pointer <-> function-pointer conversion
     * isn't strictly ISO C, and this keeps -Wpedantic quiet too. */
    memcpy(&fn, &sym, sizeof(fn) < sizeof(sym) ? sizeof(fn) : sizeof(sym));
    return fn;
}
static void cob_lib_close(void *lib) {
    if (!lib) return;
#if defined(_WIN32)
    FreeLibrary(lib);
#else
    dlclose(lib);
#endif
}
#endif /* !COB_WASM */

#if !defined(COB_WASM)
/* Libraries that failed to load, so the warning is printed once each. */
static char *g_missing_libs[16];
static int g_missing_count = 0;
static int cob_lib_known_missing(const char *lib_path) {
    int i;
    for (i = 0; i < g_missing_count; i++) if (strcmp(g_missing_libs[i], lib_path) == 0) return 1;
    return 0;
}
static void cob_lib_note_missing(const char *lib_path) {
    if (g_missing_count < 16) g_missing_libs[g_missing_count++] = cob_strdup(lib_path);
}
static void cob_lib_forget_missing(void) {
    int i;
    for (i = 0; i < g_missing_count; i++) free(g_missing_libs[i]);
    g_missing_count = 0;
}
#endif

/* Finds or creates the binding slot for `name`; an existing one is reset
 * (the same `extern` line may run again, e.g. inside a while loop: drop
 * the previous library handle rather than leak it). NULL on OOM. */
static ExternBinding *cob_extern_slot(const char *name) {
    ExternBinding *b = cob_extern_find(name);
    if (!b) {
        if (g_extern_count == g_extern_cap) {
            size_t new_cap = g_extern_cap ? g_extern_cap * 2 : 8;
            ExternBinding *grown = (ExternBinding *)realloc(g_externs, new_cap * sizeof(ExternBinding));
            if (!grown) { fprintf(stderr, "[cob_interp] out of memory binding extern '%s'\n", name); return NULL; }
            g_externs = grown; g_extern_cap = new_cap;
        }
        b = &g_externs[g_extern_count];
        memset(b, 0, sizeof(*b));
        b->name = cob_strdup(name);
        if (!b->name) { fprintf(stderr, "[cob_interp] out of memory binding extern '%s'\n", name); return NULL; }
        g_extern_count++;
    } else {
#if !defined(COB_WASM)
        cob_lib_close(b->lib);
#endif
        b->lib = NULL; b->fn = NULL;
    }
    return b;
}

#if defined(COB_WASM)
/* The sandbox notice, once per library no matter how many functions it declares. */
static char *g_web_warned_libs[32];
static int g_web_warned_count = 0;
static void cob_web_warn_lib(const char *lib_path) {
    int i;
    for (i = 0; i < g_web_warned_count; i++) if (strcmp(g_web_warned_libs[i], lib_path) == 0) return;
    if (g_web_warned_count < 32) g_web_warned_libs[g_web_warned_count++] = cob_strdup(lib_path);
    printf(COB_WEB_WARNING_EXTERN_LINE1_FMT, lib_path);
    fputs(COB_WEB_WARNING_EXTERN_LINE2 "\n", stdout);
    fflush(stdout);
}
#endif

static void cob_extern_bind(const char *name, const char *lib_path) {
    ExternBinding *b = cob_extern_slot(name);
    if (!b) return;
#if defined(COB_STDLIB_STATIC)
    /* stdlib modules compiled into this build bind by name, no library needed */
    {
        CobFFIFunc sfn = cob_static_find(name);
        if (sfn) { b->fn = sfn; return; }
    }
#endif
#if defined(COB_WASM)
    cob_web_warn_lib(lib_path);
#else
    {
        void *lib; CobFFIFunc fn = NULL;
        if (cob_lib_known_missing(lib_path)) return; /* already reported: stay a silent stub */
        lib = cob_lib_open(lib_path);
        if (!lib) {
            /* said once per library, not once per function it declares */
#if defined(_WIN32)
            fprintf(stderr, "[cob_interp] warning: could not load library '%s' (needed by extern %s and the other functions it declares); calls to them will return 0\n",
                    lib_path, name);
#else
            const char *why = dlerror();
            fprintf(stderr, "[cob_interp] warning: could not load library '%s' (%s); calls to extern %s and the other functions it declares will return 0\n",
                    lib_path, why ? why : "unknown error", name);
#endif
            cob_lib_note_missing(lib_path);
            return;
        }
        fn = cob_lib_sym(lib, name);
        if (!fn) {
            fprintf(stderr, "[cob_interp] warning: extern %s -- symbol '%s' not found in '%s'; calls to it will return 0\n",
                    name, name, lib_path);
            cob_lib_close(lib);
            return;
        }
        b->lib = lib; b->fn = fn;
    }
#endif
}

static void cob_extern_shutdown(void) {
    size_t i;
    for (i = 0; i < g_extern_count; i++) {
#if !defined(COB_WASM)
        cob_lib_close(g_externs[i].lib);
#endif
        free(g_externs[i].name);
    }
    free(g_externs);
    g_externs = NULL; g_extern_count = g_extern_cap = 0;
#if !defined(COB_WASM)
    cob_lib_forget_missing();
#endif
    cob_extern_decl_free();
}

/* Called once at process exit (both cache-hit and normal-execution
 * paths in main()) so valgrind sees clean teardown of any open SQLite
 * handles / the Tcl interpreter, same spirit as the rest of this file. */
static void cob_ext_shutdown(void) {
    funcs_free();
    cob_extern_shutdown();
}

/* ---------------------------------------------------------------------
 * LINE READING -- used by the REPL (console input for programs is the
 * standard library's `porch` module now)
 * ------------------------------------------------------------------- */
#if !defined(COB_WASM)
/* Prints `prompt` (no newline, flushed), then reads one line from stdin.
 * Returns a malloc'd string with the trailing "\n" / "\r\n" removed; ""
 * at EOF. *got_line is set to 1 if a line (even an empty one) was read,
 * 0 at EOF/error. Lines of any length are supported. */
static char *cob_read_line(const char *prompt, int *got_line) {
    size_t cap = 128, len = 0;
    char *buf = (char *)malloc(cap);
    *got_line = 0;
    if (!buf) return NULL;
    if (prompt && prompt[0]) { fputs(prompt, stdout); }
    fflush(stdout);
    for (;;) {
        if (!fgets(buf + len, (int)(cap - len), stdin)) break;
        *got_line = 1;
        len += strlen(buf + len);
        if (len > 0 && buf[len - 1] == '\n') break;
        if (len + 1 >= cap) {
            char *grown;
            cap *= 2;
            grown = (char *)realloc(buf, cap);
            if (!grown) { free(buf); return NULL; }
            buf = grown;
        }
    }
    if (!*got_line) len = 0;
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) len--;
    buf[len] = '\0';
    return buf;
}
#endif /* !COB_WASM */

static Value eval_expr(const Expr *e, EvalCtx *ctx) {
    switch (e->kind) {
        case EXPR_NUM: return value_int(e->num);
        case EXPR_STR: return value_str_take(cob_strdup(e->str_lit));
        case EXPR_VAR: {
            Value v;
            if (ctx_get(ctx, e->var, &v)) return v;
            fprintf(stderr, "[cob_interp] warning: undefined variable '%s', treated as 0\n", e->var);
            return value_int(0);
        }
        case EXPR_BINOP: {
            Value l = eval_expr(e->left, ctx);
            Value r = eval_expr(e->right, ctx);

            if (e->op == '+' && (l.kind == VAL_STR || r.kind == VAL_STR)) {
                char lbuf[32], rbuf[32];
                const char *ls = l.kind == VAL_STR ? l.s : (long_to_str(l.i, lbuf, sizeof(lbuf)), lbuf);
                const char *rs = r.kind == VAL_STR ? r.s : (long_to_str(r.i, rbuf, sizeof(rbuf)), rbuf);
                size_t ln = strlen(ls), rn = strlen(rs);
                char *cat = (char *)malloc(ln + rn + 1);
                Value result;
                if (cat) {
                    memcpy(cat, ls, ln); memcpy(cat + ln, rs, rn); cat[ln + rn] = '\0';
                    result = value_str_take(cat);
                } else {
                    result = value_int(0);
                }
                value_free(&l); value_free(&r);
                return result;
            }
            if (l.kind == VAL_STR || r.kind == VAL_STR) {
                fprintf(stderr,
                    "[cob_interp] warning: operator '%c' requires two numbers, got a string; result treated as 0\n",
                    e->op);
                value_free(&l); value_free(&r);
                return value_int(0);
            }
            /* both plain integers -- original arithmetic, unchanged */
            switch (e->op) {
                case '+': return value_int(l.i + r.i);
                case '-': return value_int(l.i - r.i);
                case '*': return value_int(l.i * r.i);
                case '/':
                    if (r.i == 0) { fprintf(stderr, "[cob_interp] warning: division by zero, result treated as 0\n"); return value_int(0); }
                    return value_int(l.i / r.i);
            }
            return value_int(0);
        }
        case EXPR_HARVEST: {
            Value bytesV = eval_expr(e->left, ctx);
            long bytes;
            if (bytesV.kind == VAL_STR) {
                fprintf(stderr, "[cob_interp] warning: harvest() needs a number, got a string; treated as 0\n");
                value_free(&bytesV);
                bytes = 0;
            } else {
                bytes = bytesV.i;
            }
            if (bytes < 0) {
                fprintf(stderr, "[cob_interp] warning: harvest() with negative size, treated as 0\n");
                bytes = 0;
            }
            long id = handle_alloc(ctx->handles, (size_t)bytes);
            if (id == 0) {
                fprintf(stderr, "[cob_interp] warning: harvest(%ld) failed (out of memory)\n", bytes);
            }
            return value_int(id);
        }
        case EXPR_CALL: return call_recipe(e, ctx);
        case EXPR_EXTERN_CALL: {
            int n = e->argc, i;
            Value *vals = (Value *)calloc((size_t)(n > 0 ? n : 1), sizeof(Value));
            CobValue *argv = (CobValue *)calloc((size_t)(n > 0 ? n : 1), sizeof(CobValue));
            ExternBinding *b = cob_extern_find(e->var);
            Value result;
            if (!vals || !argv) {
                fprintf(stderr, "[cob_interp] out of memory calling extern '%s'\n", e->var);
                free(vals); free(argv);
                return value_int(0);
            }
            for (i = 0; i < n; i++) {
                vals[i] = eval_expr(e->args[i], ctx);
                if (vals[i].kind == VAL_STR) { argv[i].kind = COB_STR; argv[i].as.s = vals[i].s; }
                else                         { argv[i].kind = COB_INT; argv[i].as.i = vals[i].i; }
            }
            if (!b) {
                fprintf(stderr, "[cob_interp] warning: extern '%s' called before its `extern` statement ran; treated as 0\n", e->var);
                result = value_int(0);
            } else if (!b->fn) {
                /* failed or sandboxed binding: already warned once, stay quiet per call */
                result = value_int(0);
            } else {
                CobValue r = b->fn(argv, n);
                if (r.kind == COB_STR) {
                    char *copy = cob_strdup(r.as.s ? r.as.s : "");
                    result = copy ? value_str_take(copy) : value_int(0);
                } else {
                    result = value_int(r.as.i);
                }
            }
            for (i = 0; i < n; i++) value_free(&vals[i]);
            free(vals); free(argv);
            return result;
        }
    }
    return value_int(0);
}


/* Runs a recipe: arguments are evaluated left to right in the CALLER'S
 * scope, a fresh set of local variables is built (every local starts at 0,
 * parameters take the argument values), the body runs, and whatever
 * `serve` handed back is the value of the call (0 if the body ran off its
 * end). Arity was checked before the program started; the runtime checks
 * below only matter for hand-made caches and the REPL. */
static Value call_recipe(const Expr *e, EvalCtx *ctx) {
    FuncEntry *f = func_find(e->var);
    const Stmt *d;
    Value *argv = NULL;
    VarTable locals, *saved_locals;
    Value saved_ret, result;
    int i, returned;

    if (!f) {
        cob_report(0, "warning", "call to undefined recipe '%s', treated as 0", e->var);
        return value_int(0);
    }
    d = f->def;
    if (e->argc != d->recipe_nparams) {
        cob_report(0, "warning", "recipe '%s' takes %d argument(s) but %d given; call treated as 0",
                   e->var, d->recipe_nparams, e->argc);
        return value_int(0);
    }
    if (e->argc > 0) {
        argv = (Value *)calloc((size_t)e->argc, sizeof(Value));
        if (!argv) { fprintf(stderr, "[cob_interp] out of memory calling recipe '%s'\n", e->var); return value_int(0); }
    }
    for (i = 0; i < e->argc; i++) argv[i] = eval_expr(e->args[i], ctx);

    if (ctx->call_depth >= COB_MAX_CALL_DEPTH) {
        cob_report(0, "warning", "recipe '%s' is nested more than %d calls deep; call treated as 0",
                   e->var, COB_MAX_CALL_DEPTH);
        for (i = 0; i < e->argc; i++) value_free(&argv[i]);
        free(argv);
        return value_int(0);
    }

    vars_init(&locals);
    for (i = 0; i < f->nlocals; i++) vars_set(&locals, f->locals[i], value_int(0));
    for (i = 0; i < d->recipe_nparams; i++) {
        if (vars_set(&locals, d->recipe_params[i], argv[i]) != 0) value_free(&argv[i]);
    }
    free(argv);

    saved_locals = ctx->locals; saved_ret = ctx->ret;
    ctx->locals = &locals; ctx->ret = value_int(0); ctx->call_depth++;
    returned = execute(&d->recipe_body, ctx);
    result = ctx->ret;
    ctx->ret = saved_ret; ctx->locals = saved_locals; ctx->call_depth--;
    ctx->fell_off = !returned;
    vars_free(&locals);
    return result;
}

static int eval_cond(const Cond *c, EvalCtx *ctx) {
    Value l = eval_expr(c->left, ctx);
    if (c->op == COND_TRUTHY) {
        int truthy = (l.kind == VAL_STR) ? (l.s != NULL && l.s[0] != '\0') : (l.i != 0);
        value_free(&l);
        return truthy;
    }
    Value r = eval_expr(c->right, ctx);
    int result;
    if (l.kind != r.kind) {
        fprintf(stderr, "[cob_interp] warning: comparing a number to a string; result is false\n");
        result = 0;
    } else if (l.kind == VAL_STR) {
        int cmp = strcmp(l.s, r.s);
        switch (c->op) {
            case COND_EQ: result = (cmp == 0); break;
            case COND_NE: result = (cmp != 0); break;
            case COND_LT: result = (cmp < 0); break;
            case COND_GT: result = (cmp > 0); break;
            case COND_LE: result = (cmp <= 0); break;
            case COND_GE: result = (cmp >= 0); break;
            default: result = 0;
        }
    } else {
        switch (c->op) {
            case COND_EQ: result = (l.i == r.i); break;
            case COND_NE: result = (l.i != r.i); break;
            case COND_LT: result = (l.i < r.i); break;
            case COND_GT: result = (l.i > r.i); break;
            case COND_LE: result = (l.i <= r.i); break;
            case COND_GE: result = (l.i >= r.i); break;
            default: result = 0;
        }
    }
    value_free(&l); value_free(&r);
    return result;
}

/* Runs a block. Returns 1 if a `serve` ran inside it (the caller must stop
 * and hand the value up through ctx->ret), 0 if it simply finished. */
static int execute(const Program *prog, EvalCtx *ctx) {
    size_t i;
    for (i = 0; i < prog->count; i++) {
        const Stmt *s = &prog->items[i];
        switch (s->kind) {
            case STMT_POP: {
                Value v = eval_expr(s->pop_expr, ctx);
                if (v.kind == VAL_STR) {
                    fwrite(v.s, 1, strlen(v.s), stdout);
                } else {
                    char buf[32];
                    int n = snprintf(buf, sizeof(buf), "%ld", v.i);
                    if (n > 0) fwrite(buf, 1, (size_t)n, stdout);
                }
                fputc('\n', stdout);
#if defined(COB_WASM)
                /* Browser hosts that capture stdout byte by byte (Pounce) would
                 * otherwise see output only when libc's buffer fills or at exit. */
                fflush(stdout);
#endif
                value_free(&v);
                break;
            }
            case STMT_SET: {
                Value v = eval_expr(s->set_expr, ctx);
                /* inside a recipe, every `set` target is one of its locals */
                if (vars_set(ctx->locals ? ctx->locals : ctx->vars, s->set_name, v) != 0) {
                    fprintf(stderr, "[cob_interp] out of memory setting variable '%s'\n", s->set_name);
                    value_free(&v); /* vars_set didn't take ownership on this failure path */
                }
                break;
            }
            case STMT_TRASH: {
                Value v;
                if (!ctx_get(ctx, s->trash_name, &v)) {
                    fprintf(stderr, "[cob_interp] warning: trash() on undefined variable '%s'\n", s->trash_name);
                    break;
                }
                if (v.kind == VAL_STR) {
                    fprintf(stderr, "[cob_interp] warning: trash(%s) -- variable holds a string, not a handle\n", s->trash_name);
                    value_free(&v);
                    break;
                }
                if (handle_free(ctx->handles, v.i) != 0) {
                    fprintf(stderr,
                        "[cob_interp] warning: trash(%s) -- handle %ld is invalid or already freed\n",
                        s->trash_name, v.i);
                }
                break;
            }
            case STMT_EXTERN:
                cob_extern_bind(s->ext_name, s->ext_lib);
                break;
            case STMT_IF:
                if (eval_cond(s->if_cond, ctx)) { if (execute(&s->if_body, ctx)) return 1; }
                else { if (execute(&s->else_body, ctx)) return 1; }
                break;
            case STMT_RECIPE:
                break; /* hoisted: already registered before the program started */
            case STMT_SERVE: {
                Value v = eval_expr(s->serve_expr, ctx);
                value_free(&ctx->ret);
                ctx->ret = v;
                return 1;
            }
            case STMT_CALL: {
                Value v = eval_expr(s->call_expr, ctx);
                value_free(&v);
                break;
            }
            case STMT_WHILE: {
                unsigned long iterations = 0;
                while (eval_cond(s->while_cond, ctx)) {
                    if (execute(&s->while_body, ctx)) return 1;
                    iterations++;
                    if (iterations >= COB_MAX_LOOP_ITERATIONS) {
                        fprintf(stderr,
                            "[cob_interp] warning: while loop hit the %lu-iteration safety cap, stopping it early\n",
                            COB_MAX_LOOP_ITERATIONS);
                        break;
                    }
                }
                break;
            }
        }
    }
    return 0;
}

/* ---------------------------------------------------------------------
 * STATEMENT PARSERS (each consumes exactly one physical line's content)
 * ------------------------------------------------------------------- */
static int parse_pop_statement(const char *trimmed_line, int line_no, Expr **out_expr) {
    const char *p = trimmed_line + strlen(COB_KW_POP);
    p = skip_blank(p);
    if (*p != '(') {
        fprintf(stderr, "[cob_interp] syntax error at line %d: expected '(' after pop\n", line_no);
        return -1;
    }
    p++; p = skip_blank(p);
    Expr *expr;
    p = parse_expr(p, &expr);
    if (!p) {
        fprintf(stderr, "[cob_interp] syntax error at line %d: malformed expression in pop()\n", line_no);
        return -1;
    }
    p = skip_blank(p);
    if (*p != ')') {
        fprintf(stderr, "[cob_interp] syntax error at line %d: expected ')' to close pop(\n", line_no);
        expr_free(expr);
        return -1;
    }
    *out_expr = expr;
    return 0;
}

static int parse_set_statement(const char *after_kw, int line_no, char **out_name, Expr **out_expr) {
    const char *p = skip_blank(after_kw);
    char *name;
    p = parse_identifier(p, &name);
    if (!p) {
        fprintf(stderr, "[cob_interp] syntax error at line %d: expected a variable name after 'set'\n", line_no);
        return -1;
    }
    p = skip_blank(p);
    if (*p != '=') {
        fprintf(stderr, "[cob_interp] syntax error at line %d: expected '=' in set statement\n", line_no);
        free(name);
        return -1;
    }
    p++;
    Expr *expr;
    p = parse_expr(p, &expr);
    if (!p) {
        fprintf(stderr, "[cob_interp] syntax error at line %d: malformed expression in set statement\n", line_no);
        free(name);
        return -1;
    }
    *out_name = name;
    *out_expr = expr;
    return 0;
}

static int parse_while_header(const char *after_kw, int line_no, Cond **out_cond) {
    Cond *cond;
    const char *p = parse_condition(after_kw, &cond);
    if (!p) {
        fprintf(stderr, "[cob_interp] syntax error at line %d: malformed condition in while statement\n", line_no);
        return -1;
    }
    p = skip_blank(p);
    if (*p != COB_BLOCK_COLON) {
        fprintf(stderr, "[cob_interp] syntax error at line %d: expected ':' to open while block\n", line_no);
        cond_free(cond);
        return -1;
    }
    *out_cond = cond;
    return 0;
}

static int parse_if_header(const char *after_kw, int line_no, const char *kw_name, Cond **out_cond) {
    Cond *cond;
    const char *p = parse_condition(after_kw, &cond);
    if (!p) {
        fprintf(stderr, "[cob_interp] syntax error at line %d: malformed condition in %s statement\n", line_no, kw_name);
        return -1;
    }
    p = skip_blank(p);
    if (*p != COB_BLOCK_COLON) {
        fprintf(stderr, "[cob_interp] syntax error at line %d: expected ':' to open %s block\n", line_no, kw_name);
        cond_free(cond);
        return -1;
    }
    *out_cond = cond;
    return 0;
}

static int parse_trash_statement(const char *after_kw, int line_no, char **out_name) {
    const char *p = skip_blank(after_kw);
    if (*p != '(') {
        fprintf(stderr, "[cob_interp] syntax error at line %d: expected '(' after trash\n", line_no);
        return -1;
    }
    p = skip_blank(p + 1);
    char *name;
    p = parse_identifier(p, &name);
    if (!p) {
        fprintf(stderr, "[cob_interp] syntax error at line %d: expected a variable name in trash()\n", line_no);
        return -1;
    }
    p = skip_blank(p);
    if (*p != ')') {
        fprintf(stderr, "[cob_interp] syntax error at line %d: expected ')' to close trash(\n", line_no);
        free(name);
        return -1;
    }
    *out_name = name;
    return 0;
}

/* extern <name>(<param>, ...) from "<library>"          (CEP-2)
 *
 * Parameter names are accepted for readability but carry no type
 * information and are not stored: every argument is just whichever
 * value (int or string) it evaluates to, and arity is deliberately not
 * checked against the native function (CEP-2 3.5). Registers <name> so
 * later calls parse as extern calls. */
static int cob_func_exists(const char *name); /* defined with the function registry below */

static int parse_extern_statement(const char *after_kw, int line_no, char **out_name, char **out_lib) {
    const char *p = skip_blank(after_kw);
    char *name = NULL, *lib = NULL, *param = NULL;
    size_t lib_len = 0, kw_len;

    p = parse_identifier(p, &name);
    if (!p) {
        fprintf(stderr, "[cob_interp] syntax error at line %d: expected a function name after 'extern'\n", line_no);
        return -1;
    }
    if (cob_is_reserved_word(name)) {
        fprintf(stderr, "[cob_interp] syntax error at line %d: extern cannot reuse the reserved name '%s'\n", line_no, name);
        goto fail;
    }
    if (cob_func_exists(name)) {
        fprintf(stderr, "[cob_interp] syntax error at line %d: extern cannot reuse the name '%s' of a recipe\n", line_no, name);
        goto fail;
    }
    p = skip_blank(p);
    if (*p != '(') {
        fprintf(stderr, "[cob_interp] syntax error at line %d: expected '(' after extern %s\n", line_no, name);
        goto fail;
    }
    p = skip_blank(p + 1);
    if (*p != ')') {
        int nparams = 0;
        for (;;) {
            p = parse_identifier(p, &param);
            if (!p) {
                fprintf(stderr, "[cob_interp] syntax error at line %d: expected a parameter name in extern %s(...)\n", line_no, name);
                goto fail;
            }
            free(param); param = NULL;
            if (++nparams > COB_MAX_EXTERN_ARGS) {
                fprintf(stderr, "[cob_interp] syntax error at line %d: extern %s has too many parameters (max %d)\n",
                        line_no, name, COB_MAX_EXTERN_ARGS);
                goto fail;
            }
            p = skip_blank(p);
            if (*p == ',') { p = skip_blank(p + 1); continue; }
            break;
        }
        if (*p != ')') {
            fprintf(stderr, "[cob_interp] syntax error at line %d: expected ')' to close extern %s(\n", line_no, name);
            goto fail;
        }
    }
    p = skip_blank(p + 1);
    kw_len = strlen(COB_KW_FROM);
    if (strncmp(p, COB_KW_FROM, kw_len) != 0 || is_ident_char(p[kw_len])) {
        fprintf(stderr, "[cob_interp] syntax error at line %d: expected 'from \"<library>\"' after extern %s(...)\n", line_no, name);
        goto fail;
    }
    p = skip_blank(p + kw_len);
    p = parse_string_literal(p, &lib, &lib_len);
    if (!p || lib_len == 0) {
        fprintf(stderr, "[cob_interp] syntax error at line %d: expected a non-empty library path string after 'from'\n", line_no);
        goto fail;
    }
    p = skip_blank(p);
    if (*p != '\0') {
        fprintf(stderr, "[cob_interp] syntax error at line %d: unexpected text after extern statement: %s\n", line_no, p);
        goto fail;
    }
    if (cob_extern_declare(name) != 0) {
        fprintf(stderr, "[cob_interp] out of memory declaring extern '%s'\n", name);
        goto fail;
    }
    *out_name = name; *out_lib = lib;
    return 0;
fail:
    free(name); free(lib); free(param);
    return -1;
}

/* ---------------------------------------------------------------------
 * LINE SPLITTING
 * ------------------------------------------------------------------- */
typedef struct { int indent; char *content; int line_no; } PhysLine;
typedef struct { PhysLine *items; size_t count, capacity; } PhysLines;

static void physlines_init(PhysLines *pl) { pl->items = NULL; pl->count = 0; pl->capacity = 0; }
static void physlines_free(PhysLines *pl) {
    size_t i;
    for (i = 0; i < pl->count; i++) free(pl->items[i].content);
    free(pl->items);
    pl->items = NULL; pl->count = 0; pl->capacity = 0;
}
static int physlines_push(PhysLines *pl, int indent, char *owned_content, int line_no) {
    if (pl->count == pl->capacity) {
        size_t new_cap = (pl->capacity == 0) ? 32 : pl->capacity * 2;
        PhysLine *grown = (PhysLine *)realloc(pl->items, new_cap * sizeof(PhysLine));
        if (!grown) return -1;
        pl->items = grown; pl->capacity = new_cap;
    }
    pl->items[pl->count].indent = indent;
    pl->items[pl->count].content = owned_content;
    pl->items[pl->count].line_no = line_no;
    pl->count++;
    return 0;
}

static int cob_strip_leading_makecache_directive(const char **source_ptr) {
    const char *s = *source_ptr;
    const char *line_end = strchr(s, '\n');
    size_t len = line_end ? (size_t)(line_end - s) : strlen(s);
    char buf[128];
    size_t i, j = 0;
    for (i = 0; i < len && j < sizeof(buf) - 1; i++) {
        char c = s[i];
        if (c == ' ' || c == '\t' || c == '\r') continue;
        buf[j++] = (char)tolower((unsigned char)c);
    }
    buf[j] = '\0';
    if (strcmp(buf, "_makecache=false") == 0) {
        *source_ptr = line_end ? line_end + 1 : s + len;
        return 1;
    }
    return 0;
}

static int cob_split_lines(const char *source, int start_line_no, PhysLines *out) {
    const char *line_start = source;
    int line_no = start_line_no;

    physlines_init(out);

    while (*line_start != '\0') {
        const char *line_end = strchr(line_start, '\n');
        size_t raw_len = line_end ? (size_t)(line_end - line_start) : strlen(line_start);
        char *line_buf = (char *)malloc(raw_len + 1);
        if (!line_buf) { physlines_free(out); return -1; }
        memcpy(line_buf, line_start, raw_len);
        line_buf[raw_len] = '\0';
        if (raw_len > 0 && line_buf[raw_len - 1] == '\r') line_buf[raw_len - 1] = '\0';

        if (line_buf[0] == '\t') {
            fprintf(stderr, "[cob_interp] indentation error at line %d: tabs are not allowed, use spaces\n", line_no);
            free(line_buf);
            physlines_free(out);
            return -1;
        }

        int indent = 0;
        while (line_buf[indent] == ' ') indent++;
        const char *trimmed = line_buf + indent;

        if (*trimmed == '\0' || *trimmed == '#') {
            free(line_buf);
        } else {
            size_t content_len = strlen(trimmed);
            char *content = (char *)malloc(content_len + 1);
            if (!content) { free(line_buf); physlines_free(out); return -1; }
            memcpy(content, trimmed, content_len + 1);
            free(line_buf);
            if (physlines_push(out, indent, content, line_no) != 0) {
                free(content);
                physlines_free(out);
                return -1;
            }
        }

        line_no++;
        line_start = line_end ? line_end + 1 : line_start + raw_len;
    }
    return 0;
}

/* ---------------------------------------------------------------------
 * `shuck` LIBRARY LOADING
 * ------------------------------------------------------------------- */
static const char *g_shuck_stack[COB_MAX_SHUCK_DEPTH];
static int g_shuck_depth = 0;

static int parse_block(PhysLines *lines, size_t *pos, int parent_indent, Program *out_prog);

/* Embedded stdlib wrappers (COB_STDLIB_STATIC builds, e.g. the browser):
 * a generated table of { module name, wrapper source }. Native builds
 * read the wrappers from the standard library directory instead. */
#if defined(COB_STDLIB_STATIC)
#include "stdlib_embed.h"
static const char *cob_stdlib_embedded(const char *name) {
    size_t i;
    for (i = 0; i < sizeof(cob_embedded_wrappers) / sizeof(cob_embedded_wrappers[0]); i++)
        if (strcmp(cob_embedded_wrappers[i].name, name) == 0) return cob_embedded_wrappers[i].source;
    return NULL;
}
#endif

/* Loads and fully parses <name>.cob, returning its top-level statements in
 * *out_prog. Search order: the current directory, then
 * cob_modules/<name>/<name>.cob (the layout `farmer harvest` installs
 * packages into), then the STANDARD LIBRARY directory ($COB_STDLIB, else
 * <this program>/lib) -- which is where `shuck porch`, `shuck cornmath`,
 * `shuck pantry` ... come from. */
static int cob_load_library(const char *name, int line_no, Program *out_prog) {
    char path_a[512], path_b[600], path_c[1200];
    char std_dir[1024];
    CobFileBuffer buf;
    CobFileStatus status;
    const char *source = NULL;
    int i, have_buf = 0;

    for (i = 0; i < g_shuck_depth; i++) {
        if (strcmp(g_shuck_stack[i], name) == 0) {
            fprintf(stderr, "[cob_interp] error at line %d: circular shuck detected for '%s'\n", line_no, name);
            return -1;
        }
    }
    if (g_shuck_depth >= COB_MAX_SHUCK_DEPTH) {
        fprintf(stderr, "[cob_interp] error at line %d: shuck nesting too deep (max %d)\n", line_no, COB_MAX_SHUCK_DEPTH);
        return -1;
    }

    snprintf(path_a, sizeof(path_a), "%s%s", name, COB_SOURCE_EXTENSION);
    snprintf(path_b, sizeof(path_b), "cob_modules%c%s%c%s%s",
             COB_PATH_SEP, name, COB_PATH_SEP, name, COB_SOURCE_EXTENSION);
    path_c[0] = '\0';

    status = cob_file_read_all(path_a, &buf);
    if (status != COB_FILE_OK) status = cob_file_read_all(path_b, &buf);
    if (status != COB_FILE_OK && cob_stdlib_dir(std_dir, sizeof(std_dir)) == 0) {
        snprintf(path_c, sizeof(path_c), "%s%c%s%s", std_dir, COB_PATH_SEP, name, COB_SOURCE_EXTENSION);
        status = cob_file_read_all(path_c, &buf);
    }
    if (status == COB_FILE_OK) { source = buf.data; have_buf = 1; }
#if defined(COB_STDLIB_STATIC)
    if (!source) source = cob_stdlib_embedded(name);
#endif
    if (!source) {
        fprintf(stderr, "[cob_interp] error at line %d: shuck '%s' failed -- tried '%s' and '%s'",
                line_no, name, path_a, path_b);
        if (path_c[0]) fprintf(stderr, " and '%s'", path_c);
        fputs("\n", stderr);
        return -1;
    }

    g_shuck_stack[g_shuck_depth++] = name;

    PhysLines lib_lines;
    int rc = cob_split_lines(source, 1, &lib_lines);
    if (have_buf) cob_file_free(&buf);
    if (rc != 0) { g_shuck_depth--; return -1; }

    size_t pos = 0;
    rc = parse_block(&lib_lines, &pos, -1, out_prog);
    physlines_free(&lib_lines);

    g_shuck_depth--;
    return rc;
}

/* ---------------------------------------------------------------------
 * `recipe` HEADER:  recipe <name>(<param>, <param>, ...):
 * ------------------------------------------------------------------- */
static int parse_recipe_header(const char *after_kw, int line_no, char **out_name, char ***out_params, int *out_np) {
    const char *p = skip_blank(after_kw);
    char *name = NULL, **params = NULL;
    int np = 0, cap = 0, i;

    p = parse_identifier(p, &name);
    if (!p) {
        fprintf(stderr, "[cob_interp] syntax error at line %d: expected a name after 'recipe'\n", line_no);
        return -1;
    }
    if (cob_is_reserved_word(name)) {
        fprintf(stderr, "[cob_interp] syntax error at line %d: '%s' is a Cob keyword and cannot name a recipe\n", line_no, name);
        goto fail;
    }
    p = skip_blank(p);
    if (*p != '(') {
        fprintf(stderr, "[cob_interp] syntax error at line %d: expected '(' after recipe %s\n", line_no, name);
        goto fail;
    }
    p = skip_blank(p + 1);
    if (*p != ')') {
        for (;;) {
            char *param = NULL;
            p = parse_identifier(p, &param);
            if (!p) {
                fprintf(stderr, "[cob_interp] syntax error at line %d: expected a parameter name in recipe %s\n", line_no, name);
                goto fail;
            }
            for (i = 0; i < np; i++) {
                if (strcmp(params[i], param) == 0) {
                    fprintf(stderr, "[cob_interp] syntax error at line %d: recipe %s has two parameters named '%s'\n", line_no, name, param);
                    free(param);
                    goto fail;
                }
            }
            if (np >= COB_MAX_EXTERN_ARGS) {
                fprintf(stderr, "[cob_interp] syntax error at line %d: recipe %s has too many parameters (max %d)\n", line_no, name, COB_MAX_EXTERN_ARGS);
                free(param);
                goto fail;
            }
            if (np == cap) {
                int new_cap = cap ? cap * 2 : 4;
                char **grown = (char **)realloc(params, (size_t)new_cap * sizeof(char *));
                if (!grown) { free(param); goto fail; }
                params = grown; cap = new_cap;
            }
            params[np++] = param;
            p = skip_blank(p);
            if (*p == ',') { p = skip_blank(p + 1); continue; }
            break;
        }
        if (*p != ')') {
            fprintf(stderr, "[cob_interp] syntax error at line %d: expected ',' or ')' in the parameters of recipe %s\n", line_no, name);
            goto fail;
        }
    }
    p = skip_blank(p + 1);
    if (*p != COB_BLOCK_COLON) {
        fprintf(stderr, "[cob_interp] syntax error at line %d: expected ':' at the end of the recipe line\n", line_no);
        goto fail;
    }
    p = skip_blank(p + 1);
    if (*p != '\0') {
        fprintf(stderr, "[cob_interp] syntax error at line %d: unexpected text after ':' on the recipe line\n", line_no);
        goto fail;
    }
    *out_name = name; *out_params = params; *out_np = np;
    return 0;
fail:
    free(name);
    for (i = 0; i < np; i++) free(params[i]);
    free(params);
    return -1;
}

/* ---------------------------------------------------------------------
 * RECURSIVE BLOCK PARSER
 * ------------------------------------------------------------------- */
/* Parses one `if` or `elif` line (the caller has already matched the
 * keyword, whose length is kw_len), its indented body, and any `elif` /
 * `else` that follows at the SAME indent, into *out. `elif` becomes a
 * nested STMT_IF stored as the only statement of else_body, so the
 * interpreter, cache format and compiler only ever deal with if/else.
 * On failure everything built so far is freed. */
static int parse_if_chain(PhysLines *lines, size_t *pos, size_t kw_len, const char *kw_name, Stmt *out) {
    PhysLine *pl = &lines->items[*pos];
    int line_no = pl->line_no;
    int this_indent = pl->indent;
    Cond *cond;
    Program body;
    g_parse_line = line_no;

    if (parse_if_header(pl->content + kw_len, line_no, kw_name, &cond) != 0) return -1;
    (*pos)++;
    if (parse_block(lines, pos, this_indent, &body) != 0) { cond_free(cond); return -1; }

    memset(out, 0, sizeof(*out));
    out->kind = STMT_IF;
    out->if_cond = cond;
    out->if_body = body;
    program_init(&out->else_body);

    if (*pos < lines->count && lines->items[*pos].indent == this_indent) {
        const char *next = lines->items[*pos].content;
        size_t elif_len = strlen(COB_KW_ELIF);
        size_t else_len = strlen(COB_KW_ELSE);

        if (strncmp(next, COB_KW_ELIF, elif_len) == 0 && next[elif_len] == ' ') {
            Stmt nested;
            if (parse_if_chain(lines, pos, elif_len, COB_KW_ELIF, &nested) != 0) {
                stmt_free_contents(out);
                return -1;
            }
            if (program_push(&out->else_body, nested) != 0) {
                stmt_free_contents(&nested);
                stmt_free_contents(out);
                return -1;
            }
        } else if (strncmp(next, COB_KW_ELSE, else_len) == 0) {
            const char *p = skip_blank(next + else_len);
            if (*p == COB_BLOCK_COLON && *skip_blank(p + 1) == '\0') {
                (*pos)++;
                if (parse_block(lines, pos, this_indent, &out->else_body) != 0) {
                    stmt_free_contents(out);
                    return -1;
                }
            }
            /* else: `else` not followed by ':' -- leave the line for
             * parse_block's "unrecognized statement" error. */
        }
    }
    return 0;
}

static int parse_block(PhysLines *lines, size_t *pos, int parent_indent, Program *out_prog) {
    program_init(out_prog);

    while (*pos < lines->count) {
        PhysLine *pl = &lines->items[*pos];
        if (pl->indent <= parent_indent) break;

        const char *trimmed = pl->content;
        int line_no = pl->line_no;
        size_t kw_len;
        g_parse_line = line_no;

        kw_len = strlen(COB_KW_POP);
        if (strncmp(trimmed, COB_KW_POP, kw_len) == 0 &&
            (trimmed[kw_len] == '(' || trimmed[kw_len] == ' ')) {
            Expr *pop_expr;
            if (parse_pop_statement(trimmed, line_no, &pop_expr) != 0) { program_free(out_prog); return -1; }
            Stmt s; memset(&s, 0, sizeof(s));
            s.kind = STMT_POP; s.pop_expr = pop_expr;
            if (program_push(out_prog, s) != 0) { expr_free(pop_expr); program_free(out_prog); return -1; }
            (*pos)++;
            continue;
        }

        kw_len = strlen(COB_KW_SET);
        if (strncmp(trimmed, COB_KW_SET, kw_len) == 0 && trimmed[kw_len] == ' ') {
            char *name; Expr *expr;
            if (parse_set_statement(trimmed + kw_len, line_no, &name, &expr) != 0) { program_free(out_prog); return -1; }
            Stmt s; memset(&s, 0, sizeof(s));
            s.kind = STMT_SET; s.set_name = name; s.set_expr = expr;
            if (program_push(out_prog, s) != 0) { free(name); expr_free(expr); program_free(out_prog); return -1; }
            (*pos)++;
            continue;
        }

        kw_len = strlen(COB_KW_WHILE);
        if (strncmp(trimmed, COB_KW_WHILE, kw_len) == 0 && trimmed[kw_len] == ' ') {
            Cond *cond;
            if (parse_while_header(trimmed + kw_len, line_no, &cond) != 0) { program_free(out_prog); return -1; }
            int this_indent = pl->indent;
            (*pos)++;
            Program body;
            if (parse_block(lines, pos, this_indent, &body) != 0) { cond_free(cond); program_free(out_prog); return -1; }
            Stmt s; memset(&s, 0, sizeof(s));
            s.kind = STMT_WHILE; s.while_cond = cond; s.while_body = body;
            if (program_push(out_prog, s) != 0) { cond_free(cond); program_free(&body); program_free(out_prog); return -1; }
            continue;
        }

        kw_len = strlen(COB_KW_IF);
        if (strncmp(trimmed, COB_KW_IF, kw_len) == 0 && trimmed[kw_len] == ' ') {
            Stmt s;
            if (parse_if_chain(lines, pos, kw_len, COB_KW_IF, &s) != 0) { program_free(out_prog); return -1; }
            if (program_push(out_prog, s) != 0) { stmt_free_contents(&s); program_free(out_prog); return -1; }
            continue;
        }

        if ((strncmp(trimmed, COB_KW_ELSE, strlen(COB_KW_ELSE)) == 0 &&
             (trimmed[strlen(COB_KW_ELSE)] == ':' || trimmed[strlen(COB_KW_ELSE)] == ' ' || trimmed[strlen(COB_KW_ELSE)] == '\0')) ||
            (strncmp(trimmed, COB_KW_ELIF, strlen(COB_KW_ELIF)) == 0 && trimmed[strlen(COB_KW_ELIF)] == ' ')) {
            fprintf(stderr, "[cob_interp] syntax error at line %d: '%s' without a matching 'if' at the same indentation\n",
                    line_no, (trimmed[2] == 's') ? COB_KW_ELSE : COB_KW_ELIF);
            program_free(out_prog);
            return -1;
        }

        kw_len = strlen(COB_KW_TRASH);
        if (strncmp(trimmed, COB_KW_TRASH, kw_len) == 0 && trimmed[kw_len] == '(') {
            char *name;
            if (parse_trash_statement(trimmed + kw_len, line_no, &name) != 0) { program_free(out_prog); return -1; }
            Stmt s; memset(&s, 0, sizeof(s));
            s.kind = STMT_TRASH; s.trash_name = name;
            if (program_push(out_prog, s) != 0) { free(name); program_free(out_prog); return -1; }
            (*pos)++;
            continue;
        }

        kw_len = strlen(COB_KW_EXTERN);
        if (strncmp(trimmed, COB_KW_EXTERN, kw_len) == 0 && trimmed[kw_len] == ' ') {
            char *name, *lib;
            if (parse_extern_statement(trimmed + kw_len, line_no, &name, &lib) != 0) { program_free(out_prog); return -1; }
            Stmt s; memset(&s, 0, sizeof(s));
            s.kind = STMT_EXTERN; s.ext_name = name; s.ext_lib = lib;
            if (program_push(out_prog, s) != 0) { free(name); free(lib); program_free(out_prog); return -1; }
            (*pos)++;
            continue;
        }

        kw_len = strlen(COB_KW_RECIPE);
        if (strncmp(trimmed, COB_KW_RECIPE, kw_len) == 0 && trimmed[kw_len] == ' ') {
            char *rname = NULL, **rparams = NULL;
            int rnp = 0, k, this_indent = pl->indent, brc;
            Program body;
            Stmt s;
            if (parent_indent != -1) {
                fprintf(stderr, "[cob_interp] syntax error at line %d: a recipe can only be defined at the top level of a program, not inside another block\n", line_no);
                program_free(out_prog);
                return -1;
            }
            if (parse_recipe_header(trimmed + kw_len, line_no, &rname, &rparams, &rnp) != 0) { program_free(out_prog); return -1; }
            (*pos)++;
            g_recipe_depth++;
            brc = parse_block(lines, pos, this_indent, &body);
            g_recipe_depth--;
            if (brc == 0 && body.count == 0) {
                fprintf(stderr, "[cob_interp] syntax error at line %d: recipe %s needs an indented body\n", line_no, rname);
                program_free(&body);
                brc = -1;
            }
            if (brc != 0) {
                free(rname);
                for (k = 0; k < rnp; k++) free(rparams[k]);
                free(rparams);
                program_free(out_prog);
                return -1;
            }
            memset(&s, 0, sizeof(s));
            s.kind = STMT_RECIPE; s.recipe_name = rname; s.recipe_params = rparams;
            s.recipe_nparams = rnp; s.recipe_body = body; s.recipe_line = line_no;
            if (program_push(out_prog, s) != 0) { stmt_free_contents(&s); program_free(out_prog); return -1; }
            continue;
        }

        kw_len = strlen(COB_KW_SERVE);
        if (strncmp(trimmed, COB_KW_SERVE, kw_len) == 0 && (trimmed[kw_len] == ' ' || trimmed[kw_len] == '\0')) {
            Expr *e = NULL;
            const char *p;
            Stmt s;
            if (g_recipe_depth == 0) {
                fprintf(stderr, "[cob_interp] syntax error at line %d: 'serve' can only be used inside a recipe\n", line_no);
                program_free(out_prog);
                return -1;
            }
            p = skip_blank(trimmed + kw_len);
            if (*p == '\0') {
                e = expr_new_num(0);
            } else {
                p = parse_expr(p, &e);
                if (!p) {
                    fprintf(stderr, "[cob_interp] syntax error at line %d: bad expression after 'serve'\n", line_no);
                    program_free(out_prog);
                    return -1;
                }
                p = skip_blank(p);
                if (*p != '\0') {
                    fprintf(stderr, "[cob_interp] syntax error at line %d: unexpected text after the serve expression: %s\n", line_no, p);
                    expr_free(e);
                    program_free(out_prog);
                    return -1;
                }
            }
            if (!e) { fprintf(stderr, "[cob_interp] out of memory parsing serve\n"); program_free(out_prog); return -1; }
            memset(&s, 0, sizeof(s));
            s.kind = STMT_SERVE; s.serve_expr = e;
            if (program_push(out_prog, s) != 0) { expr_free(e); program_free(out_prog); return -1; }
            (*pos)++;
            continue;
        }

        kw_len = strlen(COB_KW_SHUCK);
        if (strncmp(trimmed, COB_KW_SHUCK, kw_len) == 0 && trimmed[kw_len] == ' ') {
            char *lib_name;
            const char *p = skip_blank(trimmed + kw_len);
            p = parse_identifier(p, &lib_name);
            if (!p) {
                fprintf(stderr, "[cob_interp] syntax error at line %d: expected a library name after 'shuck'\n", line_no);
                program_free(out_prog);
                return -1;
            }
            Program lib_prog;
            int rc = cob_load_library(lib_name, line_no, &lib_prog);
            if (rc != 0) { free(lib_name); program_free(out_prog); return -1; }
            if (parent_indent != -1 && program_has_recipe(&lib_prog)) {
                fprintf(stderr, "[cob_interp] error at line %d: shuck '%s' defines recipes, so it must be at the top level of the program\n", line_no, lib_name);
                program_free(&lib_prog);
                free(lib_name);
                program_free(out_prog);
                return -1;
            }
            int append_rc = program_append_move(out_prog, &lib_prog);
            /* Whether the move fully succeeded or partially rolled back,
             * lib_prog's own array shell (and any un-moved remainder on
             * failure) still needs freeing -- program_free handles both
             * cases correctly since it only touches what lib_prog still
             * owns after program_append_move's bookkeeping. */
            program_free(&lib_prog);
            if (append_rc != 0) {
                fprintf(stderr, "[cob_interp] out of memory splicing shuck '%s'\n", lib_name);
                free(lib_name);
                program_free(out_prog);
                return -1;
            }
            free(lib_name);
            (*pos)++;
            continue;
        }

        /* a call standing alone: greet("Ada")  /  porch_ask("name? ")  (result dropped) */
        if (is_ident_start(*trimmed)) {
            const char *q = trimmed;
            while (is_ident_char((unsigned char)*q)) q++;
            q = skip_blank(q);
            if (*q == '(') {
                Expr *e = NULL;
                const char *p = parse_expr(trimmed, &e);
                Stmt s;
                if (!p) {
                    fprintf(stderr, "[cob_interp] syntax error at line %d: malformed call: %s\n", line_no, trimmed);
                    program_free(out_prog);
                    return -1;
                }
                p = skip_blank(p);
                if (*p != '\0') {
                    fprintf(stderr, "[cob_interp] syntax error at line %d: unexpected text after the call: %s\n", line_no, p);
                    expr_free(e);
                    program_free(out_prog);
                    return -1;
                }
                if (e->kind != EXPR_CALL && e->kind != EXPR_EXTERN_CALL) {
                    fprintf(stderr, "[cob_interp] syntax error at line %d: only a call to a recipe or a library function can stand alone as a statement: %s\n", line_no, trimmed);
                    expr_free(e);
                    program_free(out_prog);
                    return -1;
                }
                memset(&s, 0, sizeof(s));
                s.kind = STMT_CALL; s.call_expr = e;
                if (program_push(out_prog, s) != 0) { expr_free(e); program_free(out_prog); return -1; }
                (*pos)++;
                continue;
            }
        }

        fprintf(stderr, "[cob_interp] syntax error at line %d: unrecognized statement: %s\n", line_no, trimmed);
        program_free(out_prog);
        return -1;
    }
    return 0;
}

static int cob_parse_source(const char *source, Program *out_prog, int *out_disable_cache) {
    *out_disable_cache = cob_strip_leading_makecache_directive(&source);

    PhysLines lines;
    int start_line_no = *out_disable_cache ? 2 : 1;
    if (cob_split_lines(source, start_line_no, &lines) != 0) return -1;

    size_t pos = 0;
    int rc = parse_block(&lines, &pos, -1, out_prog);
    physlines_free(&lines);
    return rc;
}

/* ---------------------------------------------------------------------
 * .strawberry CACHE (binary AST dump). STRAWBERRY_MAGIC/_LEN now live
 * in common.h -- see the comment there for the format's version
 * history and why this is shared with popcorn_comp.c instead of two
 * independent copies. A cache written by an older cob_interp is simply
 * treated as a miss and regenerated, same as any other magic mismatch.
 * ------------------------------------------------------------------- */

typedef struct { unsigned char *data; size_t len, cap; } ByteBuf;

static int bytebuf_reserve(ByteBuf *b, size_t extra) {
    if (b->len + extra <= b->cap) return 0;
    size_t new_cap = (b->cap == 0) ? 256 : b->cap * 2;
    while (new_cap < b->len + extra) new_cap *= 2;
    unsigned char *grown = (unsigned char *)realloc(b->data, new_cap);
    if (!grown) return -1;
    b->data = grown; b->cap = new_cap;
    return 0;
}
static int bytebuf_u8(ByteBuf *b, uint8_t v) {
    if (bytebuf_reserve(b, 1) != 0) return -1;
    b->data[b->len++] = v;
    return 0;
}
static int bytebuf_u32(ByteBuf *b, uint32_t v) {
    if (bytebuf_reserve(b, 4) != 0) return -1;
    b->data[b->len++] = (unsigned char)(v & 0xFF);
    b->data[b->len++] = (unsigned char)((v >> 8) & 0xFF);
    b->data[b->len++] = (unsigned char)((v >> 16) & 0xFF);
    b->data[b->len++] = (unsigned char)((v >> 24) & 0xFF);
    return 0;
}
static int bytebuf_i64(ByteBuf *b, int64_t v) {
    uint64_t u = (uint64_t)v;
    if (bytebuf_reserve(b, 8) != 0) return -1;
    int i;
    for (i = 0; i < 8; i++) b->data[b->len++] = (unsigned char)((u >> (8 * i)) & 0xFF);
    return 0;
}
static int bytebuf_bytes(ByteBuf *b, const char *s, size_t len) {
    if (bytebuf_u32(b, (uint32_t)len) != 0) return -1;
    if (bytebuf_reserve(b, len) != 0) return -1;
    memcpy(b->data + b->len, s, len);
    b->len += len;
    return 0;
}

static int write_expr(ByteBuf *b, const Expr *e) {
    if (bytebuf_u8(b, (uint8_t)e->kind) != 0) return -1;
    switch (e->kind) {
        case EXPR_NUM: return bytebuf_i64(b, (int64_t)e->num);
        case EXPR_STR: return bytebuf_bytes(b, e->str_lit, e->str_lit_len);
        case EXPR_VAR: return bytebuf_bytes(b, e->var, strlen(e->var));
        case EXPR_BINOP:
            if (bytebuf_u8(b, (uint8_t)e->op) != 0) return -1;
            if (write_expr(b, e->left) != 0) return -1;
            return write_expr(b, e->right);
        case EXPR_HARVEST:
            return write_expr(b, e->left);
        case EXPR_EXTERN_CALL:
        case EXPR_CALL: {
            int i;
            if (bytebuf_bytes(b, e->var, strlen(e->var)) != 0) return -1;
            if (bytebuf_u32(b, (uint32_t)e->argc) != 0) return -1;
            for (i = 0; i < e->argc; i++) if (write_expr(b, e->args[i]) != 0) return -1;
            return 0;
        }
    }
    return -1;
}
static int write_cond(ByteBuf *b, const Cond *c) {
    if (bytebuf_u8(b, (uint8_t)c->op) != 0) return -1;
    if (write_expr(b, c->left) != 0) return -1;
    if (c->op != COND_TRUTHY) return write_expr(b, c->right);
    return 0;
}
static int write_program(ByteBuf *b, const Program *p);
static int write_stmt(ByteBuf *b, const Stmt *s) {
    if (bytebuf_u8(b, (uint8_t)s->kind) != 0) return -1;
    switch (s->kind) {
        case STMT_POP: return write_expr(b, s->pop_expr);
        case STMT_SET:
            if (bytebuf_bytes(b, s->set_name, strlen(s->set_name)) != 0) return -1;
            return write_expr(b, s->set_expr);
        case STMT_TRASH: return bytebuf_bytes(b, s->trash_name, strlen(s->trash_name));
        case STMT_EXTERN:
            if (bytebuf_bytes(b, s->ext_name, strlen(s->ext_name)) != 0) return -1;
            return bytebuf_bytes(b, s->ext_lib, strlen(s->ext_lib));
        case STMT_WHILE:
            if (write_cond(b, s->while_cond) != 0) return -1;
            return write_program(b, &s->while_body);
        case STMT_IF:
            if (write_cond(b, s->if_cond) != 0) return -1;
            if (write_program(b, &s->if_body) != 0) return -1;
            return write_program(b, &s->else_body);
        case STMT_RECIPE: {
            int k;
            if (bytebuf_bytes(b, s->recipe_name, strlen(s->recipe_name)) != 0) return -1;
            if (bytebuf_u32(b, (uint32_t)s->recipe_nparams) != 0) return -1;
            for (k = 0; k < s->recipe_nparams; k++)
                if (bytebuf_bytes(b, s->recipe_params[k], strlen(s->recipe_params[k])) != 0) return -1;
            return write_program(b, &s->recipe_body);
        }
        case STMT_SERVE: return write_expr(b, s->serve_expr);
        case STMT_CALL: return write_expr(b, s->call_expr);
    }
    return -1;
}
static int write_program(ByteBuf *b, const Program *p) {
    if (bytebuf_u32(b, (uint32_t)p->count) != 0) return -1;
    size_t i;
    for (i = 0; i < p->count; i++) if (write_stmt(b, &p->items[i]) != 0) return -1;
    return 0;
}

static void cob_write_strawberry_cache(const char *cache_path, const Program *prog) {
    ByteBuf b; memset(&b, 0, sizeof(b));
    if (bytebuf_reserve(&b, STRAWBERRY_MAGIC_LEN) != 0) return;
    memcpy(b.data, STRAWBERRY_MAGIC, STRAWBERRY_MAGIC_LEN);
    b.len = STRAWBERRY_MAGIC_LEN;

    if (write_program(&b, prog) != 0) {
        fprintf(stderr, "[cob_interp] warning: could not build .strawberry cache (out of memory)\n");
        free(b.data);
        return;
    }
    if (cob_file_write_all(cache_path, b.data, b.len) != COB_FILE_OK)
        fprintf(stderr, "[cob_interp] warning: failed to write fast-boot cache '%s'\n", cache_path);
    free(b.data);
}

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

static int read_expr(ByteReader *r, Expr **out) {
    uint8_t kind;
    if (reader_u8(r, &kind) != 0) return -1;
    switch (kind) {
        case EXPR_NUM: {
            int64_t v;
            if (reader_i64(r, &v) != 0) return -1;
            *out = expr_new_num((long)v);
            return *out ? 0 : -1;
        }
        case EXPR_STR: {
            char *text; size_t len;
            if (reader_bytes_alloc(r, &text, &len) != 0) return -1;
            *out = expr_new_str(text, len);
            if (!*out) { free(text); return -1; }
            return 0;
        }
        case EXPR_VAR: {
            char *name;
            if (reader_bytes_alloc(r, &name, NULL) != 0) return -1;
            *out = expr_new_var(name);
            if (!*out) { free(name); return -1; }
            return 0;
        }
        case EXPR_BINOP: {
            uint8_t op;
            if (reader_u8(r, &op) != 0) return -1;
            Expr *l, *rr;
            if (read_expr(r, &l) != 0) return -1;
            if (read_expr(r, &rr) != 0) { expr_free(l); return -1; }
            *out = expr_new_binop((char)op, l, rr);
            if (!*out) { expr_free(l); expr_free(rr); return -1; }
            return 0;
        }
        case EXPR_HARVEST: {
            Expr *inner;
            if (read_expr(r, &inner) != 0) return -1;
            *out = expr_new_harvest(inner);
            if (!*out) { expr_free(inner); return -1; }
            return 0;
        }
        case EXPR_EXTERN_CALL:
        case EXPR_CALL: {
            char *name; uint32_t argc, i; Expr **args = NULL;
            if (reader_bytes_alloc(r, &name, NULL) != 0) return -1;
            if (reader_u32(r, &argc) != 0 || argc > COB_MAX_EXTERN_ARGS) { free(name); return -1; }
            if (argc > 0) {
                args = (Expr **)calloc(argc, sizeof(Expr *));
                if (!args) { free(name); return -1; }
            }
            for (i = 0; i < argc; i++) {
                if (read_expr(r, &args[i]) != 0) {
                    uint32_t j;
                    for (j = 0; j < i; j++) expr_free(args[j]);
                    free(args); free(name);
                    return -1;
                }
            }
            *out = expr_new_named_call((ExprKind)kind, name, args, (int)argc);
            if (!*out) {
                for (i = 0; i < argc; i++) expr_free(args[i]);
                free(args); free(name);
                return -1;
            }
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
        if (read_expr(r, &c->right) != 0) { expr_free(c->left); free(c); return -1; }
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
            if (reader_bytes_alloc(r, &out->ext_lib, NULL) != 0) { free(out->ext_name); out->ext_name = NULL; return -1; }
            return 0;
        case STMT_WHILE:
            if (read_cond(r, &out->while_cond) != 0) return -1;
            return read_program(r, &out->while_body);
        case STMT_IF:
            if (read_cond(r, &out->if_cond) != 0) return -1;
            if (read_program(r, &out->if_body) != 0) { cond_free(out->if_cond); out->if_cond = NULL; return -1; }
            if (read_program(r, &out->else_body) != 0) {
                cond_free(out->if_cond); out->if_cond = NULL;
                program_free(&out->if_body);
                return -1;
            }
            return 0;
        case STMT_RECIPE: {
            uint32_t np, k;
            if (reader_bytes_alloc(r, &out->recipe_name, NULL) != 0) return -1;
            if (reader_u32(r, &np) != 0 || np > COB_MAX_EXTERN_ARGS) { free(out->recipe_name); out->recipe_name = NULL; return -1; }
            if (np > 0) {
                out->recipe_params = (char **)calloc(np, sizeof(char *));
                if (!out->recipe_params) { free(out->recipe_name); out->recipe_name = NULL; return -1; }
            }
            for (k = 0; k < np; k++) {
                if (reader_bytes_alloc(r, &out->recipe_params[k], NULL) != 0) {
                    out->recipe_nparams = (int)k;
                    stmt_free_contents(out);
                    memset(out, 0, sizeof(*out));
                    return -1;
                }
            }
            out->recipe_nparams = (int)np;
            if (read_program(r, &out->recipe_body) != 0) {
                program_init(&out->recipe_body);
                stmt_free_contents(out);
                memset(out, 0, sizeof(*out));
                return -1;
            }
            return 0;
        }
        case STMT_SERVE: return read_expr(r, &out->serve_expr);
        case STMT_CALL: return read_expr(r, &out->call_expr);
        default: return -1;
    }
}
static int read_program(ByteReader *r, Program *out) {
    uint32_t count, i;
    program_init(out);
    if (reader_u32(r, &count) != 0) return -1;
    for (i = 0; i < count; i++) {
        Stmt s;
        if (read_stmt(r, &s) != 0) { program_free(out); return -1; }
        if (program_push(out, s) != 0) { stmt_free_contents(&s); program_free(out); return -1; }
    }
    return 0;
}

static int cob_load_strawberry_cache(const char *cache_path, Program *out_prog) {
    CobFileBuffer file;
    if (cob_file_read_all(cache_path, &file) != COB_FILE_OK) return -1;

    ByteReader r; r.data = (const unsigned char *)file.data; r.len = file.size; r.pos = 0;

    if (r.len < STRAWBERRY_MAGIC_LEN || memcmp(r.data, STRAWBERRY_MAGIC, STRAWBERRY_MAGIC_LEN) != 0) {
        cob_file_free(&file);
        return -1;
    }
    r.pos = STRAWBERRY_MAGIC_LEN;

    int rc = read_program(&r, out_prog);
    cob_file_free(&file);
    return rc;
}

/* ---------------------------------------------------------------------
 * MAIN
 * ------------------------------------------------------------------- */
static void print_usage(const char *argv0) {
    fprintf(stderr,
        "Usage: %s [file.cob] [--no-cache] [--no-gc] [-i]\n"
        "       %s --license\n"
        "       %s --version\n"
        "  (no file)    start the interactive REPL\n"
        "  -i, --repl   run the file (if any), then stay in the REPL with its state\n"
        "  --no-cache   ignore/skip the .strawberry fast-boot cache\n"
        "  --no-gc      unlock harvest(<bytes>) / trash(<variable>)\n"
        "  --license    print the short license notice and exit\n"
        "\n"
        "If a .cob file's first line is exactly `_MakeCache = False`\n"
        "(whitespace/case-insensitive), the .strawberry cache is\n"
        "disabled for that file automatically.\n"
        "\n"
        "Console input, text, time, files, SQLite and windows are in the\n"
        "standard library: add e.g. `shuck porch` to a program to use them.\n",
        argv0, argv0, argv0);
}

/* ---------------------------------------------------------------------
 * REPL (v0.0.9) -- `cob_interp` with no file, or `-i` after a file
 *
 * Reads Cob a line at a time. A line ending in ':' starts a block: keep
 * typing indented lines and finish with a blank line. A bare expression
 * (1 + 2, name, greet("Ada")) prints its value, like Python; a recipe call
 * that ends without `serve` prints nothing. Variables and recipes persist
 * from one input to the next, and typing a recipe again replaces it.
 * Commands start with ':' -- try :help. Prompts and the banner only show
 * on a real terminal, so `cob_interp --repl < script.txt` is scriptable.
 * Not part of the browser (COB_WASM) build.
 * ------------------------------------------------------------------- */
#if !defined(COB_WASM)
static Program *g_repl_kept = NULL;
static size_t g_repl_kept_n = 0, g_repl_kept_cap = 0;

/* Programs that define recipes must outlive their input: the recipe table
 * points into them. Everything else is freed right after it runs. */
static int repl_keep(Program *p) {
    if (g_repl_kept_n == g_repl_kept_cap) {
        size_t new_cap = g_repl_kept_cap ? g_repl_kept_cap * 2 : 8;
        Program *grown = (Program *)realloc(g_repl_kept, new_cap * sizeof(Program));
        if (!grown) return -1;
        g_repl_kept = grown; g_repl_kept_cap = new_cap;
    }
    g_repl_kept[g_repl_kept_n++] = *p;
    program_init(p);
    return 0;
}
static void repl_free_kept(void) {
    size_t i;
    for (i = 0; i < g_repl_kept_n; i++) program_free(&g_repl_kept[i]);
    free(g_repl_kept);
    g_repl_kept = NULL; g_repl_kept_n = g_repl_kept_cap = 0;
}

/* numbers plain, strings in quotes (so "5" and 5 look different) */
static void repl_print_value(const Value *v) {
    if (v->kind == VAL_STR) {
        const char *c;
        putchar('"');
        for (c = v->s; *c; c++) {
            switch (*c) {
                case '"':  fputs("\\\"", stdout); break;
                case '\\': fputs("\\\\", stdout); break;
                case '\n': fputs("\\n", stdout); break;
                case '\t': fputs("\\t", stdout); break;
                default: putchar(*c);
            }
        }
        putchar('"');
    } else {
        printf("%ld", v->i);
    }
    putchar('\n');
}

static int repl_is_statement_line(const char *t) {
    static const char *const kws[] = {
        COB_KW_POP, COB_KW_SET, COB_KW_WHILE, COB_KW_IF, COB_KW_ELIF, COB_KW_ELSE,
        COB_KW_SHUCK, COB_KW_RECIPE, COB_KW_SERVE, COB_KW_EXTERN, COB_KW_TRASH
    };
    size_t i;
    for (i = 0; i < sizeof(kws) / sizeof(kws[0]); i++) {
        size_t n = strlen(kws[i]);
        if (strncmp(t, kws[i], n) == 0 && !is_ident_char(t[n])) return 1;
    }
    return 0;
}
/* `x = 5` -- a common slip; Cob wants `set x = 5` */
static int repl_looks_like_assignment(const char *t) {
    const char *q = t;
    if (!is_ident_start(*q)) return 0;
    while (is_ident_char(*q)) q++;
    q = skip_blank(q);
    return (*q == '=' && q[1] != '=');
}

/* Registers + runs an already-parsed program, then keeps or frees it. */
static void repl_run_program(EvalCtx *ctx, Program *prog, int lenient) {
    if (cob_recipes_validate(prog, 1) != 0 || cob_check_calls(prog, lenient) != 0) { program_free(prog); return; }
    if (!ctx->no_gc && program_uses_harvest_or_trash(prog)) {
        fprintf(stderr, "[cob_interp] error: harvest()/trash() need the REPL started with --no-gc\n");
        program_free(prog);
        return;
    }
    if (cob_recipes_register(prog) != 0) { program_free(prog); return; }
    execute(prog, ctx);
    value_free(&ctx->ret);
    ctx->ret = value_int(0);
    if (program_has_recipe(prog)) {
        if (repl_keep(prog) != 0) fprintf(stderr, "[cob_interp] out of memory keeping recipes\n");
    } else {
        program_free(prog);
    }
}

static void repl_eval_text(EvalCtx *ctx, const char *src) {
    const char *t = skip_blank(src);
    int multiline = (strchr(t, '\n') != NULL && strchr(t, '\n')[1] != '\0');

    /* 1. a lone expression prints its value */
    if (!multiline && !repl_is_statement_line(t)) {
        Expr *e = NULL;
        const char *p;
        g_parse_line = 0; /* REPL input has no line numbers to report */
        p = parse_expr(t, &e);
        if (p) {
            p = skip_blank(p);
            if (*p == '\0' || *p == '\n') {
                Value v;
                int is_call = (e->kind == EXPR_CALL);
                if (cob_check_calls_expr(e, 0) != 0) { expr_free(e); return; }
                if (!ctx->no_gc && expr_uses_harvest(e)) {
                    fprintf(stderr, "[cob_interp] error: harvest() needs the REPL started with --no-gc\n");
                    expr_free(e);
                    return;
                }
                ctx->fell_off = 0;
                v = eval_expr(e, ctx);
                if (!(is_call && ctx->fell_off)) repl_print_value(&v);
                value_free(&v);
                expr_free(e);
                return;
            }
            expr_free(e);
        }
    }
    /* 2. otherwise one or more statements */
    if (!multiline && repl_looks_like_assignment(t)) {
        size_t tl = strlen(t);
        while (tl > 0 && (t[tl - 1] == '\n' || t[tl - 1] == '\r')) tl--;
        fprintf(stderr, "[cob_interp] syntax error: unrecognized statement: %.*s\n"
                        "  hint: assign with `set`, e.g. `set x = 5`\n", (int)tl, t);
        return;
    }
    {
        PhysLines lines;
        Program prog;
        size_t pos = 0;
        int rc;
        if (cob_split_lines(src, 1, &lines) != 0) return;
        g_recipe_depth = 0;
        rc = parse_block(&lines, &pos, -1, &prog);
        physlines_free(&lines);
        if (rc != 0) return;
        repl_run_program(ctx, &prog, 1);
    }
}

static void repl_help(void) {
    puts("Type Cob at the prompt. A line ending in ':' starts a block -- finish it with a blank line.");
    puts("A bare expression prints its value:   1 + 2    name    greet(\"Ada\")");
    puts("Commands:");
    puts("  :help             this text");
    puts("  :vars             the variables you have set");
    puts("  :recipes          the recipes you have defined (also :funcs)");
    puts("  :load <file.cob>  run a file inside this session");
    puts("  :reset            forget all variables, recipes and loaded libraries");
    puts("  :quit             leave (or press Ctrl-D)");
    puts("Libraries: shuck porch | cornmath | kernels | almanac | granary | pantry | barn | tassel | silo");
}

/* Returns 1 to leave the REPL. */
static int repl_command(EvalCtx *ctx, const char *cmd) {
    const char *arg = cmd;
    while (*arg && *arg != ' ' && *arg != '\t') arg++;
    arg = skip_blank(arg);
    {
        size_t n = (size_t)(arg - cmd);
        while (n > 0 && (cmd[n - 1] == ' ' || cmd[n - 1] == '\t')) n--;
#define REPL_IS(word) (n == strlen(word) && strncmp(cmd, word, n) == 0)
        if (REPL_IS(":quit") || REPL_IS(":q") || REPL_IS(":exit")) return 1;
        if (REPL_IS(":help") || REPL_IS(":h") || REPL_IS(":?")) { repl_help(); return 0; }
        if (REPL_IS(":vars")) {
            size_t i;
            if (ctx->vars->count == 0) puts("(no variables yet)");
            for (i = 0; i < ctx->vars->count; i++) {
                printf("%s = ", ctx->vars->items[i].name);
                repl_print_value(&ctx->vars->items[i].value);
            }
            return 0;
        }
        if (REPL_IS(":recipes") || REPL_IS(":funcs")) {
            size_t i;
            int k;
            if (g_func_count == 0) puts("(no recipes yet)");
            for (i = 0; i < g_func_count; i++) {
                const Stmt *d = g_funcs[i].def;
                printf("recipe %s(", d->recipe_name);
                for (k = 0; k < d->recipe_nparams; k++) printf("%s%s", k ? ", " : "", d->recipe_params[k]);
                puts(")");
            }
            return 0;
        }
        if (REPL_IS(":reset")) {
            vars_free(ctx->vars); vars_init(ctx->vars);
            handles_free_all(ctx->handles); handles_init(ctx->handles);
            funcs_free();
            cob_extern_shutdown();
            repl_free_kept();
            puts("(reset)");
            return 0;
        }
        if (REPL_IS(":load")) {
            CobFileBuffer buf;
            Program prog;
            int disable = 0;
            if (*arg == '\0') { fprintf(stderr, "[cob_interp] usage: :load <file.cob>\n"); return 0; }
            if (cob_file_read_all(arg, &buf) != COB_FILE_OK) {
                fprintf(stderr, "[cob_interp] error: could not read '%s'\n", arg);
                return 0;
            }
            g_recipe_depth = 0;
            if (cob_parse_source(buf.data, &prog, &disable) == 0) repl_run_program(ctx, &prog, 0);
            cob_file_free(&buf);
            return 0;
        }
#undef REPL_IS
    }
    fprintf(stderr, "[cob_interp] unknown command '%s' -- try :help\n", cmd);
    return 0;
}

static int cob_repl(EvalCtx *ctx, int banner) {
    int interactive = COB_STDIN_IS_TTY();
    if (interactive && banner) {
        printf("Cob %s REPL -- :help for help, :quit or Ctrl-D to leave\n", COB_VERSION_STRING);
    }
    for (;;) {
        int got = 0;
        char *line = cob_read_line(interactive ? "cob> " : "", &got);
        const char *t;
        size_t len;
        if (!line) { fprintf(stderr, "[cob_interp] out of memory\n"); break; }
        if (!got) { free(line); if (interactive) putchar('\n'); break; }
        t = skip_blank(line);
        if (*t == '\0') { free(line); continue; }
        if (*t == ':') {
            int quit = repl_command(ctx, t);
            free(line);
            fflush(stdout);
            if (quit) break;
            continue;
        }
        len = strlen(t);
        while (len > 0 && (t[len - 1] == ' ' || t[len - 1] == '\t')) len--;
        {
            int starts_block = (len > 0 && t[len - 1] == ':'); /* read before `line` is freed */
            size_t cap = strlen(line) + 2, used;
            char *text = (char *)malloc(cap);
            if (!text) { free(line); break; }
            used = (size_t)snprintf(text, cap, "%s\n", line);
            free(line);
            if (starts_block) {
                /* a block: read indented lines until a blank line */
                for (;;) {
                    int more = 0;
                    char *next = cob_read_line(interactive ? "...  " : "", &more);
                    char *grown;
                    size_t need;
                    if (!next) break;
                    if (!more || *skip_blank(next) == '\0') { free(next); break; }
                    need = used + strlen(next) + 2;
                    grown = (char *)realloc(text, need);
                    if (!grown) { free(next); break; }
                    text = grown;
                    used += (size_t)snprintf(text + used, need - used, "%s\n", next);
                    free(next);
                }
            }
            repl_eval_text(ctx, text);
            free(text);
        }
        fflush(stdout);
        fflush(stderr);
    }
    repl_free_kept();
    return 0;
}
#endif /* !COB_WASM */

int main(int argc, char **argv) {
    const char *src_path = NULL;
    int use_cache = 1;
    int no_gc = 0;
    int enter_repl = 0;
    char cache_path[1024];
    Program prog;
    EvalCtx ctx;
    VarTable vars;
    HandleTable handles;
    int i, exit_code = 0;

    if (cob_check_hidden_version_flag(argc, argv)) return 0;
    if (cob_check_hidden_license_flag(argc, argv)) return 0;
#ifdef COB_ENTERPRISE
    if (cob_lic_require("cob_interp") != 0) return 1;
#endif
#if defined(COB_WASM)
    if (argc < 2) { print_usage(argv[0]); return 1; }
#endif

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-cache") == 0) {
            use_cache = 0;
        } else if (strcmp(argv[i], COB_FLAG_NO_GC) == 0) {
            no_gc = 1;
#if !defined(COB_WASM)
        } else if (strcmp(argv[i], "--repl") == 0 || strcmp(argv[i], "-i") == 0 ||
                   strcmp(argv[i], "--interactive") == 0) {
            enter_repl = 1;
#endif
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "[cob_interp] error: unrecognized flag '%s'\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        } else if (src_path == NULL) {
            src_path = argv[i];
        } else {
            fprintf(stderr, "[cob_interp] error: unexpected extra argument '%s'\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    program_init(&prog);
    vars_init(&vars);
    handles_init(&handles);
    memset(&ctx, 0, sizeof(ctx));
    ctx.vars = &vars; ctx.handles = &handles; ctx.no_gc = no_gc;
    ctx.ret = value_int(0);

    if (src_path == NULL) {
#if !defined(COB_WASM)
        cob_repl(&ctx, 1);
        vars_free(&vars); handles_free_all(&handles);
        cob_ext_shutdown();
        return 0;
#else
        fprintf(stderr, "[cob_interp] error: no source file given\n");
        print_usage(argv[0]);
        return 1;
#endif
    }
    if (!cob_str_ends_with(src_path, COB_SOURCE_EXTENSION)) {
        fprintf(stderr, "[cob_interp] error: expected a %s file, got '%s'\n", COB_SOURCE_EXTENSION, src_path);
        return 1;
    }
    if (cob_make_cache_path(src_path, cache_path, sizeof(cache_path)) != 0) {
        fprintf(stderr, "[cob_interp] error: source path too long to derive cache path\n");
        return 1;
    }

    CobFileBuffer source;
    if (cob_file_read_all(src_path, &source) != COB_FILE_OK) {
        fprintf(stderr, "[cob_interp] error: could not read source file '%s'\n", src_path);
        return 1;
    }

    int disable_cache_directive;
    {
        const char *peek = source.data;
        disable_cache_directive = cob_strip_leading_makecache_directive(&peek);
    }
    if (disable_cache_directive) use_cache = 0;
    if (enter_repl) use_cache = 0; /* the REPL needs the live program, not a cached copy */

    if (use_cache && cob_file_exists(cache_path)) {
        if (cob_load_strawberry_cache(cache_path, &prog) == 0) {
            cob_file_free(&source);
            if (!no_gc && program_uses_harvest_or_trash(&prog)) {
                fprintf(stderr, "[cob_interp] error: this program uses harvest()/trash() -- rerun with --no-gc\n");
                program_free(&prog); vars_free(&vars); handles_free_all(&handles);
                cob_ext_shutdown();
                return 1;
            }
            if (cob_recipes_register(&prog) != 0) {
                program_free(&prog); vars_free(&vars); handles_free_all(&handles);
                cob_ext_shutdown();
                return 1;
            }
            execute(&prog, &ctx);
            value_free(&ctx.ret);
            program_free(&prog); vars_free(&vars); handles_free_all(&handles);
            cob_ext_shutdown();
            return 0;
        }
        fprintf(stderr, "[cob_interp] note: cache '%s' unreadable, reparsing source\n", cache_path);
        program_init(&prog);
    }

    if (cob_parse_source(source.data, &prog, &disable_cache_directive) != 0) {
        cob_file_free(&source);
        vars_free(&vars); handles_free_all(&handles);
        cob_ext_shutdown();
        return 1;
    }
    cob_file_free(&source);

    /* recipes are hoisted: validate and register them, and check every
     * call, BEFORE anything runs -- so a typo fails up front, not halfway */
    if (cob_recipes_validate(&prog, 0) != 0 || cob_check_calls(&prog, 0) != 0 ||
        cob_recipes_register(&prog) != 0) {
        program_free(&prog); vars_free(&vars); handles_free_all(&handles);
        cob_ext_shutdown();
        return 1;
    }

    if (!no_gc && program_uses_harvest_or_trash(&prog)) {
        fprintf(stderr, "[cob_interp] error: this program uses harvest()/trash() -- rerun with --no-gc\n");
        program_free(&prog); vars_free(&vars); handles_free_all(&handles);
        cob_ext_shutdown();
        return 1;
    }

    execute(&prog, &ctx);
    value_free(&ctx.ret);
    ctx.ret = value_int(0);

    if (use_cache) cob_write_strawberry_cache(cache_path, &prog);

#if !defined(COB_WASM)
    if (enter_repl) cob_repl(&ctx, 1);
#endif

    program_free(&prog);
    vars_free(&vars);
    handles_free_all(&handles);
    cob_ext_shutdown();
    return exit_code;
}

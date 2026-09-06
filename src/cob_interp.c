/* =====================================================================
 * cob_interp.c
 * ---------------------------------------------------------------------
 * Project Obsidian Falcon / Cob Language Toolchain
 * The Cob interpreter binary: `cob_interp <file.cob> [--no-cache] [--no-gc]`
 *
 * ---------------------------------------------------------------------
 * v0.0.3-bug-fix-3 SCOPE -- fully working:
 * ---------------------------------------------------------------------
 *   pop(<expr>)               writes text to stdout + newline. Accepts
 *                             any expression, not just a literal --
 *                             pop(x) prints an int variable's decimal
 *                             value or a string variable's contents.
 *                             String literals use \\ \" \n \t escapes.
 *   set <name> = <expr>       integer OR string variables -- a
 *                             variable's "type" is just whatever it
 *                             was last set to; there's no declaration.
 *   "text" + "text"           string concatenation via '+'. If either
 *                             side of '+' is a string, the other side
 *                             is stringified (an int becomes its
 *                             decimal digits) and both are joined.
 *                             '-' '*' '/' still require two numbers.
 *   while <condition>:        real looping, real nested block bodies.
 *                             ==/!= /</></<=/>= work on two strings
 *                             (strcmp-based) or two numbers; comparing
 *                             a string to a number is a warning and
 *                             evaluates to false rather than guessing.
 *   shuck <library_name>      loads <library_name>.cob (or
 *                             cob_modules/<library_name>/<library_name>.cob,
 *                             matching farmer's install layout) and
 *                             splices its statements in at that point,
 *                             like a textual include. Cyclic/too-deep
 *                             shucks are rejected with a clear error.
 *   harvest(<bytes>)          ONLY when --no-gc is passed: an expression
 *                             that allocates <bytes> raw memory and
 *                             returns an opaque handle (a plain integer,
 *                             not a real pointer -- Cob code can't do
 *                             pointer arithmetic with it, only pass it
 *                             to trash()).
 *   trash(<variable>)         ONLY when --no-gc is passed: frees the
 *                             handle held in <variable>. Double-free
 *                             and use of an unknown handle are reported
 *                             as warnings, not crashes.
 *   _MakeCache = False        as the literal first line of a .cob file
 *                             disables the .strawberry cache for that
 *                             file entirely (neither read nor written).
 *
 * NOTE ON popcorn_comp: string support above is cob_interp-only so far.
 * popcorn_comp has its own separate copy of this AST and its own C
 * code generator, which still assumes every Cob variable is a `long`.
 * A .cob program using strings runs correctly under cob_interp but is
 * not yet compilable to a native binary -- that needs popcorn_comp's
 * codegen to grow a tagged value representation too, which hasn't
 * been done yet. The .strawberry cache format bumped to v3 alongside
 * this change specifically so an old-format reader (like today's
 * popcorn_comp) fails cleanly on a new-format cache instead of
 * misreading it.
 *
 * If a program uses harvest()/trash() but --no-gc was not passed on the
 * command line, cob_interp refuses to run it with a clear error instead
 * of silently ignoring the memory keywords -- "unlock" is a real gate,
 * not just documentation.
 *
 * expr grammar:  expr := term (('+'|'-') term)*
 *                term := primary (('*'|'/') primary)*
 *                primary := NUMBER | STRING | IDENT
 *                           | 'harvest' '(' expr ')'
 * cond grammar:  expr (('=='|'!='|'<'|'>'|'<='|'>=') expr)?  (bare expr
 *                is a truthiness/nonzero test; for a string, truthy
 *                means non-empty, matching non-zero for numbers)
 *
 * SAFETY: a `while` loop is capped at COB_MAX_LOOP_ITERATIONS so a
 * buggy/infinite .cob program can't hang the interpreter forever.
 *
 * License: PolyForm Noncommercial License 1.0.0 - see LICENSE.md
 * ===================================================================== */

#include "common.h"
#include "file_io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>

#define COB_MAX_LOOP_ITERATIONS 10000000UL
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
typedef enum { EXPR_NUM, EXPR_STR, EXPR_VAR, EXPR_BINOP, EXPR_HARVEST } ExprKind;

typedef struct Expr {
    ExprKind kind;
    long num;             /* EXPR_NUM */
    char *str_lit; size_t str_lit_len; /* EXPR_STR, owned */
    char *var;            /* EXPR_VAR, owned */
    char op;              /* EXPR_BINOP: '+' '-' '*' '/' */
    struct Expr *left;    /* EXPR_BINOP left; EXPR_HARVEST byte-count child */
    struct Expr *right;   /* EXPR_BINOP right */
} Expr;

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
static void expr_free(Expr *e) {
    if (!e) return;
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
typedef enum { STMT_POP, STMT_SET, STMT_WHILE, STMT_TRASH } StmtKind;

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
        case STMT_WHILE: {
            cond_free(s->while_cond);
            size_t i;
            for (i = 0; i < s->while_body.count; i++) stmt_free_contents(&s->while_body.items[i]);
            free(s->while_body.items);
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
 * EVALUATION
 * ------------------------------------------------------------------- */
/* Renders an int as decimal text for string concatenation (e.g.
 * "score: " + 7). Buffer is comfortably large for any long. */
static void long_to_str(long n, char *buf, size_t bufsize) {
    snprintf(buf, bufsize, "%ld", n);
}

static Value eval_expr(const Expr *e, EvalCtx *ctx) {
    switch (e->kind) {
        case EXPR_NUM: return value_int(e->num);
        case EXPR_STR: return value_str_take(cob_strdup(e->str_lit));
        case EXPR_VAR: {
            Value v;
            if (vars_get(ctx->vars, e->var, &v)) return v;
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
    }
    return value_int(0);
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

static void execute(const Program *prog, EvalCtx *ctx) {
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
                value_free(&v);
                break;
            }
            case STMT_SET: {
                Value v = eval_expr(s->set_expr, ctx);
                if (vars_set(ctx->vars, s->set_name, v) != 0) {
                    fprintf(stderr, "[cob_interp] out of memory setting variable '%s'\n", s->set_name);
                    value_free(&v); /* vars_set didn't take ownership on this failure path */
                }
                break;
            }
            case STMT_TRASH: {
                Value v;
                if (!vars_get(ctx->vars, s->trash_name, &v)) {
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
            case STMT_WHILE: {
                unsigned long iterations = 0;
                while (eval_cond(s->while_cond, ctx)) {
                    execute(&s->while_body, ctx);
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

/* Loads and fully parses <name>.cob (trying the current directory, then
 * cob_modules/<name>/<name>.cob -- the layout `farmer harvest` installs
 * packages into), returning its top-level statements in *out_prog. */
static int cob_load_library(const char *name, int line_no, Program *out_prog) {
    char path_a[512], path_b[600];
    CobFileBuffer buf;
    CobFileStatus status;
    int i;

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

    status = cob_file_read_all(path_a, &buf);
    if (status != COB_FILE_OK) status = cob_file_read_all(path_b, &buf);
    if (status != COB_FILE_OK) {
        fprintf(stderr,
            "[cob_interp] error at line %d: shuck '%s' failed -- tried '%s' and '%s'\n",
            line_no, name, path_a, path_b);
        return -1;
    }

    g_shuck_stack[g_shuck_depth++] = name;

    PhysLines lib_lines;
    int rc = cob_split_lines(buf.data, 1, &lib_lines);
    cob_file_free(&buf);
    if (rc != 0) { g_shuck_depth--; return -1; }

    size_t pos = 0;
    rc = parse_block(&lib_lines, &pos, -1, out_prog);
    physlines_free(&lib_lines);

    g_shuck_depth--;
    return rc;
}

/* ---------------------------------------------------------------------
 * RECURSIVE BLOCK PARSER
 * ------------------------------------------------------------------- */
static int parse_block(PhysLines *lines, size_t *pos, int parent_indent, Program *out_prog) {
    program_init(out_prog);

    while (*pos < lines->count) {
        PhysLine *pl = &lines->items[*pos];
        if (pl->indent <= parent_indent) break;

        const char *trimmed = pl->content;
        int line_no = pl->line_no;
        size_t kw_len;

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
 * .strawberry CACHE (binary AST dump, format v2)
 * ------------------------------------------------------------------- */
#define STRAWBERRY_MAGIC      "COBSTRW3"
#define STRAWBERRY_MAGIC_LEN  8

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
        case STMT_WHILE:
            if (write_cond(b, s->while_cond) != 0) return -1;
            return write_program(b, &s->while_body);
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
        "Usage: %s <file.cob> [--no-cache] [--no-gc]\n"
        "  --no-cache   ignore/skip the .strawberry fast-boot cache\n"
        "  --no-gc      unlock harvest(<bytes>) / trash(<variable>)\n"
        "\n"
        "If a .cob file's first line is exactly `_MakeCache = False`\n"
        "(whitespace/case-insensitive), the .strawberry cache is\n"
        "disabled for that file automatically.\n",
        argv0);
}

int main(int argc, char **argv) {
    const char *src_path = NULL;
    int use_cache = 1;
    int no_gc = 0;
    char cache_path[1024];
    Program prog;
    EvalCtx ctx;
    VarTable vars;
    HandleTable handles;
    int i;

    if (cob_check_hidden_version_flag(argc, argv)) return 0;
    if (argc < 2) { print_usage(argv[0]); return 1; }

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-cache") == 0) {
            use_cache = 0;
        } else if (strcmp(argv[i], COB_FLAG_NO_GC) == 0) {
            no_gc = 1;
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
    if (src_path == NULL) {
        fprintf(stderr, "[cob_interp] error: no source file given\n");
        print_usage(argv[0]);
        return 1;
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

    program_init(&prog);
    vars_init(&vars);
    handles_init(&handles);
    ctx.vars = &vars; ctx.handles = &handles; ctx.no_gc = no_gc;

    if (use_cache && cob_file_exists(cache_path)) {
        if (cob_load_strawberry_cache(cache_path, &prog) == 0) {
            cob_file_free(&source);
            if (!no_gc && program_uses_harvest_or_trash(&prog)) {
                fprintf(stderr, "[cob_interp] error: this program uses harvest()/trash() -- rerun with --no-gc\n");
                program_free(&prog); vars_free(&vars); handles_free_all(&handles);
                return 1;
            }
            execute(&prog, &ctx);
            program_free(&prog); vars_free(&vars); handles_free_all(&handles);
            return 0;
        }
        fprintf(stderr, "[cob_interp] note: cache '%s' unreadable, reparsing source\n", cache_path);
        program_init(&prog);
    }

    if (cob_parse_source(source.data, &prog, &disable_cache_directive) != 0) {
        cob_file_free(&source);
        vars_free(&vars); handles_free_all(&handles);
        return 1;
    }
    cob_file_free(&source);

    if (!no_gc && program_uses_harvest_or_trash(&prog)) {
        fprintf(stderr, "[cob_interp] error: this program uses harvest()/trash() -- rerun with --no-gc\n");
        program_free(&prog); vars_free(&vars); handles_free_all(&handles);
        return 1;
    }

    execute(&prog, &ctx);

    if (use_cache) cob_write_strawberry_cache(cache_path, &prog);

    program_free(&prog);
    vars_free(&vars);
    handles_free_all(&handles);
    return 0;
}

//! The small C runtime embedded verbatim at the top of every generated
//! program. Cob variables hold an int or a string, but generated C needs one
//! concrete type per variable, so every variable is a `CobValue` (a tagged
//! struct) and every operator is a call into these helpers. They mirror the
//! interpreter's eval semantics on purpose -- same results, same warnings
//! (tagged "[cob]" instead of "[cob_interp]"), same fallback-to-0 rules --
//! so a program behaves the same compiled as interpreted.

/// Value type and operators. Needed by every program.
pub const PREAMBLE: &str = r#"typedef struct { int is_str; long i; char *s; } CobValue;
static char *cob_strdup_(const char *s) { size_t n = strlen(s) + 1; char *p = (char *)malloc(n); if (p) memcpy(p, s, n); return p; }
static CobValue cob_int(long n) { CobValue v; v.is_str = 0; v.i = n; v.s = NULL; return v; }
static CobValue cob_str(const char *s) { CobValue v; v.is_str = 1; v.i = 0; v.s = cob_strdup_(s); return v; }
static CobValue cob_str_take(char *s) { CobValue v; v.is_str = 1; v.i = 0; v.s = s; return v; }
static void cob_free(CobValue *v) { if (v->is_str) { free(v->s); v->s = NULL; } }
static CobValue cob_copy(const CobValue *v) { return v->is_str ? cob_str(v->s ? v->s : "") : cob_int(v->i); }
static void cob_long_to_str(long n, char *buf, size_t bufsize) { snprintf(buf, bufsize, "%ld", n); }
static CobValue cob_add(CobValue l, CobValue r) {
    if (l.is_str || r.is_str) {
        char lbuf[32], rbuf[32];
        const char *ls = l.is_str ? l.s : (cob_long_to_str(l.i, lbuf, sizeof(lbuf)), lbuf);
        const char *rs = r.is_str ? r.s : (cob_long_to_str(r.i, rbuf, sizeof(rbuf)), rbuf);
        size_t ln = strlen(ls), rn = strlen(rs);
        char *cat = (char *)malloc(ln + rn + 1);
        CobValue result;
        if (cat) { memcpy(cat, ls, ln); memcpy(cat + ln, rs, rn); cat[ln + rn] = '\0'; result = cob_str_take(cat); }
        else result = cob_int(0);
        cob_free(&l); cob_free(&r);
        return result;
    }
    return cob_int(l.i + r.i);
}
static CobValue cob_sub(CobValue l, CobValue r) {
    if (l.is_str || r.is_str) { fprintf(stderr, "[cob] warning: operator '-' requires two numbers, got a string; result treated as 0\n"); cob_free(&l); cob_free(&r); return cob_int(0); }
    return cob_int(l.i - r.i);
}
static CobValue cob_mul(CobValue l, CobValue r) {
    if (l.is_str || r.is_str) { fprintf(stderr, "[cob] warning: operator '*' requires two numbers, got a string; result treated as 0\n"); cob_free(&l); cob_free(&r); return cob_int(0); }
    return cob_int(l.i * r.i);
}
static CobValue cob_divop(CobValue l, CobValue r) {
    if (l.is_str || r.is_str) { fprintf(stderr, "[cob] warning: operator '/' requires two numbers, got a string; result treated as 0\n"); cob_free(&l); cob_free(&r); return cob_int(0); }
    if (r.i == 0) { fprintf(stderr, "[cob] warning: division by zero, result treated as 0\n"); return cob_int(0); }
    return cob_int(l.i / r.i);
}
static long cob_to_long_for_harvest(CobValue v) {
    long n;
    if (v.is_str) { fprintf(stderr, "[cob] warning: harvest() needs a number, got a string; treated as 0\n"); n = 0; cob_free(&v); }
    else n = v.i;
    return n;
}
static long cob_harvest_bytes(CobValue v) {
    long bytes = cob_to_long_for_harvest(v);
    if (bytes < 0) { fprintf(stderr, "[cob] warning: harvest() with negative size, treated as 0\n"); bytes = 0; }
    return bytes;
}
static int cob_cmp_raw(CobValue l, CobValue r, int *mismatch) {
    int result;
    *mismatch = 0;
    if (l.is_str != r.is_str) { *mismatch = 1; result = 0; }
    else if (l.is_str) result = strcmp(l.s, r.s);
    else result = (l.i > r.i) - (l.i < r.i);
    cob_free(&l); cob_free(&r);
    return result;
}
static int cob_mismatch_warn(void) { fprintf(stderr, "[cob] warning: comparing a number to a string; result is false\n"); return 0; }
static int cob_eq(CobValue l, CobValue r) { int m; int c = cob_cmp_raw(l, r, &m); return m ? cob_mismatch_warn() : (c == 0); }
static int cob_ne(CobValue l, CobValue r) { int m; int c = cob_cmp_raw(l, r, &m); return m ? cob_mismatch_warn() : (c != 0); }
static int cob_lt(CobValue l, CobValue r) { int m; int c = cob_cmp_raw(l, r, &m); return m ? cob_mismatch_warn() : (c < 0); }
static int cob_gt(CobValue l, CobValue r) { int m; int c = cob_cmp_raw(l, r, &m); return m ? cob_mismatch_warn() : (c > 0); }
static int cob_le(CobValue l, CobValue r) { int m; int c = cob_cmp_raw(l, r, &m); return m ? cob_mismatch_warn() : (c <= 0); }
static int cob_ge(CobValue l, CobValue r) { int m; int c = cob_cmp_raw(l, r, &m); return m ? cob_mismatch_warn() : (c >= 0); }
static int cob_truthy(CobValue v) { int t = v.is_str ? (v.s != NULL && v.s[0] != '\0') : (v.i != 0); cob_free(&v); return t; }
static void cob_pop(CobValue v) {
    if (v.is_str) { fwrite(v.s, 1, strlen(v.s), stdout); }
    else { char buf[32]; int n = snprintf(buf, sizeof(buf), "%ld", v.i); if (n > 0) fwrite(buf, 1, (size_t)n, stdout); }
    fputc('\n', stdout);
    cob_free(&v);
}
"#;

/// The argument stack: carries recipe-call arguments and parks the left operand
/// where Cob's left-to-right evaluation order matters (C leaves argument
/// evaluation order unspecified). Emitted for programs with recipes or externs.
pub const RECIPE_RUNTIME: &str = r#"static CobValue *cob_args = NULL;
static size_t cob_args_n = 0, cob_args_cap = 0;
static int cob_depth = 0;
static void cob_argpush(CobValue v) {
    if (cob_args_n == cob_args_cap) {
        size_t nc = cob_args_cap ? cob_args_cap * 2 : 64;
        CobValue *g = (CobValue *)realloc(cob_args, nc * sizeof(CobValue));
        if (!g) { fprintf(stderr, "[cob] out of memory\n"); exit(1); }
        cob_args = g; cob_args_cap = nc;
    }
    cob_args[cob_args_n++] = v;
}
static CobValue cob_argpop(void) { return cob_args[--cob_args_n]; }
static CobValue *cob_argtop(int n) { return &cob_args[cob_args_n - (size_t)n]; }
static CobValue cob_argdrop(CobValue r, int n) { cob_args_n -= (size_t)n; return r; }
"#;

/// A locally named twin of include/cob.h's `CobValue` (same tag + union
/// layout, so it is ABI-identical) -- the generated program already has its
/// own `CobValue`, and must not need Cob's include/ directory at all.
pub const FFI_PREAMBLE: &str = r#"typedef enum { COB_FFI_INT = 0, COB_FFI_STR = 1 } CobFFIValueKind;
typedef struct { CobFFIValueKind kind; union { long i; const char *s; } as; } CobFFIValue;
"#;

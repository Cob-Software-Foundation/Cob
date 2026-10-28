/* =====================================================================
 * cornmath -- numbers beyond + - * /          (shuck cornmath)
 * ---------------------------------------------------------------------
 *   husk_abs(n)            absolute value
 *   ear_max(a, b)          the larger of two numbers
 *   ear_min(a, b)          the smaller of two numbers
 *   stalk_pow(base, exp)   base to a whole-number power (exp < 0 gives 0)
 *   stalk_sqrt(n)          whole-number square root, rounded down
 *   silk_mod(a, b)         remainder, always >= 0 for a positive b
 *   scatter_seed(n)        seed the random numbers (repeatable runs)
 *   scatter_rand(lo, hi)   a random whole number from lo to hi, inclusive
 *
 * scatter_rand uses its own generator (splitmix64), so a seeded run gives
 * the same numbers on every platform.
 * ===================================================================== */
#include "stdhelp.h"
#include <time.h>

#define MOD "cornmath"

COB_EXPORT CobValue husk_abs(CobValue *argv, int argc) {
    long n;
    if (!cs_arity(MOD, "husk_abs", argc, 1)) return cob_int(0);
    n = cs_int(MOD, "husk_abs", argv, argc, 0);
    return cob_int(n < 0 ? -n : n);
}
COB_EXPORT CobValue ear_max(CobValue *argv, int argc) {
    long a, b;
    if (!cs_arity(MOD, "ear_max", argc, 2)) return cob_int(0);
    a = cs_int(MOD, "ear_max", argv, argc, 0); b = cs_int(MOD, "ear_max", argv, argc, 1);
    return cob_int(a > b ? a : b);
}
COB_EXPORT CobValue ear_min(CobValue *argv, int argc) {
    long a, b;
    if (!cs_arity(MOD, "ear_min", argc, 2)) return cob_int(0);
    a = cs_int(MOD, "ear_min", argv, argc, 0); b = cs_int(MOD, "ear_min", argv, argc, 1);
    return cob_int(a < b ? a : b);
}
COB_EXPORT CobValue stalk_pow(CobValue *argv, int argc) {
    long base, exp;
    unsigned long result = 1, b;
    if (!cs_arity(MOD, "stalk_pow", argc, 2)) return cob_int(0);
    base = cs_int(MOD, "stalk_pow", argv, argc, 0); exp = cs_int(MOD, "stalk_pow", argv, argc, 1);
    if (exp < 0) { fprintf(stderr, "[" MOD "] warning: stalk_pow() with a negative exponent gives 0\n"); return cob_int(0); }
    b = (unsigned long)base;
    while (exp > 0) { if (exp & 1) result *= b; b *= b; exp >>= 1; }
    return cob_int((long)result);
}
COB_EXPORT CobValue stalk_sqrt(CobValue *argv, int argc) {
    long n, lo = 0, hi;
    if (!cs_arity(MOD, "stalk_sqrt", argc, 1)) return cob_int(0);
    n = cs_int(MOD, "stalk_sqrt", argv, argc, 0);
    if (n < 0) { fprintf(stderr, "[" MOD "] warning: stalk_sqrt() of a negative number gives 0\n"); return cob_int(0); }
    hi = n < 3037000499L ? n : 3037000499L; /* floor(sqrt(2^63)) */
    while (lo < hi) {
        long mid = lo + (hi - lo + 1) / 2;
        if (mid <= n / mid) lo = mid; else hi = mid - 1;
    }
    return cob_int(lo);
}
COB_EXPORT CobValue silk_mod(CobValue *argv, int argc) {
    long a, b, m;
    if (!cs_arity(MOD, "silk_mod", argc, 2)) return cob_int(0);
    a = cs_int(MOD, "silk_mod", argv, argc, 0); b = cs_int(MOD, "silk_mod", argv, argc, 1);
    if (b == 0) { fprintf(stderr, "[" MOD "] warning: silk_mod() by zero gives 0\n"); return cob_int(0); }
    if (b == -1) return cob_int(0);
    m = a % b;
    if (m != 0 && ((m < 0) != (b < 0))) m += b;
    return cob_int(m);
}

static unsigned long long scatter_state = 0;
static int scatter_seeded = 0;
static unsigned long long scatter_next(void) {
    unsigned long long z;
    if (!scatter_seeded) { scatter_state = (unsigned long long)time(NULL) * 2654435761ULL; scatter_seeded = 1; }
    z = (scatter_state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}
COB_EXPORT CobValue scatter_seed(CobValue *argv, int argc) {
    if (!cs_arity(MOD, "scatter_seed", argc, 1)) return cob_int(0);
    scatter_state = (unsigned long long)cs_int(MOD, "scatter_seed", argv, argc, 0);
    scatter_seeded = 1;
    return cob_int(0);
}
COB_EXPORT CobValue scatter_rand(CobValue *argv, int argc) {
    long lo, hi;
    unsigned long long span;
    if (!cs_arity(MOD, "scatter_rand", argc, 2)) return cob_int(0);
    lo = cs_int(MOD, "scatter_rand", argv, argc, 0); hi = cs_int(MOD, "scatter_rand", argv, argc, 1);
    if (hi < lo) { long t = lo; lo = hi; hi = t; }
    span = (unsigned long long)hi - (unsigned long long)lo + 1ULL;
    if (span == 0) return cob_int((long)scatter_next());
    return cob_int((long)((unsigned long long)lo + scatter_next() % span));
}

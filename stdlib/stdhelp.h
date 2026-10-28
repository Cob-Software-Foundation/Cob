/* =====================================================================
 * stdhelp.h -- tiny helpers shared by the Cob standard library modules
 * ---------------------------------------------------------------------
 * Every stdlib module is an ordinary CEP-2 native extension (see
 * include/cob.h): exported functions with the one fixed signature
 *     CobValue fn(CobValue *argv, int argc)
 * built into a shared library (.so / .dylib / .dll) and bound by
 * `extern` declarations in a small <module>.cob wrapper, which is what
 * `shuck <module>` loads.
 *
 * Naming rule for the whole standard library: every module and every
 * exported function is corn themed (porch_ask, kernel_upper, pantry_open,
 * ...). Warnings go to stderr as "[<module>] warning: ...".
 *
 * License: PolyForm Noncommercial License 1.0.0 - see LICENSE.md
 * ===================================================================== */
#ifndef COB_STDHELP_H
#define COB_STDHELP_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cob.h"

/* Checks the argument count; prints a warning and returns 0 on mismatch. */
static inline int cs_arity(const char *mod, const char *fn, int argc, int want) {
    if (argc == want) return 1;
    fprintf(stderr, "[%s] warning: %s() takes %d argument%s, got %d\n",
            mod, fn, want, want == 1 ? "" : "s", argc);
    return 0;
}
/* i-th argument as a number / string; a wrong kind warns and gives 0 / "". */
static inline long cs_int(const char *mod, const char *fn, CobValue *argv, int argc, int i) {
    if (i < argc && argv[i].kind == COB_INT) return argv[i].as.i;
    fprintf(stderr, "[%s] warning: argument %d of %s() must be a number; treated as 0\n", mod, i + 1, fn);
    return 0;
}
static inline const char *cs_str(const char *mod, const char *fn, CobValue *argv, int argc, int i) {
    if (i < argc && argv[i].kind == COB_STR && argv[i].as.s) return argv[i].as.s;
    fprintf(stderr, "[%s] warning: argument %d of %s() must be a string; treated as \"\"\n", mod, i + 1, fn);
    return "";
}
/* Returns a string result. Cob copies it right after the call, so each
 * function keeps its own buffer ("slot") alive until its next call. */
static inline CobValue cs_ret_str(char **slot, const char *text, size_t n) {
    char *copy = (char *)malloc(n + 1);
    if (!copy) return cob_str("");
    memcpy(copy, text, n);
    copy[n] = '\0';
    free(*slot);
    *slot = copy;
    return cob_str(*slot);
}

/* Runs when the library is unloaded: modules use it to free the buffers
 * their string results live in, so a finished program leaves nothing behind. */
#if defined(__GNUC__) || defined(__clang__)
#  define CS_ON_UNLOAD(fn) __attribute__((destructor)) static void fn(void)
#else
#  define CS_ON_UNLOAD(fn) static void fn(void)  /* no automatic hook: buffers are freed by the OS at exit */
#endif

#endif /* COB_STDHELP_H */

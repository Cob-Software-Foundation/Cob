/* =====================================================================
 * cob.h -- the only header a native Cob extension needs   (CEP-2)
 * ---------------------------------------------------------------------
 * Project Obsidian Falcon / Cob Language Toolchain
 *
 * A Cob script binds to a native function with:
 *
 *     extern calculate_fast(x) from "math_ext.so"
 *     set result = calculate_fast(42)
 *
 * and the native side is an ordinary exported C function with the one
 * fixed signature below, regardless of how many arguments it takes:
 *
 *     #include "cob.h"
 *     COB_EXPORT CobValue calculate_fast(CobValue *argv, int argc) {
 *         if (argc < 1 || argv[0].kind != COB_INT) return cob_int(0);
 *         return cob_int(argv[0].as.i * argv[0].as.i + 1);
 *     }
 *
 *     gcc -shared -fPIC -Iinclude -o math_ext.so math_ext.c      (Linux)
 *     cc  -dynamiclib   -Iinclude -o math_ext.dylib math_ext.c   (macOS)
 *     gcc -shared       -Iinclude -o math_ext.dll   math_ext.c   (Windows)
 *
 * Value model (CEP-2 3.1): Cob has exactly two value kinds, an integer
 * (long) and a string (char *). CobValue bridges those two -- no
 * arrays, structs, or callbacks cross this boundary.
 *
 * Ownership rules:
 *   - argv[i].as.s (COB_STR arguments) is NUL-terminated and owned by
 *     the interpreter. It is only valid for the duration of the call;
 *     copy it if you need to keep it.
 *   - A COB_STR *return value* is copied by Cob immediately after your
 *     function returns, and Cob never frees it. Return a string literal,
 *     a static buffer, or a buffer you keep alive until your next call --
 *     do NOT return a pointer to a local (stack) array.
 *   - argc is the number of arguments written at the Cob call site.
 *     Arity is NOT checked by Cob; validate argc and each argv[i].kind
 *     before reading argv[i].as.
 *
 * Not available in the browser sandbox: the WebAssembly build of
 * cob_interp cannot dlopen() native code, so `extern` there binds to a
 * harmless no-op stub (see the "web sandbox" notes in docs/).
 *
 * License: PolyForm Noncommercial License 1.0.0 - see LICENSE.md
 * ===================================================================== */

#ifndef COB_H
#define COB_H

#ifdef __cplusplus
extern "C" {
#endif

/* Mark a native function so it is visible to dlsym()/GetProcAddress().
 * Needed on Windows (DLLs export nothing by default); harmless elsewhere. */
#if defined(_WIN32) || defined(_WIN64)
#   define COB_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#   define COB_EXPORT __attribute__((visibility("default")))
#else
#   define COB_EXPORT
#endif

typedef enum { COB_INT, COB_STR } CobValueKind;

typedef struct {
    CobValueKind kind;
    union {
        long        i;   /* COB_INT */
        const char *s;   /* COB_STR -- NUL-terminated, owned by the caller;
                            copy it if you need to keep it past this call */
    } as;
} CobValue;

/* Every extern function has this exact signature. argv[0..argc-1] are the
 * arguments as written at the Cob call site, left to right. */
typedef CobValue (*CobFFIFunc)(CobValue *argv, int argc);

/* Convenience constructors for a native function's return value */
static inline CobValue cob_int(long v)        { CobValue r; r.kind = COB_INT; r.as.i = v; return r; }
static inline CobValue cob_str(const char *v) { CobValue r; r.kind = COB_STR; r.as.s = v; return r; }

#ifdef __cplusplus
}
#endif

#endif /* COB_H */

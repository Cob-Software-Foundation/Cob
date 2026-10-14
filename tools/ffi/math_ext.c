/* math_ext.c -- example/test native extension for CEP-2 (the C API / FFI).
 *
 *   Linux:   gcc -shared -fPIC -I../../include -o math_ext.so math_ext.c
 *   macOS:   cc  -dynamiclib   -I../../include -o math_ext.dylib math_ext.c
 *   Windows: gcc -shared       -I../../include -o math_ext.dll math_ext.c
 */
#include <stdio.h>
#include <string.h>
#include "cob.h"

/* the example straight out of CEP-2 section 3.2, with the argc/kind check
 * its comment says "real code should" do */
COB_EXPORT CobValue calculate_fast(CobValue *argv, int argc) {
    long x;
    if (argc < 1 || argv[0].kind != COB_INT) return cob_int(0);
    x = argv[0].as.i;
    return cob_int(x * x + 1);
}

/* string in, string out. The returned pointer is a static buffer: Cob
 * copies it immediately and never frees it (see cob.h). */
COB_EXPORT CobValue greet(CobValue *argv, int argc) {
    static char buf[256];
    const char *who = (argc >= 1 && argv[0].kind == COB_STR) ? argv[0].as.s : "stranger";
    snprintf(buf, sizeof(buf), "Hello, %s!", who);
    return cob_str(buf);
}

/* variable arity: the Cob call site decides how many arguments it passes */
COB_EXPORT CobValue sum_all(CobValue *argv, int argc) {
    long total = 0; int i;
    for (i = 0; i < argc; i++) if (argv[i].kind == COB_INT) total += argv[i].as.i;
    return cob_int(total);
}

/* proves the int/string tag survives the boundary in both directions */
COB_EXPORT CobValue kind_of(CobValue *argv, int argc) {
    if (argc < 1) return cob_str("none");
    return cob_str(argv[0].kind == COB_STR ? "string" : "int");
}

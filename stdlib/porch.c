/* =====================================================================
 * porch -- sit on the porch and talk to whoever is at the keyboard
 *                                                  (shuck porch)
 * ---------------------------------------------------------------------
 *   porch_ask(prompt)      print the prompt (no newline), read one line,
 *                          return it as a string without the line ending.
 *                          "" at end of input. The prompt is optional.
 *   porch_ask_int(prompt)  the same, but read the line as a whole number.
 *                          Not a number (or end of input) gives 0, with a
 *                          warning for the first case.
 * ===================================================================== */
#include "stdhelp.h"

#define MOD "porch"
static char *p_line_buf;

/* Reads one line from stdin. Returns a malloc'd string without the line
 * ending, "" at EOF; *got is 1 if a line (even an empty one) was read. */
static char *porch_read_line(const char *prompt, int *got) {
    size_t cap = 128, len = 0;
    char *buf = (char *)malloc(cap);
    *got = 0;
    if (!buf) return NULL;
    if (prompt && prompt[0]) fputs(prompt, stdout);
    fflush(stdout);
    for (;;) {
        if (!fgets(buf + len, (int)(cap - len), stdin)) break;
        *got = 1;
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
    if (!*got) len = 0;
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) len--;
    buf[len] = '\0';
    return buf;
}
static const char *porch_prompt(CobValue *argv, int argc, char *numbuf, size_t numsz) {
    if (argc < 1) return "";
    if (argv[0].kind == COB_STR) return argv[0].as.s ? argv[0].as.s : "";
    snprintf(numbuf, numsz, "%ld", argv[0].as.i);
    return numbuf;
}
CS_ON_UNLOAD(porch_unload) { free(p_line_buf); }

COB_EXPORT CobValue porch_ask(CobValue *argv, int argc) {
    char nb[32]; int got; char *line; CobValue r;
    line = porch_read_line(porch_prompt(argv, argc, nb, sizeof(nb)), &got);
    if (!line) { fprintf(stderr, "[" MOD "] out of memory in porch_ask()\n"); return cob_str(""); }
    r = cs_ret_str(&p_line_buf, line, strlen(line));
    free(line);
    return r;
}
COB_EXPORT CobValue porch_ask_int(CobValue *argv, int argc) {
    char nb[32]; int got, ok = 0; long n = 0; char *line; const char *q; char *end;
    line = porch_read_line(porch_prompt(argv, argc, nb, sizeof(nb)), &got);
    if (!line) { fprintf(stderr, "[" MOD "] out of memory in porch_ask_int()\n"); return cob_int(0); }
    q = line;
    while (*q == ' ' || *q == '\t') q++;
    if (*q) {
        n = strtol(q, &end, 10);
        while (*end == ' ' || *end == '\t') end++;
        ok = (end != q && *end == '\0');
    }
    if (!ok) {
        if (got) fprintf(stderr, "[" MOD "] warning: porch_ask_int() got non-numeric input \"%s\"; treated as 0\n", line);
        n = 0;
    }
    free(line);
    return cob_int(n);
}

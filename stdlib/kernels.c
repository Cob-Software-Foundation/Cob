/* =====================================================================
 * kernels -- text, kernel by kernel            (shuck kernels)
 * ---------------------------------------------------------------------
 *   kernel_len(s)                  number of characters (bytes)
 *   kernel_upper(s) / kernel_lower(s)   ASCII case change
 *   kernel_slice(s, start, count)  `count` characters from `start` (0-based)
 *   kernel_find(hay, needle)       index of the first match, or -1
 *   kernel_repeat(s, n)            s written n times
 *   kernel_trim(s)                 without leading/trailing blanks
 *   kernel_replace(s, from, to)    every `from` swapped for `to`
 *   kernel_char(s, i)              the one-character string at index i ("" if none)
 *   kernel_code(s)                 byte value of the first character (0 if empty)
 *   kernel_from_code(n)            the one-character string with byte value n (1..255)
 *   kernel_to_int(s)               s read as a whole number (0 if it isn't one)
 *   kernel_starts(s, prefix) / kernel_ends(s, suffix)   1 or 0
 * ===================================================================== */
#include "stdhelp.h"
#include <ctype.h>

#define MOD "kernels"
#define KERNEL_MAX_RESULT (64UL * 1024UL * 1024UL)

static char *k_upper_buf, *k_lower_buf, *k_slice_buf, *k_repeat_buf, *k_trim_buf, *k_replace_buf, *k_char_buf, *k_chr_buf;

CS_ON_UNLOAD(kernels_unload) {
    free(k_upper_buf); free(k_lower_buf); free(k_slice_buf); free(k_repeat_buf);
    free(k_trim_buf); free(k_replace_buf); free(k_char_buf); free(k_chr_buf);
}

COB_EXPORT CobValue kernel_len(CobValue *argv, int argc) {
    if (!cs_arity(MOD, "kernel_len", argc, 1)) return cob_int(0);
    return cob_int((long)strlen(cs_str(MOD, "kernel_len", argv, argc, 0)));
}
COB_EXPORT CobValue kernel_upper(CobValue *argv, int argc) {
    const char *s; size_t i, n; char *t; CobValue r;
    if (!cs_arity(MOD, "kernel_upper", argc, 1)) return cob_str("");
    s = cs_str(MOD, "kernel_upper", argv, argc, 0); n = strlen(s);
    t = (char *)malloc(n + 1); if (!t) return cob_str("");
    for (i = 0; i < n; i++) t[i] = (char)toupper((unsigned char)s[i]);
    r = cs_ret_str(&k_upper_buf, t, n); free(t); return r;
}
COB_EXPORT CobValue kernel_lower(CobValue *argv, int argc) {
    const char *s; size_t i, n; char *t; CobValue r;
    if (!cs_arity(MOD, "kernel_lower", argc, 1)) return cob_str("");
    s = cs_str(MOD, "kernel_lower", argv, argc, 0); n = strlen(s);
    t = (char *)malloc(n + 1); if (!t) return cob_str("");
    for (i = 0; i < n; i++) t[i] = (char)tolower((unsigned char)s[i]);
    r = cs_ret_str(&k_lower_buf, t, n); free(t); return r;
}
COB_EXPORT CobValue kernel_slice(CobValue *argv, int argc) {
    const char *s; long start, count; size_t n;
    if (!cs_arity(MOD, "kernel_slice", argc, 3)) return cob_str("");
    s = cs_str(MOD, "kernel_slice", argv, argc, 0);
    start = cs_int(MOD, "kernel_slice", argv, argc, 1); count = cs_int(MOD, "kernel_slice", argv, argc, 2);
    n = strlen(s);
    if (start < 0) start = 0;
    if (count < 0) count = 0;
    if ((size_t)start >= n) return cs_ret_str(&k_slice_buf, "", 0);
    if ((size_t)count > n - (size_t)start) count = (long)(n - (size_t)start);
    return cs_ret_str(&k_slice_buf, s + start, (size_t)count);
}
COB_EXPORT CobValue kernel_find(CobValue *argv, int argc) {
    const char *h, *nd, *p;
    if (!cs_arity(MOD, "kernel_find", argc, 2)) return cob_int(-1);
    h = cs_str(MOD, "kernel_find", argv, argc, 0); nd = cs_str(MOD, "kernel_find", argv, argc, 1);
    p = strstr(h, nd);
    return cob_int(p ? (long)(p - h) : -1);
}
COB_EXPORT CobValue kernel_repeat(CobValue *argv, int argc) {
    const char *s; long times, i; size_t n; char *t; CobValue r;
    if (!cs_arity(MOD, "kernel_repeat", argc, 2)) return cob_str("");
    s = cs_str(MOD, "kernel_repeat", argv, argc, 0); times = cs_int(MOD, "kernel_repeat", argv, argc, 1);
    n = strlen(s);
    if (times <= 0 || n == 0) return cs_ret_str(&k_repeat_buf, "", 0);
    if ((unsigned long)times > KERNEL_MAX_RESULT / n) {
        fprintf(stderr, "[" MOD "] warning: kernel_repeat() result would be too large; gives \"\"\n");
        return cs_ret_str(&k_repeat_buf, "", 0);
    }
    t = (char *)malloc(n * (size_t)times + 1); if (!t) return cob_str("");
    for (i = 0; i < times; i++) memcpy(t + (size_t)i * n, s, n);
    r = cs_ret_str(&k_repeat_buf, t, n * (size_t)times); free(t); return r;
}
COB_EXPORT CobValue kernel_trim(CobValue *argv, int argc) {
    const char *s; size_t n;
    if (!cs_arity(MOD, "kernel_trim", argc, 1)) return cob_str("");
    s = cs_str(MOD, "kernel_trim", argv, argc, 0); n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) n--;
    while (n > 0 && isspace((unsigned char)*s)) { s++; n--; }
    return cs_ret_str(&k_trim_buf, s, n);
}
COB_EXPORT CobValue kernel_replace(CobValue *argv, int argc) {
    const char *s, *from, *to, *p; size_t sl, fl, tl, count = 0, outn; char *t, *w; CobValue r;
    if (!cs_arity(MOD, "kernel_replace", argc, 3)) return cob_str("");
    s = cs_str(MOD, "kernel_replace", argv, argc, 0); from = cs_str(MOD, "kernel_replace", argv, argc, 1);
    to = cs_str(MOD, "kernel_replace", argv, argc, 2);
    sl = strlen(s); fl = strlen(from); tl = strlen(to);
    if (fl == 0) return cs_ret_str(&k_replace_buf, s, sl);
    for (p = s; (p = strstr(p, from)) != NULL; p += fl) count++;
    outn = sl + count * tl - count * fl;
    if (outn > KERNEL_MAX_RESULT) {
        fprintf(stderr, "[" MOD "] warning: kernel_replace() result would be too large; gives \"\"\n");
        return cs_ret_str(&k_replace_buf, "", 0);
    }
    t = (char *)malloc(outn + 1); if (!t) return cob_str("");
    w = t;
    for (p = s; ; ) {
        const char *hit = strstr(p, from);
        if (!hit) { size_t rest = strlen(p); memcpy(w, p, rest); w += rest; break; }
        memcpy(w, p, (size_t)(hit - p)); w += hit - p;
        memcpy(w, to, tl); w += tl;
        p = hit + fl;
    }
    r = cs_ret_str(&k_replace_buf, t, (size_t)(w - t)); free(t); return r;
}
COB_EXPORT CobValue kernel_char(CobValue *argv, int argc) {
    const char *s; long i;
    if (!cs_arity(MOD, "kernel_char", argc, 2)) return cob_str("");
    s = cs_str(MOD, "kernel_char", argv, argc, 0); i = cs_int(MOD, "kernel_char", argv, argc, 1);
    if (i < 0 || (size_t)i >= strlen(s)) return cs_ret_str(&k_char_buf, "", 0);
    return cs_ret_str(&k_char_buf, s + i, 1);
}
COB_EXPORT CobValue kernel_code(CobValue *argv, int argc) {
    const char *s;
    if (!cs_arity(MOD, "kernel_code", argc, 1)) return cob_int(0);
    s = cs_str(MOD, "kernel_code", argv, argc, 0);
    return cob_int((long)(unsigned char)s[0]);
}
COB_EXPORT CobValue kernel_from_code(CobValue *argv, int argc) {
    long n; char c;
    if (!cs_arity(MOD, "kernel_from_code", argc, 1)) return cob_str("");
    n = cs_int(MOD, "kernel_from_code", argv, argc, 0);
    if (n < 1 || n > 255) return cs_ret_str(&k_chr_buf, "", 0);
    c = (char)n;
    return cs_ret_str(&k_chr_buf, &c, 1);
}
COB_EXPORT CobValue kernel_to_int(CobValue *argv, int argc) {
    const char *s, *q; char *end; long n;
    if (!cs_arity(MOD, "kernel_to_int", argc, 1)) return cob_int(0);
    s = cs_str(MOD, "kernel_to_int", argv, argc, 0);
    q = s; while (*q == ' ' || *q == '\t') q++;
    if (!*q) return cob_int(0);
    n = strtol(q, &end, 10);
    while (*end == ' ' || *end == '\t') end++;
    return cob_int((end != q && *end == '\0') ? n : 0);
}
COB_EXPORT CobValue kernel_starts(CobValue *argv, int argc) {
    const char *s, *p;
    if (!cs_arity(MOD, "kernel_starts", argc, 2)) return cob_int(0);
    s = cs_str(MOD, "kernel_starts", argv, argc, 0); p = cs_str(MOD, "kernel_starts", argv, argc, 1);
    return cob_int(strncmp(s, p, strlen(p)) == 0);
}
COB_EXPORT CobValue kernel_ends(CobValue *argv, int argc) {
    const char *s, *p; size_t sl, pl;
    if (!cs_arity(MOD, "kernel_ends", argc, 2)) return cob_int(0);
    s = cs_str(MOD, "kernel_ends", argv, argc, 0); p = cs_str(MOD, "kernel_ends", argv, argc, 1);
    sl = strlen(s); pl = strlen(p);
    return cob_int(pl <= sl && strcmp(s + (sl - pl), p) == 0);
}

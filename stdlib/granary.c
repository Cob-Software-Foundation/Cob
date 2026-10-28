/* =====================================================================
 * granary -- files, stored like grain             (shuck granary)
 * ---------------------------------------------------------------------
 *   granary_read(path)           the whole file as a string ("" if it can't be read)
 *   granary_write(path, text)    replace the file's contents; 0 on success, 1 on failure
 *   granary_append(path, text)   add to the end of the file; 0 on success, 1 on failure
 *   granary_exists(path)         1 if the file can be opened, else 0
 *   granary_remove(path)         delete the file; 0 on success, 1 on failure
 * Text only: a file's contents are read up to its first zero byte.
 * ===================================================================== */
#include "stdhelp.h"

#define MOD "granary"
#define GRANARY_MAX_READ (256UL * 1024UL * 1024UL)
static char *g_read_buf;

CS_ON_UNLOAD(granary_unload) { free(g_read_buf); }

COB_EXPORT CobValue granary_read(CobValue *argv, int argc) {
    const char *path; FILE *fp; long len; char *buf; size_t got; CobValue r;
    if (!cs_arity(MOD, "granary_read", argc, 1)) return cob_str("");
    path = cs_str(MOD, "granary_read", argv, argc, 0);
    fp = fopen(path, "rb");
    if (!fp) { fprintf(stderr, "[" MOD "] warning: granary_read() could not open '%s'\n", path); return cs_ret_str(&g_read_buf, "", 0); }
    if (fseek(fp, 0, SEEK_END) != 0 || (len = ftell(fp)) < 0 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp); fprintf(stderr, "[" MOD "] warning: granary_read() could not size '%s'\n", path);
        return cs_ret_str(&g_read_buf, "", 0);
    }
    if ((unsigned long)len > GRANARY_MAX_READ) {
        fclose(fp); fprintf(stderr, "[" MOD "] warning: granary_read() '%s' is too large\n", path);
        return cs_ret_str(&g_read_buf, "", 0);
    }
    buf = (char *)malloc((size_t)len + 1);
    if (!buf) { fclose(fp); return cob_str(""); }
    got = fread(buf, 1, (size_t)len, fp);
    fclose(fp);
    buf[got] = '\0';
    r = cs_ret_str(&g_read_buf, buf, strlen(buf));
    free(buf);
    return r;
}
static CobValue granary_put(const char *fn, const char *mode, CobValue *argv, int argc) {
    const char *path, *text; FILE *fp; size_t n;
    if (!cs_arity(MOD, fn, argc, 2)) return cob_int(1);
    path = cs_str(MOD, fn, argv, argc, 0); text = cs_str(MOD, fn, argv, argc, 1);
    fp = fopen(path, mode);
    if (!fp) { fprintf(stderr, "[" MOD "] warning: %s() could not open '%s'\n", fn, path); return cob_int(1); }
    n = strlen(text);
    if (fwrite(text, 1, n, fp) != n) { fclose(fp); fprintf(stderr, "[" MOD "] warning: %s() could not write '%s'\n", fn, path); return cob_int(1); }
    return cob_int(fclose(fp) == 0 ? 0 : 1);
}
COB_EXPORT CobValue granary_write(CobValue *argv, int argc)  { return granary_put("granary_write", "wb", argv, argc); }
COB_EXPORT CobValue granary_append(CobValue *argv, int argc) { return granary_put("granary_append", "ab", argv, argc); }
COB_EXPORT CobValue granary_exists(CobValue *argv, int argc) {
    FILE *fp;
    if (!cs_arity(MOD, "granary_exists", argc, 1)) return cob_int(0);
    fp = fopen(cs_str(MOD, "granary_exists", argv, argc, 0), "rb");
    if (!fp) return cob_int(0);
    fclose(fp);
    return cob_int(1);
}
COB_EXPORT CobValue granary_remove(CobValue *argv, int argc) {
    if (!cs_arity(MOD, "granary_remove", argc, 1)) return cob_int(1);
    return cob_int(remove(cs_str(MOD, "granary_remove", argv, argc, 0)) == 0 ? 0 : 1);
}

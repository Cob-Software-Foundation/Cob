/* =====================================================================
 * pantry -- a SQLite database to keep things in    (shuck pantry)
 * ---------------------------------------------------------------------
 *   pantry_open(path)         open (or create) a database file; returns a
 *                             handle (1, 2, ...), or 0 on failure
 *   pantry_exec(h, sql)       run a statement that gives no answer
 *                             (CREATE, INSERT, UPDATE, ...); 0 on success
 *   pantry_query(h, sql)      run a SELECT; returns the first column of
 *                             the first row as a string ("" if none)
 *   pantry_close(h)           close it; returns 0
 * The SQLite amalgamation (vendor/SQLite) is compiled into libpantry.
 * ===================================================================== */
#include "stdhelp.h"
#include <sqlite3.h>

#define MOD "pantry"
#define PANTRY_MAX_HANDLES 16
static sqlite3 *pantry_handles[PANTRY_MAX_HANDLES];
static char *pantry_result_buf;

static sqlite3 *pantry_get(long h) {
    if (h < 1 || h > PANTRY_MAX_HANDLES) return NULL;
    return pantry_handles[h - 1];
}
COB_EXPORT CobValue pantry_open(CobValue *argv, int argc) {
    const char *path; int i;
    if (!cs_arity(MOD, "pantry_open", argc, 1)) return cob_int(0);
    path = cs_str(MOD, "pantry_open", argv, argc, 0);
    for (i = 0; i < PANTRY_MAX_HANDLES; i++) {
        if (pantry_handles[i] != NULL) continue;
        if (sqlite3_open(path, &pantry_handles[i]) != SQLITE_OK) {
            fprintf(stderr, "[" MOD "] warning: pantry_open(\"%s\") failed: %s\n", path,
                    pantry_handles[i] ? sqlite3_errmsg(pantry_handles[i]) : "unknown error");
            if (pantry_handles[i]) { sqlite3_close(pantry_handles[i]); pantry_handles[i] = NULL; }
            return cob_int(0);
        }
        return cob_int((long)(i + 1));
    }
    fprintf(stderr, "[" MOD "] warning: pantry_open(\"%s\") failed: too many open handles (max %d)\n", path, PANTRY_MAX_HANDLES);
    return cob_int(0);
}
COB_EXPORT CobValue pantry_exec(CobValue *argv, int argc) {
    sqlite3 *db; char *err = NULL; long h; const char *sql;
    if (!cs_arity(MOD, "pantry_exec", argc, 2)) return cob_int(-1);
    h = cs_int(MOD, "pantry_exec", argv, argc, 0); sql = cs_str(MOD, "pantry_exec", argv, argc, 1);
    db = pantry_get(h);
    if (!db) { fprintf(stderr, "[" MOD "] warning: pantry_exec() on invalid/closed handle %ld\n", h); return cob_int(-1); }
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "[" MOD "] warning: pantry_exec() failed: %s\n", err ? err : "unknown error");
        sqlite3_free(err);
        return cob_int(-1);
    }
    return cob_int(0);
}
/* sqlite3_exec() callback: keep the first column of the first row, then stop. */
static int pantry_first_cb(void *user, int n, char **vals, char **names) {
    char **out = (char **)user;
    (void)names;
    if (n > 0 && vals[0]) { size_t l = strlen(vals[0]); *out = (char *)malloc(l + 1); if (*out) memcpy(*out, vals[0], l + 1); }
    return 1;
}
COB_EXPORT CobValue pantry_query(CobValue *argv, int argc) {
    sqlite3 *db; char *result = NULL, *err = NULL; int rc; long h; const char *sql; CobValue r;
    if (!cs_arity(MOD, "pantry_query", argc, 2)) return cob_str("");
    h = cs_int(MOD, "pantry_query", argv, argc, 0); sql = cs_str(MOD, "pantry_query", argv, argc, 1);
    db = pantry_get(h);
    if (!db) { fprintf(stderr, "[" MOD "] warning: pantry_query() on invalid/closed handle %ld\n", h); return cs_ret_str(&pantry_result_buf, "", 0); }
    rc = sqlite3_exec(db, sql, pantry_first_cb, &result, &err);
    if (rc != SQLITE_OK && rc != SQLITE_ABORT)
        fprintf(stderr, "[" MOD "] warning: pantry_query() failed: %s\n", err ? err : "unknown error");
    sqlite3_free(err); /* also set (to "query aborted") when the callback stops after the first row */
    r = cs_ret_str(&pantry_result_buf, result ? result : "", result ? strlen(result) : 0);
    free(result);
    return r;
}
COB_EXPORT CobValue pantry_close(CobValue *argv, int argc) {
    long h; sqlite3 *db;
    if (!cs_arity(MOD, "pantry_close", argc, 1)) return cob_int(0);
    h = cs_int(MOD, "pantry_close", argv, argc, 0);
    db = pantry_get(h);
    if (db) { sqlite3_close(db); pantry_handles[h - 1] = NULL; }
    return cob_int(0);
}
#if defined(__GNUC__) || defined(__clang__)
/* close anything left open when the library is unloaded */
__attribute__((destructor)) static void pantry_shutdown(void) {
    int i;
    for (i = 0; i < PANTRY_MAX_HANDLES; i++)
        if (pantry_handles[i]) { sqlite3_close(pantry_handles[i]); pantry_handles[i] = NULL; }
    free(pantry_result_buf);
    pantry_result_buf = NULL;
    sqlite3_shutdown(); /* releases SQLite's own global state */
}
#endif

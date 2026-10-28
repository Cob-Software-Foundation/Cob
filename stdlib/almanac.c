/* =====================================================================
 * almanac -- the farmer's calendar and clock    (shuck almanac)
 * ---------------------------------------------------------------------
 *   almanac_now()        seconds since 1970-01-01 (Unix time)
 *   almanac_ticks()      milliseconds on a steady clock (for timing things)
 *   almanac_nap(ms)      sleep for `ms` milliseconds
 *   almanac_year()       the current year (local time)
 *   almanac_stamp()      "YYYY-MM-DD HH:MM:SS" in local time
 * ===================================================================== */
#define _POSIX_C_SOURCE 200809L
#include "stdhelp.h"
#include <time.h>
#if defined(_WIN32)
#  include <windows.h>
#endif

#define MOD "almanac"
static char *a_stamp_buf;

CS_ON_UNLOAD(almanac_unload) { free(a_stamp_buf); }

COB_EXPORT CobValue almanac_now(CobValue *argv, int argc) {
    (void)argv; (void)argc;
    return cob_int((long)time(NULL));
}
COB_EXPORT CobValue almanac_ticks(CobValue *argv, int argc) {
    (void)argv; (void)argc;
#if defined(_WIN32)
    return cob_int((long)GetTickCount64());
#else
    { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return cob_int((long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L); }
#endif
}
COB_EXPORT CobValue almanac_nap(CobValue *argv, int argc) {
    long ms;
    if (!cs_arity(MOD, "almanac_nap", argc, 1)) return cob_int(0);
    ms = cs_int(MOD, "almanac_nap", argv, argc, 0);
    if (ms <= 0) return cob_int(0);
#if defined(_WIN32)
    Sleep((DWORD)ms);
#else
    { struct timespec ts; ts.tv_sec = ms / 1000; ts.tv_nsec = (ms % 1000) * 1000000L; nanosleep(&ts, NULL); }
#endif
    return cob_int(0);
}
COB_EXPORT CobValue almanac_year(CobValue *argv, int argc) {
    time_t t = time(NULL); struct tm *lt = localtime(&t);
    (void)argv; (void)argc;
    return cob_int(lt ? (long)lt->tm_year + 1900 : 0);
}
COB_EXPORT CobValue almanac_stamp(CobValue *argv, int argc) {
    time_t t = time(NULL); struct tm *lt = localtime(&t); char buf[32];
    (void)argv; (void)argc;
    if (!lt || strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", lt) == 0) return cob_str("");
    return cs_ret_str(&a_stamp_buf, buf, strlen(buf));
}

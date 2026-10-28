/* =====================================================================
 * tassel -- Tcl and Tk, the tassel on top of the ear   (shuck tassel)
 * ---------------------------------------------------------------------
 *   tassel_tcl(script)   evaluate Tcl in one process-wide interpreter and
 *                        return its string result
 *   tassel_tk(script)    the same interpreter, with Tk loaded first (a real
 *                        display is needed; without one you get a normal
 *                        Tcl error string, not a crash)
 * State (variables, widgets) persists between calls for the whole run.
 * The vendored Tcl/Tk (vendor/TCL, vendor/TK) is compiled into libtassel;
 * set TCL_LIBRARY to vendor/TCL/library so Tcl_Init() finds init.tcl.
 *
 * This is the old built-in tcl_eval()/tk_eval(), moved out of the
 * interpreter into a standard-library module.
 * ===================================================================== */
#include "stdhelp.h"
#include <tcl.h>
#include <tk.h>

#define MOD "tassel"
static Tcl_Interp *tassel_interp = NULL;
static int tassel_tk_ready = 0;
static char *tassel_result_buf;

static Tcl_Interp *tassel_get(void) {
    if (!tassel_interp) {
        tassel_interp = Tcl_CreateInterp();
        if (tassel_interp && Tcl_Init(tassel_interp) != TCL_OK)
            fprintf(stderr, "[" MOD "] warning: Tcl_Init() failed: %s\n", Tcl_GetStringResult(tassel_interp));
    }
    return tassel_interp;
}
static CobValue tassel_run(const char *fn, int with_tk, CobValue *argv, int argc) {
    Tcl_Interp *interp; const char *script, *res;
    if (!cs_arity(MOD, fn, argc, 1)) return cob_str("");
    script = cs_str(MOD, fn, argv, argc, 0);
    interp = tassel_get();
    if (!interp) { fprintf(stderr, "[" MOD "] warning: %s() failed: could not create a Tcl interpreter\n", fn); return cob_str(""); }
    if (with_tk && !tassel_tk_ready) {
        tassel_tk_ready = 1;
        if (Tk_Init(interp) != TCL_OK)
            fprintf(stderr, "[" MOD "] warning: %s(): Tk_Init() failed: %s\n", fn, Tcl_GetStringResult(interp));
    }
    if (Tcl_Eval(interp, script) != TCL_OK)
        fprintf(stderr, "[" MOD "] warning: %s() error: %s\n", fn, Tcl_GetStringResult(interp));
    res = Tcl_GetStringResult(interp);
    return cs_ret_str(&tassel_result_buf, res, strlen(res));
}
COB_EXPORT CobValue tassel_tcl(CobValue *argv, int argc) { return tassel_run("tassel_tcl", 0, argv, argc); }
COB_EXPORT CobValue tassel_tk(CobValue *argv, int argc)  { return tassel_run("tassel_tk", 1, argv, argc); }
#if defined(__GNUC__) || defined(__clang__)
__attribute__((destructor)) static void tassel_shutdown(void) {
    if (tassel_interp) { Tcl_DeleteInterp(tassel_interp); tassel_interp = NULL; }
    free(tassel_result_buf);
    tassel_result_buf = NULL;
}
#endif

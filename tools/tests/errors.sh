#!/bin/sh
# errors.sh <cob_interp> -- every diagnostic is clear, names the line, and exits 1
INTERP=$1
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
cd "$T" || exit 1
fails=0
# expect NAME 'source with \n escapes' 'text the error must contain'
expect() {
    printf '%b' "$2" > case.cob
    out=$("$INTERP" case.cob --no-cache 2>&1); rc=$?
    case "$out" in *"$3"*) found=1 ;; *) found=0 ;; esac
    if [ "$rc" = 1 ] && [ "$found" = 1 ]; then echo "ok    $1"; else echo "FAIL  $1 (exit $rc): $out"; fails=1; fi
}
expect "call with too few arguments"  'recipe f(a, b):\n    serve a\npop(f(1))\n'  "recipe 'f' takes 2 arguments but 1 was given"
expect "call to an unknown name"      'pop(nothere(1))\n'                          "call to 'nothere', which is not a recipe or a library function"
expect "stdlib function without shuck" 'pop(porch_ask("x"))\n'                     "add \`shuck porch\`"
expect "stdlib cornmath without shuck" 'pop(ear_max(1, 2))\n'                      "add \`shuck cornmath\`"
expect "old input() is gone"          'set x = input("a")\n'                       "'input' is now porch_ask()"
expect "old input_int() is gone"      'set x = input_int("a")\n'                   "porch_ask_int"
expect "old sql_open() is gone"       'set h = sql_open("a.db")\n'                 "pantry_open"
expect "old window_open() is gone"    'set h = window_open("w")\n'                 "barn_open"
expect "old tcl_eval() is gone"       'set h = tcl_eval("x")\n'                    "tassel_tcl"
expect "serve outside a recipe"       'serve 5\n'                                  "'serve' can only be used inside a recipe"
expect "recipe inside a block"        'if 1 == 1:\n    recipe f():\n        serve 1\n' "only be defined at the top level"
expect "duplicate recipe"             'recipe f():\n    serve 1\nrecipe f():\n    serve 2\n' "recipe 'f' is already defined (line 1)"
expect "keyword as recipe name"       'recipe pop():\n    serve 1\n'                "is a Cob keyword and cannot name a recipe"
expect "duplicate parameter"          'recipe f(a, a):\n    serve 1\n'            "two parameters named 'a'"
expect "empty recipe body"            'recipe f():\npop(1)\n'                      "needs an indented body"
expect "missing colon"                'recipe f()\n    serve 1\n'                  "expected ':'"
expect "junk after serve"             'recipe f():\n    serve 1 2\n'                "unexpected text after the serve expression"
expect "harvest alone as a statement" 'harvest(5)\n'                               "can stand alone as a statement"
expect "unknown shuck module"         'shuck nosuchmodule\n'                       "shuck 'nosuchmodule' failed"
expect "extern clashing with recipe"  'recipe f():\n    serve 1\nextern f(x) from "m"\n' "clashes with an extern"
exit $fails

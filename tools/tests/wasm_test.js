// wasm_test.js <cob_interp> -- the browser build (docs/play/cob_interp.wasm,
// loaded through docs/play/cob_interp.js exactly as the playground page loads
// it) must behave like the native interpreter. Run by wasm.sh.
"use strict";
const fs = require("fs");
const path = require("path");
const cp = require("child_process");

const HERE = __dirname;
const PLAY = path.join(HERE, "..", "..", "docs", "play");
const NATIVE = process.argv[2];
const { createCobModule } = require(path.join(PLAY, "cob_interp.js"));

let failed = 0;
function ok(name) { console.log("ok    " + name); }
function bad(name, why) { failed++; console.log("FAIL  " + name + (why ? "\n      " + why : "")); }
function expectEq(name, got, want) {
  if (got === want) ok(name);
  else bad(name, "got " + JSON.stringify(got).slice(0, 300) + " want " + JSON.stringify(want).slice(0, 300));
}

// Runs a program in the wasm build, feeding stdin the way the playground page
// does: one line per read, with a null ending each read().
async function runWasm(source, stdinText, flags) {
  let out = "", err = "";
  const lines = stdinText === undefined ? null : stdinText.split("\n");
  if (lines && lines[lines.length - 1] === "") lines.pop();
  let cur = null, at = 0, eol = false, li = 0;
  function stdin() {
    if (!lines) return null;
    if (eol) { eol = false; return null; }
    if (!cur) {
      if (li >= lines.length) return null;
      cur = Buffer.from(lines[li++] + "\n"); at = 0;
    }
    const c = cur[at++];
    if (at >= cur.length) { cur = null; eol = true; }
    return c;
  }
  const M = await createCobModule({
    stdin,
    stdout: (c) => (out += String.fromCharCode(c)),
    stderr: (c) => (err += String.fromCharCode(c)),
  });
  M.FS.writeFile("/program.cob", source);
  let status = 0;
  try { M.callMain(["/program.cob", "--no-cache"].concat(flags || [])); }
  catch (e) { if (e && typeof e.status === "number") status = e.status; else throw e; }
  return { out, err, status };
}

function runNative(file, stdinText) {
  const r = cp.spawnSync(NATIVE, [file, "--no-cache"], {
    input: stdinText === undefined ? "" : stdinText,
    encoding: "latin1",
    env: Object.assign({}, process.env, { COB_STDLIB: path.join(path.dirname(NATIVE), "lib") }),
  });
  return { out: r.stdout, err: r.stderr, status: r.status };
}

const latin1 = (s) => Buffer.from(s, "utf8").toString("latin1"); // wasm output is collected byte-wise
const read = (f) => fs.readFileSync(path.join(HERE, f), "utf8");

(async () => {
  // 1. the shared fixtures: wasm must produce the .expected output
  for (const name of ["functions", "if_statements"]) {
    const r = await runWasm(read(name + ".cob"));
    expectEq("wasm  " + name + " (matches .expected)", r.out, read(name + ".expected"));
  }
  {
    const r = await runWasm(read("porch_input.cob"), read("porch_input.stdin"));
    expectEq("wasm  porch_input (page-style stdin, matches .expected)", r.out, read("porch_input.expected"));
  }

  // 2. differential: stdout, stderr and exit code identical to the native build
  for (const name of ["parity_language", "parity_stdlib", "functions", "if_statements"]) {
    const w = await runWasm(read(name + ".cob"));
    const n = runNative(path.join(HERE, name + ".cob"));
    expectEq("parity " + name + " stdout", w.out, n.out);
    expectEq("parity " + name + " stderr", w.err, n.err);
    expectEq("parity " + name + " exit", w.status, n.status);
  }

  // 3. console input: end of input, and the page's cancel behaviour
  {
    const src = 'shuck porch\nset a = porch_ask("A? ")\nset b = porch_ask("B? ")\npop("[" + a + "][" + b + "]")\n';
    const r = await runWasm(src, "one\n");
    expectEq("wasm  porch: second read at end of input is empty", r.out, "A? B? [one][]\n");
    const r2 = await runWasm(src, undefined);
    expectEq("wasm  porch: no input at all", r2.out, "A? B? [][]\n");
  }

  // 4. errors: same words on stderr, exit code 1
  {
    const r = await runWasm("pop(\n");
    expectEq("wasm  syntax error text", r.err, "[cob_interp] syntax error at line 1: malformed expression in pop()\n");
    expectEq("wasm  syntax error exit status", r.status, 1);
    const r2 = await runWasm("pop(nope(1))\n");
    expectEq("wasm  unknown call text", r2.err, "[cob_interp] error at line 1: call to 'nope', which is not a recipe or a library function\n");
    const r3 = await runWasm("shuck nosuchmodule\n");
    expectEq("wasm  unknown shuck", r3.err.split(" -- ")[0], "[cob_interp] error at line 1: shuck 'nosuchmodule' failed");
    const r4 = await runWasm("set h = harvest(8)\n");
    expectEq("wasm  harvest needs --no-gc", r4.err, "[cob_interp] error: this program uses harvest()/trash() -- rerun with --no-gc\n");
    const r5 = await runWasm("set h = harvest(8)\npop(h)\ntrash(h)\n", undefined, ["--no-gc"]);
    expectEq("wasm  harvest/trash with --no-gc", r5.out, "1\n");
  }

  // 5. the sandbox: what can't run in a tab is announced once and evaluates to 0
  {
    const r = await runWasm('extern twice(n) from "mathx.so"\npop(twice(4))\npop(twice(5))\n');
    const notice = "\u26A0\uFE0F [COB Warning]: Native extensions (extern) cannot be loaded in the web sandbox; calls to 'mathx.so' are bypassed and evaluate to 0.\n";
    expectEq("wasm  extern stub: one notice, calls give 0", r.out.startsWith(latin1(notice)) || r.out.startsWith(notice), true);
    expectEq("wasm  extern stub: results", r.out.split("\n").slice(-3).join("|"), "0|0|");
    const p = await runWasm('shuck pantry\nset h = pantry_open("x.db")\npop(h)\nset g = pantry_open("y.db")\n');
    const count = (p.out.match(/COB Warning/g) || []).length;
    expectEq("wasm  pantry is stubbed with ONE notice per library", count, 1);
  }

  // 6. isolation: a second run starts from nothing
  {
    const a = await runWasm("set leak = 41\npop(leak)\nrecipe r():\n    serve 1\n");
    const b = await runWasm("pop(leak)\n");
    expectEq("wasm  fresh instance per run (first)", a.out, "41\n");
    expectEq("wasm  fresh instance per run (no leaked variable)", b.err, "[cob_interp] warning: undefined variable 'leak', treated as 0\n");
  }

  // 7. depth: the recursion cap and deep nesting must not trap the module
  {
    const rec = "recipe d(n):\n    if n == 0:\n        serve 0\n    serve 1 + d(n - 1)\npop(d(499))\npop(d(600))\n";
    const r = await runWasm(rec);
    expectEq("wasm  recursion to the 500-call cap", r.out, "499\n500\n");
    const nest = (n) => "recipe f(x):\n    serve x\npop(" + "f(".repeat(n) + "1" + ")".repeat(n) + ")\n";
    const d = await runWasm(nest(1000));
    expectEq("wasm  1000-deep nested call expression", d.out, "1\n");
    // far beyond that, the engine's own call stack runs out: a clean JS
    // RangeError (the page reports it) -- never a hang, and the next run is unaffected
    let overflow = null;
    try { await runWasm(nest(20000)); } catch (e) { overflow = e; }
    expectEq("wasm  absurd nesting fails cleanly with a RangeError", overflow instanceof RangeError, true);
    const after = await runWasm("pop(1 + 1)\n");
    expectEq("wasm  a fresh run works after an overflow", after.out, "2\n");
  }

  console.log(failed ? "\n" + failed + " wasm check(s) failed" : "\nall wasm checks passed");
  process.exit(failed ? 1 : 0);
})().catch((e) => { console.error("wasm_test crashed:", e); process.exit(2); });

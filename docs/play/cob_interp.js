/* cob_interp.js -- loads cob_interp.wasm (the Cob interpreter, built from
 * crates/cob-wasm by `make play`) and exposes it through the small
 * Emscripten-style API the playground page was written against:
 *
 *     var Module = await createCobModule({ stdin, stdout, stderr, print, printErr });
 *     Module.FS.writeFile("/program.cob", sourceText);
 *     Module.callMain(["/program.cob", "--no-cache"]);   // throws {status} on a nonzero exit
 *
 *   stdout(byte) / stderr(byte)   receive output one byte at a time, so a
 *                                 prompt with no newline is visible when asked.
 *   stdin()                       returns the next input byte, or null. As on a
 *                                 terminal, null ends one read() -- it is NOT end
 *                                 of file by itself: a read that gets no bytes
 *                                 at all is the end of input.
 *   print / printErr              line-based fallbacks if a sink above is absent.
 *
 * Each createCobModule() call makes a FRESH instance of the (once-compiled)
 * module, so no state -- variables, recipes, the per-run sandbox notice --
 * survives from one run to the next. Works in a browser and in Node.
 */
(function (root) {
  "use strict";

  var FLAG_NO_GC = 1;
  var compiled = null; // Promise<WebAssembly.Module>, shared by every run

  // opts.locateFile (Emscripten's convention, which Pounce's worker passes) gives
  // the .wasm's URL directly. Without it we work it out from the page's <script>
  // tag; a Web Worker has no `document`, so that path must not touch it.
  function wasmBytes(opts) {
    if (typeof process !== "undefined" && process.versions && process.versions.node && typeof require === "function") {
      var fs = require("fs"), path = require("path");
      return Promise.resolve(fs.readFileSync(path.join(__dirname, "cob_interp.wasm")));
    }
    var url;
    if (opts && typeof opts.locateFile === "function") {
      url = opts.locateFile("cob_interp.wasm");
    } else {
      var here = (typeof document !== "undefined" && document.currentScript && document.currentScript.src) ||
                 (typeof location !== "undefined" ? location.href : "");
      url = new URL("cob_interp.wasm", here).href;
    }
    return fetch(url).then(function (r) {
      if (!r.ok) throw new Error("could not load cob_interp.wasm (" + r.status + ")");
      return r.arrayBuffer();
    });
  }

  function loadModule(opts) {
    if (!compiled) compiled = wasmBytes(opts).then(function (b) { return WebAssembly.compile(b); });
    return compiled;
  }

  function ExitStatus(status) {
    this.name = "ExitStatus";
    this.message = "Program terminated with exit(" + status + ")";
    this.status = status;
  }
  ExitStatus.prototype = Object.create(Error.prototype);

  function createCobModule(opts) {
    opts = opts || {};
    return loadModule(opts).then(function (mod) {
      var files = Object.create(null);
      var exportsRef = null;
      var enc = new TextEncoder();

      // ---- output ----------------------------------------------------------
      var lineBuf = { 1: [], 2: [] };
      var dec = new TextDecoder();
      function lineFallback(fd, c) {
        var cb = fd === 1 ? (opts.print || function (l) { console.log(l); }) : (opts.printErr || function (l) { console.error(l); });
        if (c === 10) { cb(dec.decode(new Uint8Array(lineBuf[fd]))); lineBuf[fd] = []; } else lineBuf[fd].push(c);
      }
      function put(fd, c) {
        var sink = fd === 1 ? opts.stdout : opts.stderr;
        if (typeof sink === "function") sink(c); else lineFallback(fd, c);
      }
      function flushFallback() {
        [1, 2].forEach(function (fd) {
          if (lineBuf[fd].length) {
            var cb = fd === 1 ? (opts.print || console.log) : (opts.printErr || console.error);
            cb(dec.decode(new Uint8Array(lineBuf[fd]))); lineBuf[fd] = [];
          }
        });
      }

      // ---- input: emulate a terminal's read() -------------------------------
      var pending = [], pendingAt = 0;
      function fill() {
        pending = []; pendingAt = 0;
        if (typeof opts.stdin !== "function") return;
        for (var n = 0; n < (1 << 20); n++) {
          var c = opts.stdin();
          if (c === null || c === undefined) break;   // ends this read()
          pending.push(c & 255);
        }
      }

      var env = {
        cob_host_write: function (fd, ptr, len) {
          var b = new Uint8Array(exportsRef.memory.buffer, ptr, len);
          for (var i = 0; i < len; i++) put(fd, b[i]);
        },
        cob_host_flush: function () {},
        cob_host_read_byte: function () {
          if (pendingAt >= pending.length) fill();
          return pendingAt < pending.length ? pending[pendingAt++] : -1; // no bytes at all = end of input
        },
        cob_host_now_ms: function () { return Date.now(); },
        cob_host_ticks_ms: function () {
          return (typeof performance !== "undefined" && performance.now) ? performance.now() : Date.now();
        },
        // A sleep has to block (Cob's almanac_nap is synchronous), as it always did in the sandbox.
        cob_host_nap: function (ms) { var end = Date.now() + ms; while (Date.now() < end) { /* wait */ } },
        cob_host_local_time: function (ptr) {
          var d = new Date(), v = new DataView(exportsRef.memory.buffer);
          [d.getFullYear(), d.getMonth() + 1, d.getDate(), d.getHours(), d.getMinutes(), d.getSeconds()]
            .forEach(function (x, i) { v.setInt32(ptr + 4 * i, x, true); });
        }
      };

      return WebAssembly.instantiate(mod, { env: env }).then(function (instance) {
        exportsRef = instance.exports;
        return {
          FS: {
            writeFile: function (path, data) { files[path] = data; }
          },
          callMain: function (args) {
            args = args || [];
            var path = args[0], flags = 0;
            for (var i = 1; i < args.length; i++) if (args[i] === "--no-gc") flags |= FLAG_NO_GC;
            if (!(path in files)) {
              var msg = enc.encode("[cob_interp] error: could not read source file '" + path + "'\n");
              for (var k = 0; k < msg.length; k++) put(2, msg[k]);
              flushFallback();
              throw new ExitStatus(1);
            }
            var data = files[path];
            if (typeof data === "string") data = enc.encode(data);
            var ptr = exportsRef.cob_alloc(data.length);
            new Uint8Array(exportsRef.memory.buffer, ptr, data.length).set(data);
            var rc;
            try {
              rc = exportsRef.cob_run(ptr, data.length, flags);
            } finally {
              flushFallback();
            }
            exportsRef.cob_free(ptr, data.length);
            if (rc !== 0) throw new ExitStatus(rc);
          }
        };
      });
    });
  }

  root.createCobModule = createCobModule;
  if (typeof module !== "undefined" && module.exports) module.exports = { createCobModule: createCobModule };
})(typeof window !== "undefined" ? window : globalThis);

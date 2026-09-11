# =========================================================================
# Makefile -- Project Obsidian Falcon / Cob Language Toolchain
# =========================================================================
# Builds:
#   cob_interp    - the interpreter (src/cob_interp.c + src/file_io.c)
#   popcorn_comp  - the native compiler (transpiles to C, spawns a real
#                   C compiler -- see src/popcorn_comp.c's header
#                   comment for why this isn't a statically-linked TCC,
#                   and how it picks which compiler to spawn)
#   farmer        - the package manager
#
# None of the three have any TCC/src-tcc build dependency -- all three
# are plain, dependency-free C99 that build standalone.
#
# Platform detection follows the same $(OS)/uname pattern used in
# .github/workflows/build.yml, so this Makefile works unmodified under
# MSYS2/MinGW make on Windows as well as Linux/macOS make.
# =========================================================================

CC      ?= gcc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra
INCLUDE  = -Iinclude

BIN_DIR    = bin
VENDOR_DIR = vendor

ifeq ($(OS),Windows_NT)
    EXE_SUF = .exe
    SLEEP_CMD = timeout /t 1 >nul
    # ---------------------------------------------------------------
    # Windows (MSYS2/MinGW native shell -- sh, make, gcc, ar, windres
    # all from the mingw64 toolchain). Tcl/Tk ship a *separate* build
    # tree for this: vendor/TCL/win and vendor/TK/win, not unix/. The
    # Windows Tk build needs no X11 at all -- it talks to native Win32
    # (GDI/User32) and resolves Tk's internal `#include <X11/Xlib.h>`
    # via its own vendor/TK/xlib/ shim header, not real Xlib. Static
    # lib names also differ from the Unix build (libtcl90.a /
    # libtcl9tk90.a, no dot before the "90"), and because Tk always
    # goes through Tcl's stub mechanism internally (even in a "static"
    # build), the *.a and the matching *stub.a both have to be linked.
    # All of this was verified for real by cross-compiling this exact
    # target tree with a Linux-hosted x86_64-w64-mingw32 toolchain --
    # see Release.txt -- but not yet run on an actual Windows machine.
    # ---------------------------------------------------------------
    TCL_BUILD_DIR  = $(VENDOR_DIR)/TCL/win
    TK_BUILD_DIR   = $(VENDOR_DIR)/TK/win
    TCL_LIB        = $(TCL_BUILD_DIR)/libtcl90.a
    TCL_STUB_LIB   = $(TCL_BUILD_DIR)/libtclstub.a
    TK_LIB         = $(TK_BUILD_DIR)/libtcl9tk90.a
    TK_STUB_LIB    = $(TK_BUILD_DIR)/libtkstub.a
    TCL_DB_INCLUDE = -I$(VENDOR_DIR)/TCL/generic -I$(TCL_BUILD_DIR)
    TK_DB_INCLUDE  = -I$(VENDOR_DIR)/TK/generic -I$(TK_BUILD_DIR) -I$(VENDOR_DIR)/TK/xlib
    DB_DEFINES     = -DSTATIC_BUILD=1
    DB_LIBS        = $(TK_LIB) $(TK_STUB_LIB) $(TCL_LIB) $(TCL_STUB_LIB) \
                      -lnetapi32 -lkernel32 -luser32 -ladvapi32 -luserenv -lws2_32 \
                      -lgdi32 -lcomdlg32 -limm32 -lcomctl32 -lshell32 -luuid -lole32 -loleaut32 -lwinspool
    TCL_CONFIGURE  = cd $(TCL_BUILD_DIR) && CC="$(CC)" sh ./configure --disable-shared --enable-64bit
    TK_CONFIGURE   = cd $(TK_BUILD_DIR) && CC="$(CC)" sh ./configure --disable-shared --enable-64bit \
                      --with-tcl=$(abspath $(TCL_BUILD_DIR))
    # _cobwindow (v0.0.5): no vendor tree, just user32/gdi32 -- part of
    # every Windows install, nothing to build or configure.
    WINDOW_LIBS    = -luser32 -lgdi32
else
    EXE_SUF =
    SLEEP_CMD = sleep 1
    # ---------------------------------------------------------------
    # Linux / macOS: the unix/ build tree, real X11 for Tk.
    # ---------------------------------------------------------------
    TCL_BUILD_DIR  = $(VENDOR_DIR)/TCL/unix
    TK_BUILD_DIR   = $(VENDOR_DIR)/TK/unix
    TCL_LIB        = $(TCL_BUILD_DIR)/libtcl9.0.a
    TCL_STUB_LIB   =
    TK_LIB         = $(TK_BUILD_DIR)/libtcl9tk9.0.a
    TK_STUB_LIB    =
    TCL_DB_INCLUDE = -I$(VENDOR_DIR)/TCL/generic -I$(TCL_BUILD_DIR)
    TK_DB_INCLUDE  = -I$(VENDOR_DIR)/TK/generic -I$(TK_BUILD_DIR)
    DB_DEFINES     =
    DB_LIBS        = $(TK_LIB) $(TCL_LIB) -lX11 -ldl -lz -lpthread -lm
    TCL_CONFIGURE  = cd $(TCL_BUILD_DIR) && CC="$(CC)" sh ./configure --disable-shared --prefix=/tmp/tclinstall
    TK_CONFIGURE   = cd $(TK_BUILD_DIR) && CC="$(CC)" sh ./configure --disable-shared \
                      --with-tcl=$(abspath $(TCL_BUILD_DIR)) --prefix=/tmp/tkinstall
    # _cobwindow (v0.0.5): no vendor tree, just Xlib -- needs the same
    # libx11-dev / X11 dev headers the `tk` target's comment covers,
    # but no configure/make step of its own.
    WINDOW_LIBS    = -lX11
endif

.PHONY: all clean cob_interp cob_interp_db cob_interp_window popcorn_comp farmer smartpass

all: cob_interp popcorn_comp farmer

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

# -------------------------------------------------------------------------
# FUNNY EASTER EGG TARGET: smartpass
# Run 'make smartpass' to see the countdown action.
# -------------------------------------------------------------------------
smartpass:
	@echo "--- [SmartPass System Alert] ---"
	@echo "Initiating 3-second countdown to exit the terminal..."
	@$(SLEEP_CMD)
	@echo "Timer running: 2 seconds remaining..."
	@$(SLEEP_CMD)
	@echo "Timer running: 1 second remaining..."
	@$(SLEEP_CMD)
	@echo "[ERROR] OVERTIME DETECTED! 3 minutes is up!"
	@echo "[ERROR] Compilation frozen. Turn your Chromebook around and go get a physical yellow paper pass."
	@exit 1

cob_interp: $(BIN_DIR)
	$(CC) $(CFLAGS) $(INCLUDE) -o $(BIN_DIR)/cob_interp$(EXE_SUF) \
		src/file_io.c src/cob_interp.c

# -------------------------------------------------------------------------
# cob_interp_db -- same source file as cob_interp, built with the
# v0.0.5 SQLite/Tcl/Tk keyword bindings (sql_open, sql_exec, sql_query,
# sql_close, tcl_eval, tk_eval) compiled in. Needs `make sqlite tcl tk`
# run first (in that order doesn't matter; `tk` already depends on
# `tcl`). Produces a *separate* binary from plain cob_interp so the
# default `make` / `make all` toolchain keeps its zero-extra-dependency
# promise above -- this target is opt-in.
# -------------------------------------------------------------------------
.PHONY: cob_interp_db
cob_interp_db: $(BIN_DIR)
	$(CC) $(CFLAGS) $(INCLUDE) \
		-DCOB_WITH_SQLITE -DCOB_WITH_TCL -DCOB_WITH_TK $(DB_DEFINES) \
		-I$(VENDOR_DIR)/SQLite \
		$(TCL_DB_INCLUDE) \
		$(TK_DB_INCLUDE) \
		-o $(BIN_DIR)/cob_interp_db$(EXE_SUF) \
		src/file_io.c src/cob_interp.c \
		$(VENDOR_DIR)/SQLite/libsqlite3.a \
		$(DB_LIBS)
	@echo ""
	@echo "Built $(BIN_DIR)/cob_interp_db$(EXE_SUF)."
	@echo "tcl_eval()/tk_eval() need TCL_LIBRARY pointed at the vendored"
	@echo "script library (Tcl_Init() otherwise fails with a"
	@echo "\"Cannot find a usable init.tcl\" warning -- harmless for"
	@echo "expr-only scripts, but real programs should set it):"
ifeq ($(OS),Windows_NT)
	@echo "  set TCL_LIBRARY=$(abspath $(VENDOR_DIR)/TCL/library)"
	@echo "Real Tk widgets work through native Win32 windowing on"
	@echo "Windows -- no X11/\$$DISPLAY needed here, unlike Linux/macOS."
else
	@echo "  export TCL_LIBRARY=$(abspath $(VENDOR_DIR)/TCL/library)"
	@echo "Real Tk widgets (not just tcl_eval()-style expr/string work)"
	@echo "also need a real X11 \$$DISPLAY at runtime."
endif

# -------------------------------------------------------------------------
# cob_interp_window -- same source file as cob_interp, built with the
# v0.0.5 _cobwindow keywords (window_open, window_label, window_wait,
# window_close) compiled in. No vendor library, no configure/make step
# needed -- just the OS's own windowing API (user32/gdi32 on Windows,
# Xlib on Linux/macOS-with-X11). This exists as a much lighter-weight
# alternative to `cob_interp_db`'s tk_eval() when all you want is a
# plain window with a text label: no Tcl/Tk build to get through at
# all, so none of the fragility documented above (busybox-ash PATH,
# X11-vs-win32 vendor trees, static-vs-stub linking) applies here.
# -------------------------------------------------------------------------
.PHONY: cob_interp_window
cob_interp_window: $(BIN_DIR)
	$(CC) $(CFLAGS) $(INCLUDE) -DCOB_WITH_COBWINDOW \
		-o $(BIN_DIR)/cob_interp_window$(EXE_SUF) \
		src/file_io.c src/cob_interp.c \
		$(WINDOW_LIBS)
	@echo ""
	@echo "Built $(BIN_DIR)/cob_interp_window$(EXE_SUF)."

popcorn_comp: $(BIN_DIR)
	$(CC) $(CFLAGS) $(INCLUDE) -o $(BIN_DIR)/popcorn_comp$(EXE_SUF) \
		src/file_io.c src/popcorn_comp.c

farmer: $(BIN_DIR)
	$(CC) $(CFLAGS) $(INCLUDE) -Ivendor/miniz -D_POSIX_C_SOURCE=200809L -DMINIZ_NO_ZLIB_APIS \
		-o $(BIN_DIR)/farmer$(EXE_SUF) \
		src/file_io.c src/farmer.c \
		vendor/miniz/miniz.c vendor/miniz/miniz_tdef.c \
		vendor/miniz/miniz_tinfl.c vendor/miniz/miniz_zip.c \
		-lm

clean:
	rm -rf $(BIN_DIR)

# =========================================================================
# VENDORED LIBRARIES (vendor/SQLite, vendor/TCL, vendor/TK)
# =========================================================================
# Embedded per project decision -- see each target's comment for what
# "build" means for that library. As of v0.0.5 these ARE wired into
# Cob's language syntax: see `cob_interp_db` above, and sql_open() /
# sql_exec() / sql_query() / sql_close() / tcl_eval() / tk_eval() in
# src/cob_interp.c's header comment. TCL_BUILD_DIR/TK_BUILD_DIR/etc are
# set above per-platform (win/ vs unix/ build trees).
#
# These are real file targets (not .PHONY), keyed on the actual library
# file each one produces. That means `make cob_interp_db` alone builds
# SQLite/Tcl/Tk automatically the first time (no separate `make sqlite
# tcl tk` step needed), and skips rebuilding any of them on a second
# run since the .a files already exist -- `make sqlite`, `make tcl`,
# and `make tk` below are just convenience aliases for the same files,
# kept for anyone who wants to build one piece at a time or rebuild
# after `make vendor-clean`.
# =========================================================================

# -------------------------------------------------------------------------
# SQLite -- the "amalgamation" build: a single vendor/SQLite/sqlite3.c
# that compiles directly into any project needing it, same pattern as
# TCC's libtcc.c used to be. No configure step, and no OS-specific
# handling needed -- plain portable C99, confirmed building identically
# with both native gcc and a mingw-w64 cross compiler.
# -------------------------------------------------------------------------
$(VENDOR_DIR)/SQLite/libsqlite3.a:
	$(CC) -std=c99 -O2 -c $(VENDOR_DIR)/SQLite/sqlite3.c -o $(VENDOR_DIR)/SQLite/sqlite3.o \
		-DSQLITE_THREADSAFE=1
	ar rcs $(VENDOR_DIR)/SQLite/libsqlite3.a $(VENDOR_DIR)/SQLite/sqlite3.o

.PHONY: sqlite
sqlite: $(VENDOR_DIR)/SQLite/libsqlite3.a

# -------------------------------------------------------------------------
# Tcl -- real configure && make. Linux/macOS: vendor/TCL/unix, produces
# libtcl9.0.a + tclsh. Windows (MSYS2/MinGW): vendor/TCL/win, produces
# libtcl90.a + libtclstub.a + tclsh90s.exe -- a completely different
# build tree from unix/, not the same one running under MSYS2's shell.
#
# CC is passed into configure explicitly (`CC="$(CC)"`) rather than
# left for autoconf to detect on its own: some minimal shells/toolkits
# (e.g. w64devkit's bundled sh) don't give configure's own compiler
# probe ("checking for gcc... no / checking for cc... no / ...") the
# same PATH the outer make invocation sees, even though $(CC) itself
# works fine for every other target in this file.
# -------------------------------------------------------------------------
$(TCL_LIB):
	$(TCL_CONFIGURE)
	$(MAKE) -C $(TCL_BUILD_DIR)

.PHONY: tcl
tcl: $(TCL_LIB)

# -------------------------------------------------------------------------
# Tk -- needs Tcl built first (--with-tcl points at it).
#
# Linux/macOS: vendor/TK/unix, needs real X11 development headers/libs
# on the build machine (libx11-dev on Debian/Ubuntu). Produces
# libtcl9tk9.0.a + wish.
#
# NOTE: on some systems Tk's configure fails to auto-detect X11 even
# with libx11-dev installed ("checking for X11 header files... couldn't
# find any!"). If that happens, re-run configure by hand with explicit
# paths, e.g. on Debian/Ubuntu:
#   cd vendor/TK/unix && sh ./configure --disable-shared \
#     --with-tcl=$(pwd)/../../TCL/unix \
#     --x-includes=/usr/include --x-libraries=/usr/lib/x86_64-linux-gnu
# then `make -C vendor/TK/unix`. Left as a manual fallback rather than
# hardcoded here since the right paths vary by distro/arch.
#
# Windows (MSYS2/MinGW): vendor/TK/win, needs NO X11 at all -- it talks
# to native Win32 windowing (GDI/User32/comctl32) and resolves Tk's
# internal `#include <X11/Xlib.h>` through its own vendor/TK/xlib/ shim
# header rather than a real Xlib. Produces libtcl9tk90.a + libtkstub.a
# + wish90s.exe.
# -------------------------------------------------------------------------
$(TK_LIB): $(TCL_LIB)
	$(TK_CONFIGURE)
	$(MAKE) -C $(TK_BUILD_DIR)

.PHONY: tk
tk: $(TK_LIB)

.PHONY: vendor-clean
vendor-clean:
	$(MAKE) -C $(TCL_BUILD_DIR) distclean 2>/dev/null || true
	$(MAKE) -C $(TK_BUILD_DIR) distclean 2>/dev/null || true
	rm -f $(VENDOR_DIR)/SQLite/sqlite3.o $(VENDOR_DIR)/SQLite/libsqlite3.a

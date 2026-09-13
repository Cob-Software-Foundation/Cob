# =========================================================================
# Makefile -- Project Obsidian Falcon / Cob Language Toolchain
# =========================================================================
# `make` (the default `all` target) builds:
#   cob_interp_full - the interpreter, with SQLite (sql_open, sql_exec,
#                   sql_query, sql_close) AND _cobwindow (window_open,
#                   window_label, window_wait, window_close, backed by
#                   raylib) built in. This builds SQLite and raylib
#                   from vendor/ itself the first time (see the
#                   cob_interp_full/sqlite/raylib target comments
#                   below) -- no configure/autoconf step for either,
#                   just plain Makefiles, so this is fast (well under a
#                   minute on a clean tree). Needs real X11 + OpenGL
#                   dev headers on Linux/macOS for the _cobwindow piece
#                   (libx11-dev libgl1-mesa-dev libxrandr-dev
#                   libxinerama-dev libxcursor-dev libxi-dev on
#                   Debian/Ubuntu); Windows needs nothing extra beyond
#                   a normal MinGW/w64devkit install.
#
#                   Tcl/Tk are NOT part of this binary -- see
#                   `make cob_interp_db` below if you specifically want
#                   tcl_eval()/tk_eval(), which do need a much heavier
#                   from-source Tcl/Tk build.
#   popcorn_comp  - the native compiler (transpiles to C, spawns a real
#                   C compiler -- see src/popcorn_comp.c's header
#                   comment for why this isn't a statically-linked TCC,
#                   and how it picks which compiler to spawn)
#   farmer        - the package manager
#
# popcorn_comp and farmer have no TCC/src-tcc build dependency -- both
# are plain, dependency-free C99 that build standalone. If you want
# the interpreter without SQLite/_cobwindow at all -- no vendor build,
# no X11/OpenGL dependency, builds in under a second -- use
# `make cob_interp` instead of plain `make`; see its target comment
# below.
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
    TCL_CONFIGURE  = cd $(TCL_BUILD_DIR) && CC="$(CC)" EGREP_TRADITIONAL="grep -E" sh ./configure --disable-shared --enable-64bit
    TK_CONFIGURE   = cd $(TK_BUILD_DIR) && CC="$(CC)" EGREP_TRADITIONAL="grep -E" sh ./configure --disable-shared --enable-64bit \
                      --with-tcl=$(abspath $(TCL_BUILD_DIR))
    # _cobwindow (v0.0.5): backed by raylib (vendor/raylib), built via
    # its own plain Makefile -- no configure/autoconf, unlike Tcl/Tk.
    # Link flags match raylib's own Makefile's Windows/GLFW-static case.
    WINDOW_LIBS    = -lopengl32 -lgdi32 -lwinmm -lshell32
    RAYLIB_BUILD_DIR = $(VENDOR_DIR)/raylib/src
    RAYLIB_LIB       = $(RAYLIB_BUILD_DIR)/libraylib.a
    # SQLite's amalgamation is a single portable sqlite3.c, but the
    # compiled .a is NOT portable across toolchains/architectures --
    # give it an OS-specific name so switching between Windows and
    # Linux/macOS builds (e.g. `make sqlite` then later cross-compiling
    # with a different CC) can't silently link the wrong-architecture
    # object against the wrong-platform Tcl/Tk. Without this, `sqlite`
    # being a real file target (see below) means Make sees the old
    # library already exists and skips rebuilding it, even though it
    # was built for a different platform.
    SQLITE_LIB     = $(VENDOR_DIR)/SQLite/libsqlite3-win.a
    SQLITE_OBJ     = $(VENDOR_DIR)/SQLite/sqlite3-win.o
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
    TCL_CONFIGURE  = cd $(TCL_BUILD_DIR) && CC="$(CC)" EGREP_TRADITIONAL="grep -E" sh ./configure --disable-shared --prefix=/tmp/tclinstall
    TK_CONFIGURE   = cd $(TK_BUILD_DIR) && CC="$(CC)" EGREP_TRADITIONAL="grep -E" sh ./configure --disable-shared \
                      --with-tcl=$(abspath $(TCL_BUILD_DIR)) --prefix=/tmp/tkinstall
    # _cobwindow (v0.0.5): backed by raylib (vendor/raylib) instead of
    # raw Xlib -- needs the same libx11-dev / X11 dev headers the `tk`
    # target's comment covers (plus GL dev headers -- libgl1-mesa-dev
    # on Debian/Ubuntu), but raylib itself builds via a plain Makefile,
    # no configure/autoconf. Link flags match raylib's own Makefile's
    # Linux/GLFW case (LDLIBS with X11 appended).
    WINDOW_LIBS    = -lGL -lm -lpthread -ldl -lrt -lX11
    RAYLIB_BUILD_DIR = $(VENDOR_DIR)/raylib/src
    RAYLIB_LIB       = $(RAYLIB_BUILD_DIR)/libraylib.a
    # See the Windows branch's comment above for why this is
    # OS-specific rather than a shared vendor/SQLite/libsqlite3.a.
    SQLITE_LIB     = $(VENDOR_DIR)/SQLite/libsqlite3-unix.a
    SQLITE_OBJ     = $(VENDOR_DIR)/SQLite/sqlite3-unix.o
endif

.PHONY: all clean cob_interp cob_interp_db cob_interp_window cob_interp_full popcorn_comp farmer smartpass

all: cob_interp_full popcorn_comp farmer

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
# ^ The lightweight, dependency-free interpreter: no SQLite/Tcl/Tk,
# no _cobwindow, no vendor build, no X11 dependency, builds in under a
# second. Plain `make` builds `cob_interp_full` instead (see below) --
# use `make cob_interp` explicitly if that's what you want.

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
cob_interp_db: $(BIN_DIR) $(SQLITE_LIB) $(TCL_LIB) $(TK_LIB)
	$(CC) $(CFLAGS) $(INCLUDE) \
		-DCOB_WITH_SQLITE -DCOB_WITH_TCL -DCOB_WITH_TK $(DB_DEFINES) \
		-I$(VENDOR_DIR)/SQLite \
		$(TCL_DB_INCLUDE) \
		$(TK_DB_INCLUDE) \
		-o $(BIN_DIR)/cob_interp_db$(EXE_SUF) \
		src/file_io.c src/cob_interp.c \
		$(SQLITE_LIB) \
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
# window_close) compiled in, backed by raylib (vendor/raylib). Needs
# `make raylib` first -- or nothing at all, since this target lists
# $(RAYLIB_LIB) as a real prerequisite and builds it itself if missing,
# same as cob_interp_db does for sqlite/tcl/tk. raylib builds via a
# plain Makefile (no configure/autoconf), so none of the busybox-ash/
# vendor-tree fragility documented for Tcl/Tk applies here. Needs real
# X11 + OpenGL dev headers on Linux/macOS (libx11-dev libgl1-mesa-dev
# on Debian/Ubuntu, plus libxrandr-dev libxinerama-dev libxcursor-dev
# libxi-dev for GLFW's X11 backend).
# -------------------------------------------------------------------------
.PHONY: cob_interp_window
cob_interp_window: $(BIN_DIR) $(RAYLIB_LIB)
	$(CC) $(CFLAGS) $(INCLUDE) -DCOB_WITH_COBWINDOW \
		-I$(RAYLIB_BUILD_DIR) \
		-o $(BIN_DIR)/cob_interp_window$(EXE_SUF) \
		src/file_io.c src/cob_interp.c \
		$(RAYLIB_LIB) $(WINDOW_LIBS)
	@echo ""
	@echo "Built $(BIN_DIR)/cob_interp_window$(EXE_SUF)."

# -------------------------------------------------------------------------
# cob_interp_full -- SQLite (sql_open/sql_exec/sql_query/sql_close) AND
# _cobwindow (window_open/label/wait/close, backed by raylib) baked
# into one binary. This is what plain `make` builds by default (see
# the `all` target above) -- it lists $(SQLITE_LIB)/$(RAYLIB_LIB) as
# real prerequisites, so it builds both itself the first time, no
# separate build step needed.
#
# Tcl/Tk are deliberately NOT part of this binary. `cob_interp_db`
# (SQLite + Tcl + Tk, including tk_eval()) still exists as a separate,
# explicitly-opt-in target for anyone who wants it and is willing to
# build Tcl/Tk from source -- see its own comment above for the
# busybox-ash/autoconf fragility that comes with that. `cob_interp_full`
# is the "just works, no configure step anywhere" default.
# -------------------------------------------------------------------------
.PHONY: cob_interp_full
cob_interp_full: $(BIN_DIR) $(SQLITE_LIB) $(RAYLIB_LIB)
	$(CC) $(CFLAGS) $(INCLUDE) \
		-DCOB_WITH_SQLITE -DCOB_WITH_COBWINDOW \
		-I$(VENDOR_DIR)/SQLite \
		-I$(RAYLIB_BUILD_DIR) \
		-o $(BIN_DIR)/cob_interp_full$(EXE_SUF) \
		src/file_io.c src/cob_interp.c \
		$(SQLITE_LIB) $(RAYLIB_LIB) $(WINDOW_LIBS)
	@echo ""
	@echo "Built $(BIN_DIR)/cob_interp_full$(EXE_SUF) -- SQLite + _cobwindow, all in one binary."
	@echo "(Tcl/Tk not included -- see \`make cob_interp_db\` if you want tcl_eval()/tk_eval() too.)"

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
# TCC's libtcc.c used to be. No configure step. The source itself is
# portable C99 across native gcc and mingw-w64, but the *compiled*
# output is architecture/toolchain-specific, hence the OS-specific
# $(SQLITE_LIB)/$(SQLITE_OBJ) paths set above rather than one shared
# vendor/SQLite/libsqlite3.a.
# -------------------------------------------------------------------------
$(SQLITE_LIB):
	$(CC) -std=c99 -O2 -c $(VENDOR_DIR)/SQLite/sqlite3.c -o $(SQLITE_OBJ) \
		-DSQLITE_THREADSAFE=1
	ar rcs $(SQLITE_LIB) $(SQLITE_OBJ)

.PHONY: sqlite
sqlite: $(SQLITE_LIB)

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

# -------------------------------------------------------------------------
# raylib -- backs _cobwindow (window_open/label/wait/close). Plain
# Makefile, no configure/autoconf at all, same PLATFORM_DESKTOP build
# on every OS this Makefile supports (its GLFW backend picks Win32 vs
# X11/Wayland vs Cocoa internally). Needs real X11 + OpenGL dev headers
# on Linux (libx11-dev libgl1-mesa-dev libxrandr-dev libxinerama-dev
# libxcursor-dev libxi-dev on Debian/Ubuntu); on Windows, opengl32 and
# friends ship with any standard MinGW/w64devkit install, nothing extra
# to install.
# -------------------------------------------------------------------------
$(RAYLIB_LIB):
	$(MAKE) -C $(RAYLIB_BUILD_DIR) PLATFORM=PLATFORM_DESKTOP CC="$(CC)"

.PHONY: raylib
raylib: $(RAYLIB_LIB)

.PHONY: vendor-clean
vendor-clean:
	$(MAKE) -C $(TCL_BUILD_DIR) distclean 2>/dev/null || true
	$(MAKE) -C $(TK_BUILD_DIR) distclean 2>/dev/null || true
	$(MAKE) -C $(RAYLIB_BUILD_DIR) PLATFORM=PLATFORM_DESKTOP clean 2>/dev/null || true
	rm -f $(SQLITE_OBJ) $(SQLITE_LIB)
	rm -f $(VENDOR_DIR)/SQLite/sqlite3-win.o $(VENDOR_DIR)/SQLite/libsqlite3-win.a
	rm -f $(VENDOR_DIR)/SQLite/sqlite3-unix.o $(VENDOR_DIR)/SQLite/libsqlite3-unix.a

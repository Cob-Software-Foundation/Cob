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

BIN_DIR  = bin

ifeq ($(OS),Windows_NT)
    EXE_SUF = .exe
    SLEEP_CMD = timeout /t 1 >nul
else
    EXE_SUF =
    SLEEP_CMD = sleep 1
endif

.PHONY: all clean cob_interp popcorn_comp farmer smartpass

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
# VENDORED LIBRARIES (vendor/sqlite, vendor/tcl, vendor/tk)
# =========================================================================
# Embedded per project decision -- see each target's comment for what
# "build" means for that library. None of these are wired into Cob's
# language syntax yet (no new keywords); this only makes them
# buildable/linkable. That's a separate design step.
# =========================================================================

VENDOR_DIR = vendor

# -------------------------------------------------------------------------
# SQLite -- the "amalgamation" build: a single vendor/sqlite/sqlite3.c
# that compiles directly into any project needing it, same pattern as
# TCC's libtcc.c used to be. No configure step at all.
# -------------------------------------------------------------------------
.PHONY: sqlite
sqlite:
	$(CC) -std=c99 -O2 -c $(VENDOR_DIR)/sqlite/sqlite3.c -o $(VENDOR_DIR)/sqlite/sqlite3.o \
		-DSQLITE_THREADSAFE=1
	ar rcs $(VENDOR_DIR)/sqlite/libsqlite3.a $(VENDOR_DIR)/sqlite/sqlite3.o

# -------------------------------------------------------------------------
# Tcl -- real configure && make, produces a static libtcl9.0.a plus a
# tclsh binary in vendor/tcl/unix/. Run from inside unix/ (configure
# writes its output relative to the current directory).
# -------------------------------------------------------------------------
.PHONY: tcl
tcl:
	cd $(VENDOR_DIR)/tcl/unix && sh ./configure --disable-shared --prefix=/tmp/tclinstall
	$(MAKE) -C $(VENDOR_DIR)/tcl/unix

# -------------------------------------------------------------------------
# Tk -- needs Tcl built first (--with-tcl points at it) and real X11
# development headers/libs on the build machine (libx11-dev on
# Debian/Ubuntu). Produces libtcl9tk9.0.a plus a wish binary.
# -------------------------------------------------------------------------
.PHONY: tk
tk: tcl
	cd $(VENDOR_DIR)/tk/unix && sh ./configure --disable-shared \
		--with-tcl=$(abspath $(VENDOR_DIR)/tcl/unix) \
		--prefix=/tmp/tkinstall
	$(MAKE) -C $(VENDOR_DIR)/tk/unix

.PHONY: vendor-clean
vendor-clean:
	$(MAKE) -C $(VENDOR_DIR)/tcl/unix distclean 2>/dev/null || true
	$(MAKE) -C $(VENDOR_DIR)/tk/unix distclean 2>/dev/null || true
	rm -f $(VENDOR_DIR)/sqlite/sqlite3.o $(VENDOR_DIR)/sqlite/libsqlite3.a
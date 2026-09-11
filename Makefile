# =========================================================================
# Makefile -- Project Obsidian Falcon / Cob Language Toolchain
# =========================================================================
# Builds:
#   cob_interp    - the interpreter (src/cob_interp.c + src/file_io.c)
#   popcorn_comp  - the native compiler, statically linking src/tcc/libtcc.c
#   farmer        - the package manager
#
# popcorn_comp needs TCC's runtime archive (libtcc1.a) built first --
# `make tcc-runtime` bootstraps TCC's own build system (./configure &&
# make libtcc1.a) inside src/tcc/. This only needs to happen once; it's
# a dependency of `make popcorn_comp`, not of `make cob_interp`.
#
# Platform detection follows the same $(OS)/uname pattern used in
# .github/workflows/build.yml, so this Makefile works unmodified under
# MSYS2/MinGW make on Windows as well as Linux/macOS make.
# =========================================================================

CC      ?= gcc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra
INCLUDE  = -Iinclude

BIN_DIR  = bin
TCC_DIR  = src/tcc

ifeq ($(OS),Windows_NT)
    EXE_SUF = .exe
else
    EXE_SUF =
endif

.PHONY: all clean cob_interp popcorn_comp farmer tcc-runtime

all: cob_interp popcorn_comp farmer

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

# -------------------------------------------------------------------------
# cob_interp -- no TCC dependency at all, builds standalone
# -------------------------------------------------------------------------
cob_interp: $(BIN_DIR)
	$(CC) $(CFLAGS) $(INCLUDE) -o $(BIN_DIR)/cob_interp$(EXE_SUF) \
		src/file_io.c src/cob_interp.c

# -------------------------------------------------------------------------
# TCC runtime (libtcc1.a) -- built once via TCC's own build system, run
# from INSIDE src/tcc (configure writes config.h/config.mak relative to
# the current directory, not the script's location -- always cd first).
# On Windows without MSYS2's `sh`, use src/tcc/win32/build-tcc.bat
# instead (see README) and pass its output dir via TCC_RUNTIME_DIR.
# -------------------------------------------------------------------------
tcc-runtime:
	cd $(TCC_DIR) && sh ./configure --prefix=/tmp/tccinstall
	$(MAKE) -C $(TCC_DIR) libtcc1.a

# -------------------------------------------------------------------------
# popcorn_comp -- statically links src/tcc/libtcc.c as one translation
# unit (the ONE_SOURCE amalgamation TCC itself supports), rather than
# linking a prebuilt libtcc.a, so there is exactly one place (this
# recipe) that decides which TCC_TARGET_* this binary compiles for.
# -------------------------------------------------------------------------
TCC_RUNTIME_DIR ?= $(abspath $(TCC_DIR))

popcorn_comp: $(BIN_DIR) tcc-runtime
	$(CC) $(CFLAGS) $(INCLUDE) -I$(TCC_DIR) \
		-DPOPCORN_TCC_RUNTIME_DIR='"$(TCC_RUNTIME_DIR)"' \
		-o $(BIN_DIR)/popcorn_comp$(EXE_SUF) \
		src/file_io.c src/popcorn_comp.c $(TCC_DIR)/libtcc.c \
		-ldl -lpthread -lm

# -------------------------------------------------------------------------
# farmer -- the package manager (no TCC dependency)
# -------------------------------------------------------------------------
farmer: $(BIN_DIR)
	$(CC) $(CFLAGS) $(INCLUDE) -o $(BIN_DIR)/farmer$(EXE_SUF) \
		src/file_io.c src/farmer.c

clean:
	rm -rf $(BIN_DIR)
	$(MAKE) -C $(TCC_DIR) clean 2>/dev/null || true

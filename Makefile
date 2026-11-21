# =========================================================================
# Makefile -- Project Obsidian Falcon / Cob Language Toolchain (Rust)
# =========================================================================
# A thin wrapper over Cargo that lays the results out the way the rest of
# the project (tests, docs, release scripts) expects them:
#
#   bin/cob_interp, bin/popcorn_comp, bin/farmer
#   bin/lib/                 the STANDARD LIBRARY: one shared library per
#                            module plus the tiny .cob wrapper `shuck <m>`
#                            loads:  porch cornmath kernels almanac granary
#                            pantry (SQLite, from vendor/SQLite)
#
# `make` (the default `all` target) builds the free tools and, when
# keys/drm_pubkeys.txt exists (your PUBLIC key -- safe to commit), the
# license-gated *_enterprise binaries too.
#
#   make test            build everything, then run the regression tests
#   make stdlib-barn     windows (raylib/raygui) -- needs X11 + OpenGL dev
#                        headers on Linux/macOS, so it is opt-in
#   make enterprise      bin/*_enterprise (see ENTERPRISE.md)
#   make cob_sign        the license-signing tool (maintainers only)
#
# Cross-compiling: make TARGET=x86_64-pc-windows-gnu   (needs that Rust
# target and a matching linker installed).
#
# The free build never contains any license code.
# =========================================================================

CARGO     ?= cargo
BIN_DIR    = bin
LIB_DIR    = $(BIN_DIR)/lib
TARGET    ?=

ifeq ($(TARGET),)
    CARGO_TARGET_FLAG =
    OUT_ROOT = target
    UNAME_S ?= $(shell uname -s 2>/dev/null)
    ifeq ($(OS),Windows_NT)
        IS_WINDOWS = 1
    endif
    ifeq ($(UNAME_S),Darwin)
        IS_MAC = 1
    endif
else
    CARGO_TARGET_FLAG = --target $(TARGET)
    OUT_ROOT = target/$(TARGET)
    ifneq ($(findstring windows,$(TARGET)),)
        IS_WINDOWS = 1
    endif
    ifneq ($(findstring apple,$(TARGET)),)
        IS_MAC = 1
    endif
endif

ifeq ($(IS_WINDOWS),1)
    EXE_SUF   = .exe
    SO_PREFIX =
    SO_EXT    = .dll
else ifeq ($(IS_MAC),1)
    EXE_SUF   =
    SO_PREFIX = lib
    SO_EXT    = .dylib
else
    EXE_SUF   =
    SO_PREFIX = lib
    SO_EXT    = .so
endif

REL         = $(OUT_ROOT)/release
STDLIB_MODULES = porch cornmath kernels almanac granary pantry
STDLIB_COB  = $(wildcard stdlib/*.cob)
SLEEP_CMD  ?= sleep 1

ifneq ($(wildcard keys/drm_pubkeys.txt),)
    ALL_ENT = enterprise
else
    ALL_ENT =
endif

.PHONY: all build clean test wasm play wasm-test cob_interp popcorn_comp farmer cob_sign stdlib stdlib-barn vendor-clean \
        cob_interp_full cob_interp_window cob_interp_db enterprise embed embed_enterprise smartpass

all: cob_interp popcorn_comp farmer stdlib $(ALL_ENT)
ifeq ($(ALL_ENT),)
	@echo ""
	@echo "[note] keys/drm_pubkeys.txt not found -- skipped the Enterprise builds (see ENTERPRISE.md)."
endif

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(LIB_DIR):
	mkdir -p $(LIB_DIR)

# One cargo invocation builds every free tool and the stdlib modules.
build:
	$(CARGO) build --release $(CARGO_TARGET_FLAG) --workspace

cob_interp: build | $(BIN_DIR)
	cp $(REL)/cob_interp$(EXE_SUF) $(BIN_DIR)/cob_interp$(EXE_SUF)

popcorn_comp: build | $(BIN_DIR)
	cp $(REL)/popcorn_comp$(EXE_SUF) $(BIN_DIR)/popcorn_comp$(EXE_SUF)

farmer: build | $(BIN_DIR)
	cp $(REL)/farmer$(EXE_SUF) $(BIN_DIR)/farmer$(EXE_SUF)

cob_sign: build | $(BIN_DIR)
	cp $(REL)/cob_sign$(EXE_SUF) $(BIN_DIR)/cob_sign$(EXE_SUF)

# The standard library: shared libraries + their .cob wrappers.
stdlib: build | $(LIB_DIR)
	for m in $(STDLIB_MODULES); do cp $(REL)/$(SO_PREFIX)$$m$(SO_EXT) $(LIB_DIR)/; done
	cp $(STDLIB_COB) $(LIB_DIR)/

# barn (raylib + raygui windows) is opt-in: it needs X11 + OpenGL dev headers
# (Linux) and a display to run, which an ordinary build must not require, so it
# lives outside the main Cargo workspace. Its build script builds vendor/raylib
# with raylib's own Makefile (position-independent) and compiles raygui through
# a one-file C shim.
stdlib-barn: | $(LIB_DIR)
	cd stdlib-rs/barn && $(CARGO) build --release $(CARGO_TARGET_FLAG)
	cp stdlib-rs/barn/target/$(if $(TARGET),$(TARGET)/,)release/$(SO_PREFIX)barn$(SO_EXT) $(LIB_DIR)/

vendor-clean:
	$(MAKE) -C vendor/raylib/src PLATFORM=PLATFORM_DESKTOP clean 2>/dev/null || true

# ---- the browser build (WebAssembly) ---------------------------------------
# docs/play/ runs this same interpreter in a tab: crates/cob-wasm compiled to
# wasm32-unknown-unknown, with porch/cornmath/kernels/almanac linked in and
# every .cob wrapper embedded (a tab has no dlopen() and no files). The page
# talks to it through docs/play/cob_interp.js (hand-written, committed); only
# the .wasm is a build product.
#
#   make wasm        -> bin/wasm/cob_interp.wasm
#   make play        -> same, copied into docs/play/ next to the page
#   make wasm-test   -> build it, then run the parity tests under Node
#
# Needs the wasm32 Rust target (`rustup target add wasm32-unknown-unknown`). On a
# toolchain that lacks its standard library, WASM_CARGO_FLAGS can build it from
# source (RUSTC_BOOTSTRAP=1 WASM_CARGO_FLAGS=-Zbuild-std=std,panic_abort) and
# CARGO_TARGET_WASM32_UNKNOWN_UNKNOWN_LINKER can name a wasm-ld.
WASM_TARGET      = wasm32-unknown-unknown
WASM_CARGO_FLAGS ?=
PLAY_DIR          = docs/play

wasm: | $(BIN_DIR)
	cd crates/cob-wasm && $(CARGO) build --release --target $(WASM_TARGET) $(WASM_CARGO_FLAGS)
	mkdir -p $(BIN_DIR)/wasm
	cp crates/cob-wasm/target/$(WASM_TARGET)/release/cob_wasm.wasm $(BIN_DIR)/wasm/cob_interp.wasm
	@echo "Built $(BIN_DIR)/wasm/cob_interp.wasm"

play: wasm
	cp $(BIN_DIR)/wasm/cob_interp.wasm $(PLAY_DIR)/cob_interp.wasm
	cp VERSION $(PLAY_DIR)/version.txt
	@echo "Copied the sandbox build into $(PLAY_DIR)/ -- serve docs/ with a local web server and open play/."

wasm-test: play cob_interp stdlib
	sh tools/tests/wasm.sh $(CURDIR)/$(BIN_DIR)/cob_interp

# ---- compatibility aliases ------------------------------------------------
# Before v0.0.9 these were three interpreter builds with SQLite or raylib windows
# compiled in. There is one interpreter now; those features are the pantry /
# barn standard-library modules.
cob_interp_full: cob_interp stdlib stdlib-barn
cob_interp_window: cob_interp stdlib-barn
cob_interp_db: cob_interp stdlib

# ---- Enterprise (license-gated) builds --------------------------------------
# bin/cob_interp_enterprise + bin/popcorn_comp_enterprise, built from exactly
# the same source with the `enterprise` feature on. They need a signed,
# machine-bound cob.lic at startup (see ENTERPRISE.md).
enterprise: | $(BIN_DIR) $(LIB_DIR)
	@test -f keys/drm_pubkeys.txt || { echo "enterprise needs keys/drm_pubkeys.txt (your PUBLIC key) -- see ENTERPRISE.md"; exit 1; }
	$(CARGO) build --release $(CARGO_TARGET_FLAG) --target-dir target/enterprise \
		-p cob_interp -p popcorn_comp --features cob_interp/enterprise,popcorn_comp/enterprise
	cp target/enterprise/$(if $(TARGET),$(TARGET)/,)release/cob_interp$(EXE_SUF) $(BIN_DIR)/cob_interp_enterprise$(EXE_SUF)
	cp target/enterprise/$(if $(TARGET),$(TARGET)/,)release/popcorn_comp$(EXE_SUF) $(BIN_DIR)/popcorn_comp_enterprise$(EXE_SUF)
	cp $(BIN_DIR)/cob_interp_enterprise$(EXE_SUF) $(BIN_DIR)/cob_interp_full_enterprise$(EXE_SUF)
	@echo ""
	@echo "Enterprise build done: $(BIN_DIR)/cob_interp_enterprise$(EXE_SUF), $(BIN_DIR)/popcorn_comp_enterprise$(EXE_SUF)"

# The embeddable package with the license-gated interpreter inside:
# bin/cob-<version>-embed_enterprise.zip. The host app must make cob.lic
# reachable (set COB_LICENSE, or keep cob.lic in the working directory).
embed_enterprise:
	$(MAKE) ENT_SUF=_enterprise embed

# ---- the embeddable package ---------------------------------------------------
# A minimal, no-installer, extract-and-run folder meant to be dropped inside
# another application's distribution (see EMBED-README.md): the interpreter
# renamed `cob`, the stdlib, the license text, and VERSION. No farmer, no
# popcorn_comp, no docs -- a host embeds the runtime, not the toolchain.
#
#   ENT_SUF   which interpreter goes in: "" (free) or "_enterprise" (gated);
#             ENTERPRISE=1 is accepted as an alias for ENT_SUF=_enterprise
#   EMBED_SUF the zip's name suffix (defaults to ENT_SUF), e.g. "-linux-amd64"
ifeq ($(ENTERPRISE),1)
    ENT_SUF = _enterprise
endif
ENT_SUF    ?=
# Some Windows runners have no `zip`; 7-Zip makes the same archive.
ifeq ($(shell command -v zip 2>/dev/null),)
    ZIP_CONTENTS = 7z a -tzip -bso0 -bsp0
else
    ZIP_CONTENTS = zip -rq
endif
EMBED_STAGE = $(BIN_DIR)/embed-stage
EMBED_SUF  ?= $(ENT_SUF)
EMBED_ZIP   = $(BIN_DIR)/cob-$(shell cat VERSION)-embed$(EMBED_SUF).zip

embed: cob_interp stdlib $(if $(ENT_SUF),enterprise)
	rm -rf $(EMBED_STAGE) $(EMBED_ZIP)
	mkdir -p $(EMBED_STAGE)/legal $(EMBED_STAGE)/lib
	cp $(LIB_DIR)/* $(EMBED_STAGE)/lib/
	cp $(BIN_DIR)/cob_interp$(ENT_SUF)$(EXE_SUF) $(EMBED_STAGE)/cob$(EXE_SUF)
	cp LICENSE.md $(EMBED_STAGE)/
	cp legal/license_raylib.terms legal/license_raygui.terms $(EMBED_STAGE)/legal/
	cp VERSION $(EMBED_STAGE)/
	cp EMBED-README.md $(EMBED_STAGE)/README.md
	cd $(EMBED_STAGE) && $(ZIP_CONTENTS) "../../$(EMBED_ZIP)" .
	rm -rf $(EMBED_STAGE)
	@echo ""
	@echo "Built $(EMBED_ZIP) -- extract-and-run, no installer, just cob$(EXE_SUF) + license text."

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

test: cob_interp popcorn_comp stdlib
	sh tools/tests/run_tests.sh
	$(CARGO) test --workspace

clean:
	rm -rf $(BIN_DIR) target stdlib-rs/barn/target crates/cob-wasm/target $(PLAY_DIR)/cob_interp.wasm $(PLAY_DIR)/version.txt

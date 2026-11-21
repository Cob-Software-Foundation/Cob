# Cob setup wizard -- a self-extracting installer (egui). See README.md.
#
#   make payload   build ../bin (interpreter, compiler, stdlib) and zip it into payload.zip
#   make           payload.zip -> release installer in dist/:
#                    Windows  Cob-<version>-Setup-windows-amd64.exe
#                    macOS    Cob-<version>-Setup-macos-<arm64|amd64>.dmg  (a Cob Setup.app inside)
#                    Linux    Cob-<version>-Setup-linux
#   make test      the extraction tests (no payload needed)
#   make run       build and launch the installer
#   make clean
#
# Needs a Rust newer than the main workspace's 1.75 MSRV (egui 0.29): the
# installer is its own cargo project, deliberately outside the Cob workspace.

VERSION     := $(shell cat ../VERSION)
PAYLOAD_ZIP := payload.zip
DIST_DIR    := dist
RUST_SRCS   := $(wildcard src/*.rs) $(wildcard src/pages/*.rs) $(wildcard assets/*.png assets/*.ico assets/*.icns)
CARGO       ?= cargo

# Per-platform output. PACKAGE turns the built binary into the thing you ship;
# RUN opens it.
ifeq ($(OS),Windows_NT)
    EXE_EXT    := .exe
    FINAL_NAME := Cob-$(VERSION)-Setup-windows-amd64.exe
    PACKAGE     = cp target/release/$(INSTALLER_BIN) $@
    RUN         = ./$(FINAL_SETUP)
else ifeq ($(shell uname -s),Darwin)
    EXE_EXT    :=
    MAC_LABEL  := $(if $(filter arm64 aarch64,$(shell uname -m)),arm64,amd64)
    FINAL_NAME := Cob-$(VERSION)-Setup-macos-$(MAC_LABEL).dmg
    # wrap it in Cob Setup.app and a .dmg (see tools/package_macos.sh)
    PACKAGE     = tools/package_macos.sh target/release/$(INSTALLER_BIN) $@
    RUN         = open $(FINAL_SETUP)
else
    EXE_EXT    :=
    FINAL_NAME := Cob-$(VERSION)-Setup-linux
    PACKAGE     = cp target/release/$(INSTALLER_BIN) $@
    RUN         = ./$(FINAL_SETUP)
endif

# Some Windows runners have no `zip`; 7-Zip makes the same archive.
ifeq ($(shell command -v zip 2>/dev/null),)
    ZIP_CONTENTS = 7z a -tzip -bso0 -bsp0
else
    ZIP_CONTENTS = zip -rq
endif

INSTALLER_BIN := cob_installer$(EXE_EXT)
FINAL_SETUP   := $(DIST_DIR)/$(FINAL_NAME)

.PHONY: all payload test run clean assets
all: $(FINAL_SETUP)

# The same layout as the release archives (the interpreter is called `cob`),
# but without their wrapper folder.
payload: $(PAYLOAD_ZIP)
$(PAYLOAD_ZIP):
	$(MAKE) -C .. cob_interp popcorn_comp stdlib
	rm -rf stage $(PAYLOAD_ZIP)
	mkdir -p stage/lib stage/legal
	cp ../bin/cob_interp$(EXE_EXT) stage/cob$(EXE_EXT)
	cp ../bin/popcorn_comp$(EXE_EXT) stage/
	cp ../bin/lib/* stage/lib/
	cp ../LICENSE.md ../VERSION ../README.md stage/
	cp ../legal/license_raylib.terms ../legal/license_raygui.terms stage/legal/
	cd stage && $(ZIP_CONTENTS) "../$(PAYLOAD_ZIP)" .
	rm -rf stage
	@echo "Built $(PAYLOAD_ZIP)"

# Rebuilds whenever any source, image, manifest or the payload changes.
target/release/$(INSTALLER_BIN): $(PAYLOAD_ZIP) $(RUST_SRCS) Cargo.toml build.rs
	$(CARGO) build --release

$(FINAL_SETUP): target/release/$(INSTALLER_BIN) assets/Cob.icns tools/package_macos.sh
	mkdir -p $(DIST_DIR)
	$(PACKAGE)
	@echo "Done! Installer ready at $@"

test:
	$(CARGO) test

run: $(FINAL_SETUP)
	$(RUN)

# Regenerate every image in assets/ (png, ico, icns) from assets/source/cob-logo.png
assets:
	python3 tools/make_assets.py

clean:
	$(CARGO) clean
	rm -rf $(DIST_DIR) stage $(PAYLOAD_ZIP)

# Project configuration
APP_NAME := MyApp
PAYLOAD_ZIP := payload.zip
TARGET_DIR := target
DIST_DIR := dist
RUST_SRCS := $(wildcard src/*.rs) $(wildcard src/pages/*.rs)

# OS Detection & Cross-Platform Commands
ifeq ($(OS),Windows_NT)
	SHELL := cmd.exe
	EXE_EXT := .exe
	FINAL_NAME := $(APP_NAME)-Setup.exe
	MKDIR := if not exist "$(DIST_DIR)" mkdir "$(DIST_DIR)"
	CP := copy /Y
	RM := if exist "$(DIST_DIR)" rmdir /s /q "$(DIST_DIR)"
	RUN_CMD := $(subst /,\,$(DIST_DIR)/$(FINAL_NAME))
	FIX_PATH = $(subst /,\,$1)
else
	UNAME_S := $(shell uname -s)
	EXE_EXT :=
	ifeq ($(UNAME_S),Darwin)
		FINAL_NAME := $(APP_NAME)-macOS
	else
		FINAL_NAME := $(APP_NAME)-Linux
	endif
	MKDIR := mkdir -p $(DIST_DIR)
	CP := cp
	RM := rm -rf $(DIST_DIR)
	RUN_CMD := ./$(DIST_DIR)/$(FINAL_NAME)
	FIX_PATH = $1
endif

INSTALLER_BIN := my_app_installer$(EXE_EXT)
FINAL_SETUP := $(DIST_DIR)/$(FINAL_NAME)

# Default target: build release binary and package into dist/
.PHONY: all
all: $(FINAL_SETUP)

# Ensure dist directory exists
$(DIST_DIR):
	@$(MKDIR)

# Verify payload.zip exists before compiling
$(PAYLOAD_ZIP):
	$(error $(PAYLOAD_ZIP) not found! Place your pre-built payload.zip in the project root)

# Compile the Rust installer whenever any .rs file, Cargo.toml, or payload.zip changes
$(TARGET_DIR)/release/$(INSTALLER_BIN): $(PAYLOAD_ZIP) $(RUST_SRCS) Cargo.toml
	@echo "-> Compiling Python-style Rust installer..."
	@cargo build --release

# Copy finished executable to dist/
$(FINAL_SETUP): $(TARGET_DIR)/release/$(INSTALLER_BIN) | $(DIST_DIR)
	@echo "-> Packaging $(FINAL_SETUP)..."
	@$(CP) "$(call FIX_PATH,$<)" "$(call FIX_PATH,$@)"
	@echo "Done! Installer ready at $(FINAL_SETUP)"

# Generate and open Cargo documentation
.PHONY: doc
doc:
	@echo "-> Generating Rust documentation..."
	@cargo doc --no-deps --open

# Quick compile & launch for testing UI changes
.PHONY: run
run: $(FINAL_SETUP)
	@echo "-> Launching $(FINAL_SETUP)..."
	@$(RUN_CMD)

# Clean build outputs (preserves your existing payload.zip)
.PHONY: clean
clean:
	@echo "-> Cleaning build and distribution artifacts..."
	@cargo clean
	@$(RM)
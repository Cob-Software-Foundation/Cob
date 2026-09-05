#!/bin/bash
set -e

# 1. Check Architecture and Assign the Exact Download URL
RAW_ARCH=$(uname -m)

echo -e "\033[0;36m⚙️ Parsing system architecture tokens...\033[0m"

if [ "$RAW_ARCH" = "arm64" ]; then
    echo -e "\033[0;32m✅ Detected System: Apple Silicon (arm64)\033[0m"
    ZIP_NAME="cob-macos-arm64.zip"
    DOWNLOAD_URL="https://github.com/Cob-Software-Foundation/Cob/releases/latest/download/cob-darwin-arm64.zip"
elif [ "$RAW_ARCH" = "x86_64" ]; then
    echo -e "\033[0;32m✅ Detected System: Intel Mac (amd64)\033[0m"
    ZIP_NAME="cob-macos-amd64.zip"
    DOWNLOAD_URL="https://github.com/Cob-Software-Foundation/Cob/releases/latest/download/cob-darwin-amd64.zip"
else
    echo -e "\033[0;33m⚠️ Unknown architecture variable '$RAW_ARCH'. Defaulting to arm64 target.\033[0m"
    ZIP_NAME="cob-macos-arm64.zip"
    DOWNLOAD_URL="https://github.com/Cob-Software-Foundation/Cob/releases/latest/download/cob-darwin-arm64.zip"
fi

# 2. Configure Local Target Paths
INSTALL_DIR="$HOME/.cob"
TEMP_ZIP="/tmp/$ZIP_NAME"

# 3. Purge Existing Directories
if [ -d "$INSTALL_DIR" ]; then
    echo -e "\033[0;33m🧹 Cleansing old installation directory at $INSTALL_DIR...\033[0m"
    rm -rf "$INSTALL_DIR"
fi
mkdir -p "$INSTALL_DIR"

# 4. Fetch the Target Payload
echo -e "\033[0;36m📡 Fetching artifact from: $DOWNLOAD_URL\033[0m"
curl -sSL "$DOWNLOAD_URL" -o "$TEMP_ZIP"

# 5. Extract Tools
echo -e "\033[0;36m📦 Extracting binaries to $INSTALL_DIR...\033[0m"
unzip -q -o "$TEMP_ZIP" -d "$INSTALL_DIR"
rm "$TEMP_ZIP"

# 6. Apply Persistent Path Alterations to Zsh (macOS Default)
SHELL_RC="$HOME/.zshrc"

# Create .zshrc if it doesn't exist
if [ ! -f "$SHELL_RC" ]; then
    touch "$SHELL_RC"
fi

# Inject path line if not already explicitly present
PATH_LINE="export PATH=\"\$PATH:$INSTALL_DIR\""
if ! grep -Fxq "$PATH_LINE" "$SHELL_RC"; then
    echo -e "\033[0;36m⚙️ Injecting Cob binaries into user environment PATH...\033[0m"
    echo "" >> "$SHELL_RC"
    echo "$PATH_LINE" >> "$SHELL_RC"
    echo -e "\033[0;32m🚀 Environment variables saved to $SHELL_RC successfully!\033[0m"
else
    echo -e "\033[0;33mℹ️ Cob binaries are already mapped inside your shell configuration.\033[0m"
fi

echo -e "\n\033[0;32m🎉 Toolchain installation complete!\033[0m"
echo -e "\033[1;35m💡 Restart your terminal session or run 'source ~/.zshrc', then run 'cob_interp' to check your version.\033[0m"

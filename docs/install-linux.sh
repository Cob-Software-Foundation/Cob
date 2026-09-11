#!/bin/bash
set -e

# 1. Check Architecture and Assign the Exact Download URL
RAW_ARCH=$(uname -m)

echo -e "\033[0;36m⚙️ Parsing system architecture tokens...\033[0m"

if [ "$RAW_ARCH" = "x86_64" ]; then
    echo -e "\033[0;32m✅ Detected System: 64-bit Linux (amd64)\033[0m"
    ZIP_NAME="cob-linux-amd64.zip"
    DOWNLOAD_URL="https://github.com/pixel-pulse-labs/Cob/releases/latest/download/cob-linux-amd64.zip"
elif [ "$RAW_ARCH" = "aarch64" ] || [ "$RAW_ARCH" = "arm64" ]; then
    echo -e "\033[0;32m✅ Detected System: 64-bit ARM Linux (arm64)\033[0m"
    ZIP_NAME="cob-linux-arm64.zip"
    DOWNLOAD_URL="https://github.com/pixel-pulse-labs/Cob/releases/latest/download/cob-linux-arm64.zip"
elif [[ "$RAW_ARCH" == armv* ]] || [ "$RAW_ARCH" = "armhf" ] || [ "$RAW_ARCH" = "arm" ]; then
    # Catches armv7l, armv6l, etc., commonly found on 32-bit Single Board Computers
    echo -e "\033[0;32m✅ Detected System: 32-bit ARM Linux (arm32)\033[0m"
    ZIP_NAME="cob-linux-arm32.zip"
    DOWNLOAD_URL="https://github.com/pixel-pulse-labs/Cob/releases/latest/download/cob-linux-arm32.zip"
elif [ "$RAW_ARCH" = "i386" ] || [ "$RAW_ARCH" = "i686" ]; then
    echo -e "\033[0;32m✅ Detected System: 32-bit x86 Linux (386)\033[0m"
    ZIP_NAME="cob-linux-386.zip"
    DOWNLOAD_URL="https://github.com/pixel-pulse-labs/Cob/releases/latest/download/cob-linux-386.zip"
else
    echo -e "\033[0;33m⚠️ Unknown architecture variable '$RAW_ARCH'. Defaulting to amd64 target.\033[0m"
    ZIP_NAME="cob-linux-amd64.zip"
    DOWNLOAD_URL="https://github.com/pixel-pulse-labs/Cob/releases/latest/download/cob-linux-amd64.zip"
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

# 4. Fetch the Target Payload (Requires curl)
if ! command -v curl &> /dev/null; then
    echo -e "\033[0;31m❌ Error: 'curl' is required but not installed. Please install curl and try again.\033[0m"
    exit 1
fi

echo -e "\033[0;36m📡 Fetching artifact from: $DOWNLOAD_URL\033[0m"
curl -sSL "$DOWNLOAD_URL" -o "$TEMP_ZIP"

# 5. Extract Tools (Requires unzip)
if ! command -v unzip &> /dev/null; then
    echo -e "\033[0;31m❌ Error: 'unzip' is required but not installed. Please install unzip and try again.\033[0m"
    rm -f "$TEMP_ZIP"
    exit 1
fi

echo -e "\033[0;36m📦 Extracting binaries to $INSTALL_DIR...\033[0m"
unzip -q -o "$TEMP_ZIP" -d "$INSTALL_DIR"
rm "$TEMP_ZIP"

# Safety net: Explicitly force execution permissions on the extracted binaries
chmod +x "$INSTALL_DIR/cob_interp"
chmod +x "$INSTALL_DIR/popcorn_comp"
chmod +x "$INSTALL_DIR/farmer"

# 6. Apply Persistent Path Alterations to Bash (Linux Default)
SHELL_RC="$HOME/.bashrc"

if [ ! -f "$SHELL_RC" ]; then
    touch "$SHELL_RC"
fi

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
echo -e "\033[1;35m💡 Restart your terminal session or run 'source ~/.bashrc', then run 'cob_interp' to check your version.\033[0m"

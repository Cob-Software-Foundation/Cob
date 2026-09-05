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
    DOWNLOAD_URL=""https://github.com/pixel-pulse-labs/Cob/releases/latest/download/cob-linux-amd64.zip""
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

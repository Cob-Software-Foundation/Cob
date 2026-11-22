#!/usr/bin/env bash
# Packages the release build of the wizard as "Cob Setup.app" inside a .dmg.
#
#   cargo build --release
#   bash tools/package_macos.sh [path/to/cob_installer] [out.dmg]
#
# Run it on macOS (it needs `codesign` and `hdiutil`). Build on the architecture you
# are shipping for: the payload inside the wizard is that architecture's Cob, so
# arm64 and Intel each get their own .dmg.
#
# The app is signed ad hoc (`codesign -s -`), which Apple Silicon requires of every
# executable. That is NOT a Developer ID signature or notarization: until you add
# those, Gatekeeper asks users to approve the app once (right-click > Open).
#
# CODESIGN / HDIUTIL / ARCH can be overridden; the packaging test stubs the first two.
set -euo pipefail
cd "$(dirname "$0")/.."

BIN=${1:-target/release/cob_installer}
VERSION=$(sed -n 's/^version *= *"\(.*\)"/\1/p' Cargo.toml | head -1)
ARCH=${ARCH:-$(uname -m)}                       # arm64 | x86_64
OUT=${2:-dist/Cob-${VERSION}-Setup-macOS-${ARCH}.dmg}
CODESIGN=${CODESIGN:-codesign}
HDIUTIL=${HDIUTIL:-hdiutil}

APP_NAME="Cob Setup"
BUNDLE_ID="dev.pixel-pulse.cob.setup"
MIN_MACOS="11.0"                                 # matches MACOSX_DEPLOYMENT_TARGET in CI

[ -f "$BIN" ] || { echo "package_macos: $BIN not found -- run 'cargo build --release' first" >&2; exit 1; }
[ -f assets/Cob.icns ] || { echo "package_macos: assets/Cob.icns missing -- run 'make assets'" >&2; exit 1; }
[ -n "$VERSION" ] || { echo "package_macos: could not read the version from Cargo.toml" >&2; exit 1; }

STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
APP="$STAGE/$APP_NAME.app"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"

cp "$BIN" "$APP/Contents/MacOS/cob_installer"
chmod 755 "$APP/Contents/MacOS/cob_installer"
cp assets/Cob.icns "$APP/Contents/Resources/Cob.icns"
printf 'APPL????' > "$APP/Contents/PkgInfo"

cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key>               <string>${APP_NAME}</string>
  <key>CFBundleDisplayName</key>        <string>${APP_NAME}</string>
  <key>CFBundleIdentifier</key>         <string>${BUNDLE_ID}</string>
  <key>CFBundleExecutable</key>         <string>cob_installer</string>
  <key>CFBundleIconFile</key>           <string>Cob</string>
  <key>CFBundlePackageType</key>        <string>APPL</string>
  <key>CFBundleSignature</key>          <string>????</string>
  <key>CFBundleInfoDictionaryVersion</key> <string>6.0</string>
  <key>CFBundleShortVersionString</key> <string>${VERSION}</string>
  <key>CFBundleVersion</key>            <string>${VERSION}</string>
  <key>LSMinimumSystemVersion</key>     <string>${MIN_MACOS}</string>
  <key>LSApplicationCategoryType</key>  <string>public.app-category.developer-tools</string>
  <key>NSPrincipalClass</key>           <string>NSApplication</string>
  <key>NSHighResolutionCapable</key>    <true/>
</dict>
</plist>
PLIST

# Ad-hoc signature over the whole bundle (see the note at the top).
"$CODESIGN" --force --deep --sign - "$APP"

mkdir -p "$(dirname "$OUT")"
rm -f "$OUT"
"$HDIUTIL" create -volname "$APP_NAME $VERSION" -srcfolder "$STAGE" -ov -format UDZO "$OUT"
echo "Built $OUT"

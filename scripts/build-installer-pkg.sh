#!/usr/bin/env bash
# Build a macOS .pkg installer for acestep-vst (AU + VST3) from the current build tree.
# Run from repo root after: cmake -B build ... && cmake --build build --config Release
#
# Usage:
#   ./scripts/build-installer-pkg.sh [--sign-plugins] [--version 0.1.0]
#
# Output: release-artefacts/AcestepVST-macOS-Installer.pkg (and zip)
# Install location: /Library/Audio/Plug-Ins/Components, /Library/Audio/Plug-Ins/VST3

set -e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

SIGN_PLUGINS=false
PKG_VERSION="0.1.0"
while [ $# -gt 0 ]; do
  case "$1" in
    --sign-plugins) SIGN_PLUGINS=true; shift ;;
    --version)      PKG_VERSION="$2"; shift 2 ;;
    *) echo "Unknown option: $1"; exit 1 ;;
  esac
done

# Locate built plugins (Xcode or Unix Makefiles)
AU_PATH=$(find build -name "acestep-vst.component" -type d 2>/dev/null | head -1)
VST3_PATH=$(find build -name "acestep-vst.vst3" -type d 2>/dev/null | head -1)
if [ -z "$AU_PATH" ] || [ -z "$VST3_PATH" ]; then
  echo "Error: Plugin artefacts not found. Build first:"
  echo "  cmake -B build -G \"Unix Makefiles\" -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_BUILD_TYPE=Release"
  echo "  cmake --build build --config Release"
  find build -type d \( -name "*.component" -o -name "*.vst3" \) 2>/dev/null || true
  exit 1
fi
echo "Using AU:   $AU_PATH"
echo "Using VST3: $VST3_PATH"

mkdir -p release-artefacts
rm -rf release-artefacts/acestep-vst.component release-artefacts/acestep-vst.vst3
cp -R "$AU_PATH"   "release-artefacts/acestep-vst.component"
cp -R "$VST3_PATH" "release-artefacts/acestep-vst.vst3"

# Optional: ad-hoc sign plugins (for local testing without Developer ID)
if [ "$SIGN_PLUGINS" = true ]; then
  echo "Ad-hoc signing plugin bundles..."
  xcrun codesign --force --sign - --deep "release-artefacts/acestep-vst.component"
  xcrun codesign --force --sign - --deep "release-artefacts/acestep-vst.vst3"
fi

# Zip for manual install
(cd release-artefacts && zip -r "AcestepVST-macOS-AU-VST3.zip" "acestep-vst.component" "acestep-vst.vst3")
echo "Created release-artefacts/AcestepVST-macOS-AU-VST3.zip"

# Prepare pkg payload (same layout as CI)
rm -rf payload
mkdir -p payload/Library/Audio/Plug-Ins/Components
mkdir -p payload/Library/Audio/Plug-Ins/VST3
cp -R "release-artefacts/acestep-vst.component" "payload/Library/Audio/Plug-Ins/Components/"
cp -R "release-artefacts/acestep-vst.vst3" "payload/Library/Audio/Plug-Ins/VST3/"

# Build .pkg
pkgbuild \
  --root payload \
  --identifier com.audiohacking.acestep-vst \
  --version "$PKG_VERSION" \
  --install-location / \
  release-artefacts/AcestepVST-macOS-Installer.pkg

rm -rf payload
echo "Created release-artefacts/AcestepVST-macOS-Installer.pkg (version $PKG_VERSION)"
echo "Install: sudo installer -pkg release-artefacts/AcestepVST-macOS-Installer.pkg -target /"
echo "Or open the .pkg in Finder for GUI install."

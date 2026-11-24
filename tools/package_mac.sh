#!/usr/bin/env bash
# Package a self-contained FloatingIsland.app (Apple Silicon, macOS 13+) that runs without the Vulkan SDK
# or Homebrew:
#   tools/package_mac.sh [out-dir]          -> <out-dir>/FloatingIsland-mac.zip (default: dist/)
# Contents/MacOS/FloatingIsland            Release build, deployment target macOS 13; GLFW and GLM are built
#                                          from source and linked statically (Homebrew's copies require the
#                                          macOS version of the machine that built them)
# Contents/Resources/shaders/*.spv         compiled shaders (found at runtime by src/app/Paths.cpp)
# Contents/Frameworks/libMoltenVK.dylib    linked directly as the Vulkan implementation: a release build uses
#                                          no layers, so the Vulkan loader is not needed
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
OUT="${1:-dist}"; BUILD=build-macpkg
BREW="$(brew --prefix)"; MVK="$BREW/opt/molten-vk/lib/libMoltenVK.dylib"

# Reuse dependency sources another build directory already downloaded (saves the git clones).
reuse=()
for dep in imgui vma glfw glm; do
  for dir in build-release build-mingw build "$BUILD"; do
    src="$ROOT/$dir/_deps/$dep-src"
    if [ -f "$src/CMakeLists.txt" ] || [ -f "$src/imgui.cpp" ]; then
      reuse+=("-DFETCHCONTENT_SOURCE_DIR_$(echo $dep | tr a-z A-Z)=$src"); break
    fi
  done
done
cmake -S . -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 \
  -DCMAKE_DISABLE_FIND_PACKAGE_glfw3=ON -DCMAKE_DISABLE_FIND_PACKAGE_glm=ON -DBUILD_SHARED_LIBS=OFF \
  -DVulkan_INCLUDE_DIR="$BREW/opt/vulkan-headers/include" -DVulkan_LIBRARY="$MVK" \
  -DCMAKE_CXX_FLAGS="-isystem $BREW/opt/vulkan-utility-libraries/include" "${reuse[@]}" >/dev/null
ninja -C "$BUILD" > "$BUILD/ninja.log" 2>&1 || { grep -E "error|FAILED" "$BUILD/ninja.log" | head -20 >&2; exit 1; }

APP="$OUT/FloatingIsland.app"; rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Frameworks" "$APP/Contents/Resources/shaders"
cp "$BUILD/FloatingIsland" "$APP/Contents/MacOS/"
cp "$BUILD"/shaders/*.spv "$APP/Contents/Resources/shaders/"
cp -L "$MVK" "$APP/Contents/Frameworks/libMoltenVK.dylib"; chmod u+w "$APP/Contents/Frameworks/libMoltenVK.dylib"
install_name_tool -id "@rpath/libMoltenVK.dylib" "$APP/Contents/Frameworks/libMoltenVK.dylib"
mvk_ref="$(otool -L "$APP/Contents/MacOS/FloatingIsland" | awk '/libMoltenVK/{print $1}')"
install_name_tool -change "$mvk_ref" "@rpath/libMoltenVK.dylib" "$APP/Contents/MacOS/FloatingIsland"
# Drop the build-tree rpaths CMake added (e.g. /opt/homebrew/lib): they are searched first and would make
# the app silently load Homebrew's copies on a Mac that has them, hiding a broken bundle.
otool -l "$APP/Contents/MacOS/FloatingIsland" | awk '/LC_RPATH/{getline; getline; print $2}' | while read -r rp; do
  install_name_tool -delete_rpath "$rp" "$APP/Contents/MacOS/FloatingIsland"
done
install_name_tool -add_rpath "@executable_path/../Frameworks" "$APP/Contents/MacOS/FloatingIsland"

cat > "$APP/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleExecutable</key><string>FloatingIsland</string>
  <key>CFBundleIdentifier</key><string>com.zfw.floatingisland</string>
  <key>CFBundleName</key><string>FloatingIsland</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>1.0</string>
  <key>LSMinimumSystemVersion</key><string>13.0</string>
  <key>NSHighResolutionCapable</key><true/>
</dict></plist>
PLIST

# Any leftover Homebrew path means a library would be missing on another Mac; check the minimum macOS too.
if otool -L "$APP/Contents/MacOS/FloatingIsland" "$APP"/Contents/Frameworks/*.dylib | grep -q "$BREW"; then
  echo "error: Homebrew paths left in the bundle:" >&2
  otool -L "$APP/Contents/MacOS/FloatingIsland" "$APP"/Contents/Frameworks/*.dylib | grep "$BREW" >&2; exit 1
fi
for f in "$APP/Contents/MacOS/FloatingIsland" "$APP"/Contents/Frameworks/*.dylib; do
  minos="$(otool -l "$f" | awk '/LC_BUILD_VERSION/{b=1} b&&/minos/{print $2; exit}')"
  if [ "${minos%%.*}" -gt 13 ]; then echo "error: $f needs macOS $minos" >&2; exit 1; fi
done
codesign --force --deep --sign - "$APP"   # ad-hoc signature: required for arm64 code after install_name_tool
(cd "$OUT" && rm -f FloatingIsland-mac.zip && ditto -c -k --keepParent FloatingIsland.app FloatingIsland-mac.zip)
echo "packaged $OUT/FloatingIsland-mac.zip ($(du -h "$OUT/FloatingIsland-mac.zip" | cut -f1))"

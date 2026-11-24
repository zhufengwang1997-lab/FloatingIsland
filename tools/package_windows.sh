#!/usr/bin/env bash
# Cross-compile a Windows x64 package on macOS with mingw-w64
# (brew install mingw-w64 vulkan-headers vulkan-utility-libraries vulkan-loader):
#   tools/package_windows.sh [out-dir]      -> <out-dir>/FloatingIsland-windows.zip (default: dist/)
# FloatingIsland.exe + shaders/*.spv. The C++ runtime is linked statically; vulkan-1.dll comes with the
# GPU driver on Windows, so only an import library is generated here (from the loader's exported vk* names).
# Native Windows builds (MSVC) are described in docs/windows.md.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
OUT="${1:-dist}"; BUILD=build-mingw
BREW="$(brew --prefix)"
mkdir -p "$BUILD/vulkan"

# Import library for vulkan-1.dll.
{ echo "LIBRARY vulkan-1.dll"; echo "EXPORTS"
  nm -gU "$BREW/opt/vulkan-loader/lib/libvulkan.1.dylib" | awk '$3 ~ /^_vk/ {print substr($3, 2)}' | sort -u
} > "$BUILD/vulkan/vulkan-1.def"
x86_64-w64-mingw32-dlltool -d "$BUILD/vulkan/vulkan-1.def" -l "$BUILD/vulkan/libvulkan-1.a"

# Reuse dependency sources another build directory already downloaded (saves the git clones).
reuse=()
for dep in imgui vma glfw glm; do
  for dir in build-release build "$BUILD"; do
    src="$ROOT/$dir/_deps/$dep-src"
    if [ -f "$src/CMakeLists.txt" ] || [ -f "$src/imgui.cpp" ]; then
      reuse+=("-DFETCHCONTENT_SOURCE_DIR_$(echo $dep | tr a-z A-Z)=$src"); break
    fi
  done
done

cmake -S . -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=tools/cross/mingw-w64.cmake \
  -DVulkan_INCLUDE_DIR="$BREW/opt/vulkan-headers/include" \
  -DVulkan_LIBRARY="$ROOT/$BUILD/vulkan/libvulkan-1.a" "${reuse[@]}" \
  -DCMAKE_CXX_FLAGS="-isystem $BREW/opt/vulkan-utility-libraries/include" >/dev/null
ninja -C "$BUILD" > "$BUILD/ninja.log" 2>&1 || { grep -E "error|FAILED" "$BUILD/ninja.log" | head -20 >&2; exit 1; }

PKG="$OUT/FloatingIsland-windows"; rm -rf "$PKG"; mkdir -p "$PKG/shaders"
cp "$BUILD/FloatingIsland.exe" "$PKG/"
x86_64-w64-mingw32-strip "$PKG/FloatingIsland.exe"
cp "$BUILD"/shaders/*.spv "$PKG/shaders/"
# Fail if the exe needs a DLL that a stock Windows + GPU driver does not have.
deps="$(x86_64-w64-mingw32-objdump -p "$PKG/FloatingIsland.exe" | awk '/DLL Name/{print tolower($3)}' | sort -u)"
echo "DLL imports: $(echo $deps)"
for d in $deps; do
  case "$d" in
    vulkan-1.dll|kernel32.dll|user32.dll|gdi32.dll|shell32.dll|advapi32.dll|ole32.dll|msvcrt.dll|imm32.dll|\
    ucrtbase.dll|api-ms-win-*|dwmapi.dll|winmm.dll|ntdll.dll|bcrypt.dll|ws2_32.dll) ;;
    *) echo "error: unexpected DLL dependency $d" >&2; exit 1 ;;
  esac
done
(cd "$OUT" && rm -f FloatingIsland-windows.zip && zip -qr FloatingIsland-windows.zip FloatingIsland-windows)
echo "packaged $OUT/FloatingIsland-windows.zip ($(du -h "$OUT/FloatingIsland-windows.zip" | cut -f1))"

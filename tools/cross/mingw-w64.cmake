# Cross-compile FloatingIsland for Windows x64 on macOS / Linux with mingw-w64 (used by tools/package_windows.sh).
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)
# Only look for libraries / packages for the target (never the host's Homebrew GLFW or GLM);
# programs (DXC) still come from the host.
execute_process(COMMAND x86_64-w64-mingw32-gcc -print-sysroot OUTPUT_VARIABLE _sysroot OUTPUT_STRIP_TRAILING_WHITESPACE)
set(CMAKE_FIND_ROOT_PATH "${_sysroot}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
# No libstdc++ / libgcc / winpthread DLLs next to the exe.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -static-libgcc -static-libstdc++")

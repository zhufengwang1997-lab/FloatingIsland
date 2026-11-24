#pragma once

#include <string>

// Where the compiled shaders and the pipeline cache live.
// Development builds use the build directory (FI_SHADER_DIR). A packaged copy carries its shaders with it:
// <exe dir>/shaders on Windows, FloatingIsland.app/Contents/Resources/shaders on macOS; it then keeps the
// pipeline cache in the system temp directory, because the install location may be read-only.
const std::string& shaderDir();
const std::string& pipelineCachePath();

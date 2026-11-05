cmake_minimum_required(VERSION 3.25)
project(FloatingIsland LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
include(FetchContent)
find_package(Vulkan 1.3 REQUIRED)

# GLFW and GLM: use an installed package when there is one (Homebrew on macOS),
# otherwise download and build it (e.g. on Windows).
find_package(glfw3 3.4 CONFIG QUIET)
if(NOT glfw3_FOUND)
  set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(glfw GIT_REPOSITORY https://github.com/glfw/glfw.git GIT_TAG 3.4 SYSTEM)
  FetchContent_MakeAvailable(glfw)
endif()

find_package(glm 1.0 CONFIG QUIET)
if(NOT glm_FOUND)
  FetchContent_Declare(glm GIT_REPOSITORY https://github.com/g-truc/glm.git GIT_TAG 1.0.3 SYSTEM)
  FetchContent_MakeAvailable(glm)
endif()

# Vulkan Memory Allocator (header-only) is not packaged by Homebrew, so always fetch it.
FetchContent_Declare(vma
  GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
  GIT_TAG v3.5.0
  GIT_SHALLOW TRUE
  SYSTEM)
FetchContent_MakeAvailable(vma)

# DXC compiles HLSL to SPIR-V. Looks in the Vulkan SDK first, then the local source build.
find_program(DXC_EXECUTABLE dxc
  HINTS "$ENV{VULKAN_SDK}/bin" "$ENV{HOME}/dev/deps/dxc-src/build/bin"
  REQUIRED)

# ---------------------------------------------------------------------------
# Shaders: every shaders/<name>.hlsl provides VSMain and PSMain
# and becomes build/shaders/<name>.vs.spv and <name>.ps.spv
# ---------------------------------------------------------------------------
set(SHADER_OUT_DIR "${CMAKE_BINARY_DIR}/shaders")
set(SHADER_OUTPUTS "")

function(add_hlsl_shader source stage entry)
  get_filename_component(name "${source}" NAME_WE)
  set(output "${SHADER_OUT_DIR}/${name}.${stage}.spv")
  add_custom_command(
    OUTPUT "${output}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${SHADER_OUT_DIR}"
    COMMAND "${DXC_EXECUTABLE}" -spirv -fspv-target-env=vulkan1.3
            -T ${stage}_6_0 -E ${entry}
            "$<IF:$<CONFIG:Debug>,-Zi;-Od,-O3>"
            -Fo "${output}" "${CMAKE_SOURCE_DIR}/${source}"
    DEPENDS "${CMAKE_SOURCE_DIR}/${source}"
    COMMENT "DXC ${source} (${entry}) -> ${name}.${stage}.spv"
    COMMAND_EXPAND_LISTS
    VERBATIM)
  set(SHADER_OUTPUTS ${SHADER_OUTPUTS} "${output}" PARENT_SCOPE)
endfunction()

add_hlsl_shader(shaders/mesh.hlsl vs VSMain)
add_hlsl_shader(shaders/mesh.hlsl ps PSMain)
add_custom_target(shaders ALL DEPENDS ${SHADER_OUTPUTS})

# ---------------------------------------------------------------------------
# Executable
# ---------------------------------------------------------------------------
add_executable(FloatingIsland
  src/main.cpp
  src/app/App.cpp
  src/rhi/VulkanContext.cpp
  src/rhi/Swapchain.cpp
  src/rhi/Resources.cpp
  src/rhi/VmaImpl.cpp
  src/renderer/Renderer.cpp
  src/scene/Camera.cpp
  src/scene/IslandMesh.cpp)

add_dependencies(FloatingIsland shaders)
target_include_directories(FloatingIsland PRIVATE src)
target_link_libraries(FloatingIsland PRIVATE Vulkan::Vulkan glfw glm::glm GPUOpen::VulkanMemoryAllocator)
target_compile_definitions(FloatingIsland PRIVATE
  GLFW_INCLUDE_NONE
  GLM_FORCE_RADIANS
  GLM_FORCE_DEPTH_ZERO_TO_ONE   # Vulkan depth range is [0, 1], not OpenGL's [-1, 1]
  FI_SHADER_DIR="${SHADER_OUT_DIR}"
  $<$<CONFIG:Debug>:FI_ENABLE_VALIDATION>)

# VMA's implementation file is third-party code; don't hold it to our warning level.
if(NOT MSVC)
  set_source_files_properties(src/rhi/VmaImpl.cpp PROPERTIES COMPILE_OPTIONS "-w")
endif()

if(MSVC)
  target_compile_options(FloatingIsland PRIVATE /W4 /utf-8)
else()
  # VkFoo info{VK_STRUCTURE_TYPE_FOO} zero-initializes the rest on purpose; don't warn about it.
  target_compile_options(FloatingIsland PRIVATE -Wall -Wextra -Wpedantic -Wno-missing-field-initializers)
endif()

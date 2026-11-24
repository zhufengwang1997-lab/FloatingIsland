#include "app/Paths.h"

#include <filesystem>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace fs = std::filesystem;

namespace {

fs::path executableDir() {
#if defined(_WIN32)
    std::vector<wchar_t> buf(32768);
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0) return {};
    return fs::path(std::wstring(buf.data(), n)).parent_path();
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buf(size + 1);
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return {};
    std::error_code ec;
    const fs::path p = fs::weakly_canonical(fs::path(buf.data()), ec);
    return (ec ? fs::path(buf.data()) : p).parent_path();
#else
    std::error_code ec;
    return fs::read_symlink("/proc/self/exe", ec).parent_path();
#endif
}

struct Located {
    std::string shaders;
    std::string cache;
};

Located locate() {
    const fs::path exe = executableDir();
    std::error_code ec;
    for (const fs::path& candidate : {exe / "shaders", exe / ".." / "Resources" / "shaders"}) {
        if (fs::exists(candidate / "mesh.vs.spv", ec)) {
            const fs::path cache = fs::temp_directory_path(ec) / "FloatingIsland_pipeline_cache.bin";
            return {candidate.lexically_normal().string(), cache.string()};
        }
    }
    return {FI_SHADER_DIR, std::string(FI_SHADER_DIR) + "/../pipeline_cache.bin"};
}

const Located& located() {
    static const Located l = locate();
    return l;
}

}  // namespace

const std::string& shaderDir() { return located().shaders; }
const std::string& pipelineCachePath() { return located().cache; }

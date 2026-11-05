#include "rhi/PipelineCache.h"

#include "rhi/VkCheck.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

namespace {

// The header every pipeline cache blob starts with (Vulkan spec, VkPipelineCacheHeaderVersionOne).
bool headerMatches(const std::vector<char>& data, const VkPhysicalDeviceProperties& props) {
    VkPipelineCacheHeaderVersionOne header{};
    if (data.size() < sizeof(header)) return false;
    std::memcpy(&header, data.data(), sizeof(header));
    return header.headerSize >= sizeof(header) && header.headerVersion == VK_PIPELINE_CACHE_HEADER_VERSION_ONE &&
           header.vendorID == props.vendorID && header.deviceID == props.deviceID &&
           std::memcmp(header.pipelineCacheUUID, props.pipelineCacheUUID, VK_UUID_SIZE) == 0;
}

}  // namespace

void PipelineCache::init(VkDevice device, VkPhysicalDevice physicalDevice, std::string path) {
    device_ = device;
    path_ = std::move(path);

    std::vector<char> data;
    if (std::ifstream file{path_, std::ios::binary}) {
        data.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physicalDevice, &props);
    if (!data.empty() && !headerMatches(data, props)) {
        std::printf("Pipeline cache: %s is from another GPU or driver, starting empty\n", path_.c_str());
        data.clear();
    }

    VkPipelineCacheCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    info.initialDataSize = data.size();
    info.pInitialData = data.empty() ? nullptr : data.data();
    VK_CHECK(vkCreatePipelineCache(device_, &info, nullptr, &cache_));
    loadedBytes_ = data.size();
}

void PipelineCache::save() const {
    if (cache_ == VK_NULL_HANDLE) return;
    size_t size = 0;
    VK_CHECK(vkGetPipelineCacheData(device_, cache_, &size, nullptr));
    std::vector<char> data(size);
    VK_CHECK(vkGetPipelineCacheData(device_, cache_, &size, data.data()));
    data.resize(size);

    // Write to a temporary file and rename it, so a crash mid-write can't leave a truncated cache.
    const std::string temp = path_ + ".tmp";
    {
        std::ofstream file{temp, std::ios::binary | std::ios::trunc};
        if (!file.write(data.data(), static_cast<std::streamsize>(data.size()))) {
            std::fprintf(stderr, "Pipeline cache: cannot write %s\n", temp.c_str());
            return;
        }
    }
    if (std::rename(temp.c_str(), path_.c_str()) != 0) {
        std::fprintf(stderr, "Pipeline cache: cannot replace %s\n", path_.c_str());
    }
}

void PipelineCache::destroy() {
    if (cache_ != VK_NULL_HANDLE) vkDestroyPipelineCache(device_, cache_, nullptr);
    cache_ = VK_NULL_HANDLE;
}

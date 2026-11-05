#pragma once

#include <vulkan/vulkan.h>

#include <cstddef>
#include <string>

// A VkPipelineCache saved to disk between runs. Creating a pipeline compiles its shaders for this
// GPU (on MoltenVK: SPIR-V -> Metal Shading Language -> Metal's own compiler); the cache keeps the
// results, so the next run can skip that work.
// The file is only used when its header matches this GPU and driver (vendor, device, cache UUID);
// otherwise we start with an empty cache, as a stale or foreign one can't be used.
class PipelineCache {
public:
    void init(VkDevice device, VkPhysicalDevice physicalDevice, std::string path);
    // Writes the cache back to the file (call after all pipelines have been created).
    void save() const;
    void destroy();

    VkPipelineCache handle() const { return cache_; }
    size_t loadedBytes() const { return loadedBytes_; }  // 0 when starting empty

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkPipelineCache cache_ = VK_NULL_HANDLE;
    std::string path_;
    size_t loadedBytes_ = 0;
};

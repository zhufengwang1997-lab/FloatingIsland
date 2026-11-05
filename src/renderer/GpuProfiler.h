#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>

class VulkanContext;

// The parts of a frame that get their own GPU timer.
enum class GpuScope : uint32_t { Frame, Shadow, Main, Ui, Count };
const char* gpuScopeName(GpuScope scope);

// Measures GPU time per scope with timestamp queries.
// Each frame in flight has its own block of queries; a block is read back right after that frame's
// fence has signalled (so the results are ready without stalling), then reset and reused.
class GpuProfiler {
public:
    static constexpr uint32_t kSlots = 2;  // = frames in flight

    void init(const VulkanContext& ctx);
    void shutdown(VkDevice device);

    // Call after waiting on slot's fence: folds the previous use of this slot into the averages.
    void collect(VkDevice device, uint32_t slot);
    // Call at the start of the slot's command buffer, outside any rendering.
    void reset(VkCommandBuffer cmd, uint32_t slot);
    void begin(VkCommandBuffer cmd, uint32_t slot, GpuScope scope);
    void end(VkCommandBuffer cmd, uint32_t slot, GpuScope scope);

    bool supported() const { return supported_; }
    // Exponentially smoothed GPU time of the scope, in milliseconds.
    float milliseconds(GpuScope scope) const { return smoothedMs_[static_cast<uint32_t>(scope)]; }

private:
    static constexpr uint32_t kScopes = static_cast<uint32_t>(GpuScope::Count);
    static constexpr uint32_t kQueriesPerSlot = kScopes * 2;  // begin + end
    uint32_t query(uint32_t slot, GpuScope scope, bool isEnd) const {
        return slot * kQueriesPerSlot + static_cast<uint32_t>(scope) * 2 + (isEnd ? 1 : 0);
    }

    VkQueryPool pool_ = VK_NULL_HANDLE;
    bool supported_ = false;
    double nsPerTick_ = 1.0;
    uint64_t validMask_ = ~0ull;
    bool used_[kSlots][kScopes]{};  // scope was written in the slot's last frame
    float smoothedMs_[kScopes]{};
};

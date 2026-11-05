#include "renderer/GpuProfiler.h"

#include "rhi/VkCheck.h"
#include "rhi/VulkanContext.h"

#include <vector>

const char* gpuScopeName(GpuScope scope) {
    switch (scope) {
        case GpuScope::Frame: return "Frame";
        case GpuScope::Ibl: return "IBL update";
        case GpuScope::Shadow: return "Shadow pass";
        case GpuScope::Main: return "Main pass";
        case GpuScope::Composite: return "Composite";
        case GpuScope::Ui: return "UI";
        default: return "?";
    }
}

void GpuProfiler::init(const VulkanContext& ctx) {
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(ctx.physicalDevice, &props);

    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(ctx.physicalDevice, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(ctx.physicalDevice, &familyCount, families.data());
    const uint32_t validBits = families[ctx.graphicsQueueFamily].timestampValidBits;

    supported_ = validBits > 0 && props.limits.timestampPeriod > 0.0f;
    if (!supported_) return;
    nsPerTick_ = props.limits.timestampPeriod;
    validMask_ = validBits >= 64 ? ~0ull : ((1ull << validBits) - 1);

    VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount = kSlots * kQueriesPerSlot;
    VK_CHECK(vkCreateQueryPool(ctx.device, &info, nullptr, &pool_));
}

void GpuProfiler::shutdown(VkDevice device) {
    if (pool_) vkDestroyQueryPool(device, pool_, nullptr);
    pool_ = VK_NULL_HANDLE;
}

void GpuProfiler::collect(VkDevice device, uint32_t slot) {
    if (!supported_) return;
    bool any = false;
    for (bool u : used_[slot]) any |= u;
    if (!any) return;  // slot not used yet: its queries have never been reset or written
    uint64_t ticks[kQueriesPerSlot]{};
    // The slot's fence has signalled, so every query written in that frame is available.
    const VkResult r = vkGetQueryPoolResults(device, pool_, slot * kQueriesPerSlot, kQueriesPerSlot,
                                             sizeof(ticks), ticks, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
    if (r != VK_SUCCESS && r != VK_NOT_READY) VK_CHECK(r);
    if (r != VK_SUCCESS) return;
    for (uint32_t s = 0; s < kScopes; ++s) {
        lastMs_[s] = 0.0f;
        if (!used_[slot][s]) continue;
        const uint64_t begin = ticks[s * 2] & validMask_;
        const uint64_t end = ticks[s * 2 + 1] & validMask_;
        if (end < begin) continue;  // counter wrapped; skip this sample
        const float ms = static_cast<float>(static_cast<double>(end - begin) * nsPerTick_ * 1e-6);
        lastMs_[s] = ms;
        smoothedMs_[s] = smoothedMs_[s] == 0.0f ? ms : smoothedMs_[s] * 0.95f + ms * 0.05f;
    }
    ++sampleCount_;
}

void GpuProfiler::reset(VkCommandBuffer cmd, uint32_t slot) {
    if (!supported_) return;
    vkCmdResetQueryPool(cmd, pool_, slot * kQueriesPerSlot, kQueriesPerSlot);
    for (bool& u : used_[slot]) u = false;
}

// ALL_COMMANDS: the timestamp is written once all earlier commands have finished, so consecutive
// scopes measure the passes one after another instead of overlapping.
void GpuProfiler::begin(VkCommandBuffer cmd, uint32_t slot, GpuScope scope) {
    if (!supported_) return;
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, pool_, query(slot, scope, false));
}

void GpuProfiler::end(VkCommandBuffer cmd, uint32_t slot, GpuScope scope) {
    if (!supported_) return;
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, pool_, query(slot, scope, true));
    used_[slot][static_cast<uint32_t>(scope)] = true;
}

#pragma once

#include "rhi/Swapchain.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

class VulkanContext;
struct GLFWwindow;

// Records and submits one frame at a time, with up to kFramesInFlight frames queued on the GPU.
class Renderer {
public:
    void init(VulkanContext& ctx, GLFWwindow* window);
    void shutdown();
    void drawFrame();
    void onFramebufferResized() { framebufferResized_ = true; }

private:
    static constexpr uint32_t kFramesInFlight = 2;

    // Everything the CPU must not touch again until the GPU has finished that frame.
    struct FrameData {
        VkCommandPool commandPool = VK_NULL_HANDLE;
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        VkSemaphore imageAcquired = VK_NULL_HANDLE;  // swapchain image is ready to be drawn into
        VkFence inFlight = VK_NULL_HANDLE;           // GPU has finished this frame's commands
    };

    void createFrameResources();
    void createPresentSemaphores();
    void destroyPresentSemaphores();
    void createPipeline();
    void recreateSwapchain();
    void recordCommands(VkCommandBuffer cmd, uint32_t imageIndex);

    VulkanContext* ctx_ = nullptr;
    GLFWwindow* window_ = nullptr;
    Swapchain swapchain_;

    FrameData frames_[kFramesInFlight];
    uint32_t frameIndex_ = 0;
    // One per swapchain image: presentation may still be reading a semaphore after its frame's fence
    // signals, so these can't be reused per frame in flight.
    std::vector<VkSemaphore> renderFinished_;

    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    bool framebufferResized_ = false;
};

#pragma once

#include "rhi/Resources.h"
#include "rhi/Swapchain.h"

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

class Camera;
class VulkanContext;
struct GLFWwindow;
struct MeshData;

struct GpuMesh {
    Buffer vertices;
    Buffer indices;
    uint32_t indexCount = 0;
};

// Records and submits one frame at a time, with up to kFramesInFlight frames queued on the GPU.
class Renderer {
public:
    void init(VulkanContext& ctx, GLFWwindow* window);
    void shutdown();
    void loadMesh(const MeshData& mesh);
    void drawFrame(const Camera& camera);
    void onFramebufferResized() { framebufferResized_ = true; }

    glm::vec3 sunDirection = glm::normalize(glm::vec3(0.5f, 0.8f, 0.3f));

private:
    static constexpr uint32_t kFramesInFlight = 2;
    static constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;

    // Everything the CPU must not touch again until the GPU has finished that frame.
    struct FrameData {
        VkCommandPool commandPool = VK_NULL_HANDLE;
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        VkSemaphore imageAcquired = VK_NULL_HANDLE;  // swapchain image is ready to be drawn into
        VkFence inFlight = VK_NULL_HANDLE;           // GPU has finished this frame's commands
    };

    // Matches PushConstants in shaders/mesh.hlsl.
    struct PushConstants {
        glm::mat4 viewProj;
        glm::vec4 sunDirection;
    };

    void createFrameResources();
    void createPresentSemaphores();
    void destroyPresentSemaphores();
    void createDepthImage();
    void createPipeline();
    void recreateSwapchain();
    void recordCommands(VkCommandBuffer cmd, uint32_t imageIndex, const PushConstants& constants);

    VulkanContext* ctx_ = nullptr;
    GLFWwindow* window_ = nullptr;
    Swapchain swapchain_;
    Image depth_;  // one is enough: frames on the same queue are ordered by the depth barrier

    FrameData frames_[kFramesInFlight];
    uint32_t frameIndex_ = 0;
    // One per swapchain image: presentation may still be reading a semaphore after its frame's fence
    // signals, so these can't be reused per frame in flight.
    std::vector<VkSemaphore> renderFinished_;

    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    GpuMesh mesh_;
    bool framebufferResized_ = false;
};

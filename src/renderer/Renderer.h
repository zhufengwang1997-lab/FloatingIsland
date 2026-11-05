#pragma once

#include "renderer/GpuProfiler.h"
#include "rhi/Resources.h"
#include "rhi/Swapchain.h"

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
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

// Counts from the last recorded frame, for the debug UI.
struct FrameStats {
    uint32_t drawCalls = 0;
    uint64_t triangles = 0;
};

// Draws the scene in three passes per frame:
//   1. shadow pass: scene depth from the sun into a shadow map
//   2. main pass:   PBR shading into the swapchain image, sampling the shadow map
//   3. UI pass:     Dear ImGui draw data on top (if ImGui::Render() was called this frame)
// Up to kFramesInFlight frames are queued on the GPU at once.
class Renderer {
public:
    void init(VulkanContext& ctx, GLFWwindow* window);
    void shutdown();
    void addObject(const MeshData& mesh, const glm::mat4& model = glm::mat4(1.0f));
    // The sphere that must fit inside the shadow map.
    void setShadowBounds(glm::vec3 center, float radius);
    void drawFrame(const Camera& camera);
    void onFramebufferResized() { swapchainDirty_ = true; }
    // Takes effect at the end of the current frame (the swapchain is recreated).
    void setVsync(bool enabled);
    bool vsync() const { return vsync_; }
    // Saves the next frame as a binary PPM image (BGRA swapchains only).
    void requestCapture(const std::string& path) { capturePath_ = path; }

    glm::vec3 sunDirection = glm::normalize(glm::vec3(0.6f, 0.5f, 0.35f));
    glm::vec3 sunColor = glm::vec3(1.0f, 0.96f, 0.90f) * 3.5f;
    float exposure = 1.0f;
    // Shadow pass depth bias (dynamic state, so it can be tuned live without new pipelines).
    float shadowBiasConstant = 1.25f;
    float shadowBiasSlope = 2.0f;

    const GpuProfiler& profiler() const { return profiler_; }
    const FrameStats& stats() const { return stats_; }
    const Swapchain& swapchain() const { return swapchain_; }

private:
    static constexpr uint32_t kFramesInFlight = 2;
    static constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
    static constexpr uint32_t kShadowMapSize = 2048;

    // Matches FrameData in shaders/common.hlsli.
    struct FrameUniforms {
        glm::mat4 viewProj;
        glm::mat4 lightViewProj;
        glm::vec4 cameraPosition;
        glm::vec4 sunDirection;
        glm::vec4 sunColor;
        glm::vec4 params;  // x: exposure, y: 1 / shadow map size
    };

    // Everything the CPU must not touch again until the GPU has finished that frame.
    struct FrameData {
        VkCommandPool commandPool = VK_NULL_HANDLE;
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        VkSemaphore imageAcquired = VK_NULL_HANDLE;  // swapchain image is ready to be drawn into
        VkFence inFlight = VK_NULL_HANDLE;           // GPU has finished this frame's commands
        Buffer uniforms;                             // FrameUniforms, persistently mapped
        VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    };

    struct RenderObject {
        GpuMesh mesh;
        glm::mat4 model;
    };

    void createFrameResources();
    void createPresentSemaphores();
    void destroyPresentSemaphores();
    void createDepthImage();
    void createShadowResources();
    void createDescriptors();
    void createPipelines();
    void recreateSwapchain();
    glm::mat4 lightViewProj() const;
    void drawObjects(VkCommandBuffer cmd);
    void recordShadowPass(VkCommandBuffer cmd, VkDescriptorSet set);
    void recordMainPass(VkCommandBuffer cmd, uint32_t imageIndex, VkDescriptorSet set);
    void recordUiPass(VkCommandBuffer cmd, uint32_t imageIndex);
    void recordCapture(VkCommandBuffer cmd, VkImage image);
    void writeCapture(const std::string& path);

    VulkanContext* ctx_ = nullptr;
    GLFWwindow* window_ = nullptr;
    Swapchain swapchain_;
    Image depth_;      // one is enough: frames on the same queue are ordered by the depth barrier
    Image shadowMap_;  // likewise shared by all frames
    VkSampler shadowSampler_ = VK_NULL_HANDLE;

    FrameData frames_[kFramesInFlight];
    uint32_t frameIndex_ = 0;
    // One per swapchain image: presentation may still be reading a semaphore after its frame's fence
    // signals, so these can't be reused per frame in flight.
    std::vector<VkSemaphore> renderFinished_;

    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;  // shared by both pipelines
    VkPipeline meshPipeline_ = VK_NULL_HANDLE;
    VkPipeline shadowPipeline_ = VK_NULL_HANDLE;

    std::vector<RenderObject> objects_;
    glm::vec3 shadowCenter_{0.0f};
    float shadowRadius_ = 50.0f;
    bool swapchainDirty_ = false;  // window resized or present mode changed
    bool vsync_ = true;

    GpuProfiler profiler_;
    FrameStats stats_;

    std::string capturePath_;
    Buffer captureBuffer_;  // host-readable copy of one swapchain image
};

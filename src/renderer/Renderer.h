#pragma once

#include "renderer/GpuProfiler.h"
#include "rhi/Resources.h"
#include "rhi/Swapchain.h"
#include "scene/Culling.h"

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <vector>

class Camera;
class VulkanContext;
struct GLFWwindow;
struct InstanceData;
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
    uint32_t chunksVisible = 0;  // main pass, after frustum culling
    uint32_t chunksTotal = 0;
};

// Draws the scene in four passes per frame:
//   1. shadow pass:    scene depth from the sun into a shadow map
//   2. main pass:      PBR shading into an offscreen HDR image, sampling the shadow map
//   3. composite pass: sky + exposure + tonemapping, HDR image -> swapchain image
//   4. UI pass:        Dear ImGui draw data on top (if ImGui::Render() was called this frame)
// Only passes 3 and 4 touch the swapchain image, so waiting for the display to release it (which
// on Metal happens on the GPU) can't stall or skew the timing of the scene passes.
// Up to kFramesInFlight frames are queued on the GPU at once.
class Renderer {
public:
    void init(VulkanContext& ctx, GLFWwindow* window);
    void shutdown();
    // Adds a mesh drawn once. Returns its object index.
    uint32_t addObject(const MeshData& mesh, const glm::mat4& model = glm::mat4(1.0f), std::string name = {});
    // Adds a mesh drawn once per instance. twoSided disables back-face culling (thin geometry such as
    // grass blades). The instances are grouped into square chunks of chunkSize world units; the main
    // pass draws only the chunks inside the camera frustum. Returns its object index.
    uint32_t addInstanced(const MeshData& mesh, const std::vector<InstanceData>& instances, bool twoSided,
                          std::string name, float chunkSize = 8.0f);
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
    float windStrength = 1.0f;
    // >= 0: use this as the animation time (seconds) instead of the clock, for repeatable benchmarks.
    float timeOverride = -1.0f;
    // Benchmarks: record the shadow and main passes this many times per frame, so the GPU stays fully
    // busy (and at a steady clock). The output is the same; timings are totals over all repeats.
    uint32_t sceneRepeat = 1;
    // Skip chunks outside the camera frustum in the main pass.
    bool frustumCulling = true;
    // Debug: keep culling against the frustum of the moment this was switched on, so you can fly
    // out and look at what is (not) being drawn.
    bool freezeCulling = false;

    // Objects, for the debug UI.
    uint32_t objectCount() const { return static_cast<uint32_t>(objects_.size()); }
    const std::string& objectName(uint32_t i) const { return objects_[i].name; }
    uint32_t objectInstances(uint32_t i) const { return objects_[i].instanceCount; }
    uint32_t objectTriangles(uint32_t i) const { return objects_[i].mesh.indexCount / 3 * objects_[i].instanceCount; }
    bool& objectVisible(uint32_t i) { return objects_[i].visible; }
    bool& objectCastsShadow(uint32_t i) { return objects_[i].castsShadow; }
    uint32_t objectChunks(uint32_t i) const { return static_cast<uint32_t>(objects_[i].chunks.size()); }

    const GpuProfiler& profiler() const { return profiler_; }
    const FrameStats& stats() const { return stats_; }
    const Swapchain& swapchain() const { return swapchain_; }

private:
    static constexpr uint32_t kFramesInFlight = 2;
    static constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
    static constexpr VkFormat kHdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    static constexpr uint32_t kShadowMapSize = 2048;

    // Matches FrameData in shaders/common.hlsli.
    struct FrameUniforms {
        glm::mat4 viewProj;
        glm::mat4 lightViewProj;
        glm::mat4 invViewProj;
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

    // A group of instances that are close together, tested against the frustum as one box.
    struct InstanceChunk {
        Aabb bounds;              // without wind sway
        float swayReach = 0.0f;   // how far wind can move the geometry sideways, per unit of windStrength
        uint32_t firstInstance = 0;
        uint32_t instanceCount = 0;
    };
    struct InstanceRange {
        uint32_t firstInstance = 0;
        uint32_t instanceCount = 0;
    };

    struct RenderObject {
        GpuMesh mesh;
        glm::mat4 model{1.0f};
        Buffer instances;  // InstanceData; identityInstance_ when drawn once
        uint32_t instanceCount = 1;
        bool ownsInstances = false;
        bool twoSided = false;
        bool visible = true;
        bool castsShadow = true;  // drawn into the shadow map
        std::string name;
        // Instances sorted by chunk; objects drawn once have a single chunk.
        std::vector<InstanceChunk> chunks;
        // This frame's main-pass draws: runs of consecutive visible chunks, merged into one draw each.
        std::vector<InstanceRange> visibleRanges;
    };

    void createFrameResources();
    void createPresentSemaphores();
    void destroyPresentSemaphores();
    void createRenderTargets();   // depth + HDR, sized like the swapchain
    void destroyRenderTargets();
    void writeHdrDescriptors();   // binding 2 of every frame's set; after the HDR image changes
    void createShadowResources();
    void createDescriptors();
    void createPipelines();
    void recreateSwapchain();
    glm::mat4 lightViewProj() const;
    // shadowPass: the caller has bound the shadow pipeline; otherwise pick mesh / foliage per object.
    void drawObjects(VkCommandBuffer cmd, bool shadowPass);
    // Fills every object's visibleRanges for this frame's main pass.
    void cullObjects(const glm::mat4& viewProj);
    void recordShadowPass(VkCommandBuffer cmd, VkDescriptorSet set);
    void recordMainPass(VkCommandBuffer cmd, VkDescriptorSet set);
    void recordCompositePass(VkCommandBuffer cmd, uint32_t imageIndex, VkDescriptorSet set);
    void recordUiPass(VkCommandBuffer cmd, uint32_t imageIndex);
    void recordCapture(VkCommandBuffer cmd, VkImage image);
    void writeCapture(const std::string& path);

    VulkanContext* ctx_ = nullptr;
    GLFWwindow* window_ = nullptr;
    Swapchain swapchain_;
    Image depth_;      // one is enough: frames on the same queue are ordered by the depth barrier
    Image hdr_;        // linear scene radiance; alpha = 1 where geometry was drawn
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
    VkPipeline foliagePipeline_ = VK_NULL_HANDLE;  // mesh shading without back-face culling
    VkPipeline compositePipeline_ = VK_NULL_HANDLE;
    VkPipeline shadowPipeline_ = VK_NULL_HANDLE;

    std::vector<RenderObject> objects_;
    Buffer identityInstance_;  // one default InstanceData, shared by every non-instanced object
    double startTime_ = 0.0;
    glm::vec3 shadowCenter_{0.0f};
    float shadowRadius_ = 50.0f;
    bool swapchainDirty_ = false;  // window resized or present mode changed
    bool vsync_ = true;
    glm::mat4 cullViewProj_{1.0f};  // the frustum culling tests against (held while freezeCulling)

    GpuProfiler profiler_;
    FrameStats stats_;

    std::string capturePath_;
    Buffer captureBuffer_;  // host-readable copy of one swapchain image
};

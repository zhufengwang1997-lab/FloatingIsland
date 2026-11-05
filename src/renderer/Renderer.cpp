#include "renderer/Renderer.h"

#include "rhi/Pipeline.h"
#include "rhi/VkCheck.h"
#include "rhi/VulkanContext.h"
#include "scene/Camera.h"
#include "scene/Mesh.h"

#include <GLFW/glfw3.h>

#include <imgui.h>
#include <imgui_impl_vulkan.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace {

void transitionImage(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, VkImageLayout oldLayout,
                     VkImageLayout newLayout, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                     VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask = srcStage;
    barrier.srcAccessMask = srcAccess;
    barrier.dstStageMask = dstStage;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {aspect, 0, 1, 0, 1};

    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dep);
}

VkExtent2D framebufferExtent(GLFWwindow* window) {
    int w = 0, h = 0;
    glfwGetFramebufferSize(window, &w, &h);
    return {static_cast<uint32_t>(w), static_cast<uint32_t>(h)};
}

void setViewportAndScissor(VkCommandBuffer cmd, VkExtent2D extent) {
    VkViewport vp{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, extent};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
}

constexpr VkPipelineStageFlags2 kDepthStages =
    VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
constexpr VkAccessFlags2 kDepthReadWrite =
    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

}  // namespace

// ---- Setup and teardown ---------------------------------------------------------------------------

void Renderer::init(VulkanContext& ctx, GLFWwindow* window) {
    ctx_ = &ctx;
    window_ = window;
    swapchain_.create(ctx, framebufferExtent(window), vsync_);
    createRenderTargets();
    createShadowResources();
    createFrameResources();
    createPresentSemaphores();
    createDescriptors();
    writeHdrDescriptors();
    createPipelines();
    profiler_.init(ctx);
    const InstanceData identity{};
    identityInstance_ = uploadBuffer(ctx, &identity, sizeof(identity), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    startTime_ = glfwGetTime();
}

void Renderer::shutdown() {
    VkDevice device = ctx_->device;
    profiler_.shutdown(device);
    for (RenderObject& o : objects_) {
        destroyBuffer(ctx_->allocator, o.mesh.vertices);
        destroyBuffer(ctx_->allocator, o.mesh.indices);
        if (o.ownsInstances) destroyBuffer(ctx_->allocator, o.instances);
    }
    objects_.clear();
    destroyBuffer(ctx_->allocator, identityInstance_);
    destroyBuffer(ctx_->allocator, captureBuffer_);
    vkDestroyPipeline(device, meshPipeline_, nullptr);
    vkDestroyPipeline(device, foliagePipeline_, nullptr);
    vkDestroyPipeline(device, compositePipeline_, nullptr);
    vkDestroyPipeline(device, shadowPipeline_, nullptr);
    vkDestroyPipelineLayout(device, pipelineLayout_, nullptr);
    vkDestroyDescriptorPool(device, descriptorPool_, nullptr);  // also frees the sets
    vkDestroyDescriptorSetLayout(device, setLayout_, nullptr);
    destroyPresentSemaphores();
    for (FrameData& f : frames_) {
        destroyBuffer(ctx_->allocator, f.uniforms);
        vkDestroyFence(device, f.inFlight, nullptr);
        vkDestroySemaphore(device, f.imageAcquired, nullptr);
        vkDestroyCommandPool(device, f.commandPool, nullptr);  // also frees its command buffer
        f = FrameData{};
    }
    vkDestroySampler(device, shadowSampler_, nullptr);
    destroyImage(*ctx_, shadowMap_);
    destroyRenderTargets();
    swapchain_.destroy(device);
}

uint32_t Renderer::addObject(const MeshData& mesh, const glm::mat4& model, std::string name) {
    RenderObject o;
    o.mesh.vertices = uploadBuffer(*ctx_, mesh.vertices.data(), mesh.vertices.size() * sizeof(Vertex),
                                   VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    o.mesh.indices = uploadBuffer(*ctx_, mesh.indices.data(), mesh.indices.size() * sizeof(uint32_t),
                                  VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    o.mesh.indexCount = static_cast<uint32_t>(mesh.indices.size());
    o.model = model;
    o.instances = identityInstance_;
    o.name = std::move(name);
    objects_.push_back(std::move(o));
    return static_cast<uint32_t>(objects_.size() - 1);
}

uint32_t Renderer::addInstanced(const MeshData& mesh, const std::vector<InstanceData>& instances, bool twoSided,
                                std::string name) {
    if (instances.empty()) throw std::runtime_error("addInstanced: no instances for " + name);
    const uint32_t index = addObject(mesh, glm::mat4(1.0f), std::move(name));
    RenderObject& o = objects_[index];
    o.instances = uploadBuffer(*ctx_, instances.data(), instances.size() * sizeof(InstanceData),
                               VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    o.instanceCount = static_cast<uint32_t>(instances.size());
    o.ownsInstances = true;
    o.twoSided = twoSided;
    return index;
}

void Renderer::setShadowBounds(glm::vec3 center, float radius) {
    shadowCenter_ = center;
    shadowRadius_ = radius;
}

void Renderer::createFrameResources() {
    VkDevice device = ctx_->device;
    for (FrameData& f : frames_) {
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.queueFamilyIndex = ctx_->graphicsQueueFamily;
        VK_CHECK(vkCreateCommandPool(device, &poolInfo, nullptr, &f.commandPool));

        VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocInfo.commandPool = f.commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(device, &allocInfo, &f.commandBuffer));

        VkSemaphoreCreateInfo semInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(vkCreateSemaphore(device, &semInfo, nullptr, &f.imageAcquired));

        // Created signaled so the first wait on each frame returns immediately.
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VK_CHECK(vkCreateFence(device, &fenceInfo, nullptr, &f.inFlight));

        // One uniform buffer per frame in flight: the CPU writes frame N+1's while the GPU reads frame N's.
        f.uniforms = createBuffer(ctx_->allocator, sizeof(FrameUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                  VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                      VMA_ALLOCATION_CREATE_MAPPED_BIT);
    }
}

void Renderer::createPresentSemaphores() {
    renderFinished_.resize(swapchain_.images.size());
    VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (VkSemaphore& s : renderFinished_) VK_CHECK(vkCreateSemaphore(ctx_->device, &info, nullptr, &s));
}

void Renderer::destroyPresentSemaphores() {
    for (VkSemaphore s : renderFinished_) vkDestroySemaphore(ctx_->device, s, nullptr);
    renderFinished_.clear();
}

void Renderer::createRenderTargets() {
    depth_ = createImage(*ctx_, swapchain_.extent, kDepthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                         VK_IMAGE_ASPECT_DEPTH_BIT);
    hdr_ = createImage(*ctx_, swapchain_.extent, kHdrFormat,
                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
}

void Renderer::destroyRenderTargets() {
    destroyImage(*ctx_, hdr_);
    destroyImage(*ctx_, depth_);
}

void Renderer::writeHdrDescriptors() {
    VkDescriptorImageInfo imageInfo{VK_NULL_HANDLE, hdr_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    for (FrameData& f : frames_) {
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = f.descriptorSet;
        write.dstBinding = 2;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        write.pImageInfo = &imageInfo;
        vkUpdateDescriptorSets(ctx_->device, 1, &write, 0, nullptr);
    }
}

void Renderer::createShadowResources() {
    shadowMap_ = createImage(*ctx_, {kShadowMapSize, kShadowMapSize}, kDepthFormat,
                             VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                             VK_IMAGE_ASPECT_DEPTH_BIT);

    // Comparison sampler: each fetch returns how much of a 2x2 footprint passes the depth test.
    // Clamp-to-edge, not a border color; the shader treats anything outside the map as lit.
    VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    info.magFilter = VK_FILTER_LINEAR;
    info.minFilter = VK_FILTER_LINEAR;
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.compareEnable = VK_TRUE;
    info.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;  // lit when fragment depth <= stored depth
    info.maxLod = 0.0f;
    VK_CHECK(vkCreateSampler(ctx_->device, &info, nullptr, &shadowSampler_));
}

void Renderer::createDescriptors() {
    VkDevice device = ctx_->device;

    // Set 0: binding 0 = FrameUniforms, binding 1 = shadow map + comparison sampler,
    // binding 2 = HDR scene image (read by the composite pass with Load, so no sampler).
    // The comparison sampler is immutable (baked into the layout); MoltenVK prefers it that way.
    VkDescriptorSetLayoutBinding bindings[3]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].pImmutableSamplers = &shadowSampler_;
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = 3;
    layoutInfo.pBindings = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &setLayout_));

    const VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kFramesInFlight},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kFramesInFlight},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kFramesInFlight},
    };
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = kFramesInFlight;
    poolInfo.poolSizeCount = 3;
    poolInfo.pPoolSizes = sizes;
    VK_CHECK(vkCreateDescriptorPool(device, &poolInfo, nullptr, &descriptorPool_));

    for (FrameData& f : frames_) {
        VkDescriptorSetAllocateInfo allocInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocInfo.descriptorPool = descriptorPool_;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &setLayout_;
        VK_CHECK(vkAllocateDescriptorSets(device, &allocInfo, &f.descriptorSet));

        VkDescriptorBufferInfo bufferInfo{f.uniforms.buffer, 0, sizeof(FrameUniforms)};
        VkDescriptorImageInfo imageInfo{VK_NULL_HANDLE, shadowMap_.view,
                                        VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet writes[2]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = f.descriptorSet;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].pBufferInfo = &bufferInfo;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = f.descriptorSet;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo = &imageInfo;
        vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
    }
}

void Renderer::createPipelines() {
    VkDevice device = ctx_->device;

    // Shared layout: set 0 (frame data + shadow map) and the per-object model matrix.
    VkPushConstantRange pushRange{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4)};
    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &setLayout_;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    VK_CHECK(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &pipelineLayout_));

    // Binding 0: mesh vertices. Binding 1: per-instance data, advanced once per instance.
    const std::vector<VkVertexInputBindingDescription> bindings{
        {0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX},
        {1, sizeof(InstanceData), VK_VERTEX_INPUT_RATE_INSTANCE},
    };
    const VkVertexInputAttributeDescription instancePosition{4, 1, VK_FORMAT_R32G32B32A32_SFLOAT,
                                                             offsetof(InstanceData, positionScale)};
    const VkVertexInputAttributeDescription instanceParams{5, 1, VK_FORMAT_R32G32B32A32_SFLOAT,
                                                           offsetof(InstanceData, params)};

    VkShaderModule meshVs = loadShaderModule(device, FI_SHADER_DIR "/mesh.vs.spv");
    VkShaderModule meshPs = loadShaderModule(device, FI_SHADER_DIR "/mesh.ps.spv");
    GraphicsPipelineBuilder mesh;
    mesh.stage(VK_SHADER_STAGE_VERTEX_BIT, meshVs, "VSMain")
        .stage(VK_SHADER_STAGE_FRAGMENT_BIT, meshPs, "PSMain")
        .vertexInput(bindings,
                     {
                         {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
                         {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal)},
                         {2, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, color)},
                         {3, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, material)},
                         instancePosition,
                         instanceParams,
                     })
        .colorFormat(kHdrFormat)
        .depthFormat(kDepthFormat)
        .layout(pipelineLayout_);
    meshPipeline_ = mesh.build(device);
    foliagePipeline_ = mesh.cull(VK_CULL_MODE_NONE).build(device);
    vkDestroyShaderModule(device, meshVs, nullptr);
    vkDestroyShaderModule(device, meshPs, nullptr);

    // Shadow pass: positions only, depth only. The light projection has no Y flip, so faces wind the
    // other way on screen; culling is off anyway, and the depth bias pushes depths back to avoid
    // surfaces shadowing themselves ("shadow acne").
    VkShaderModule shadowVs = loadShaderModule(device, FI_SHADER_DIR "/shadow.vs.spv");
    shadowPipeline_ = GraphicsPipelineBuilder()
                          .stage(VK_SHADER_STAGE_VERTEX_BIT, shadowVs, "VSMain")
                          .vertexInput(bindings, {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
                                                  instancePosition,
                                                  instanceParams})
                          .cull(VK_CULL_MODE_NONE)
                          .depthBias(0.0f, 0.0f)  // enabled; the factors are set per frame
                          .dynamicState(VK_DYNAMIC_STATE_DEPTH_BIAS)
                          .depthFormat(kDepthFormat)
                          .layout(pipelineLayout_)
                          .build(device);
    vkDestroyShaderModule(device, shadowVs, nullptr);

    // Composite: fullscreen triangle generated from the vertex index, no depth, no culling.
    VkShaderModule compositeVs = loadShaderModule(device, FI_SHADER_DIR "/composite.vs.spv");
    VkShaderModule compositePs = loadShaderModule(device, FI_SHADER_DIR "/composite.ps.spv");
    compositePipeline_ = GraphicsPipelineBuilder()
                             .stage(VK_SHADER_STAGE_VERTEX_BIT, compositeVs, "VSMain")
                             .stage(VK_SHADER_STAGE_FRAGMENT_BIT, compositePs, "PSMain")
                             .cull(VK_CULL_MODE_NONE)
                             .depth(false, false)
                             .colorFormat(swapchain_.format)
                             .layout(pipelineLayout_)
                             .build(device);
    vkDestroyShaderModule(device, compositeVs, nullptr);
    vkDestroyShaderModule(device, compositePs, nullptr);
}

void Renderer::recreateSwapchain() {
    // A minimized window has a 0x0 framebuffer; wait until it is visible again.
    VkExtent2D size = framebufferExtent(window_);
    while (size.width == 0 || size.height == 0) {
        if (glfwWindowShouldClose(window_)) return;
        glfwWaitEvents();
        size = framebufferExtent(window_);
    }

    vkDeviceWaitIdle(ctx_->device);
    const VkFormat oldFormat = swapchain_.format;
    destroyPresentSemaphores();
    destroyRenderTargets();
    swapchain_.destroy(ctx_->device);
    swapchain_.create(*ctx_, size, vsync_);
    createRenderTargets();
    writeHdrDescriptors();
    createPresentSemaphores();
    if (swapchain_.format != oldFormat) {
        throw std::runtime_error("Swapchain format changed; pipeline would need rebuilding");
    }
    swapchainDirty_ = false;
}

void Renderer::setVsync(bool enabled) {
    if (enabled == vsync_) return;
    vsync_ = enabled;
    swapchainDirty_ = true;
}

// ---- Per frame ------------------------------------------------------------------------------------

// Orthographic projection along the sun direction that encloses the shadow bounds sphere.
glm::mat4 Renderer::lightViewProj() const {
    const glm::vec3 up = std::abs(sunDirection.y) > 0.99f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
    const float distance = shadowRadius_ * 2.0f;
    const glm::mat4 view = glm::lookAt(shadowCenter_ + sunDirection * distance, shadowCenter_, up);
    const float r = shadowRadius_;
    const glm::mat4 proj = glm::ortho(-r, r, -r, r, distance - r, distance + r);
    return proj * view;
}

void Renderer::drawFrame(const Camera& camera) {
    FrameData& frame = frames_[frameIndex_];
    VkDevice device = ctx_->device;

    // 1. Wait until the GPU is done with the last use of this frame's resources.
    VK_CHECK(vkWaitForFences(device, 1, &frame.inFlight, VK_TRUE, UINT64_MAX));
    // That frame's timestamps are now available too.
    profiler_.collect(device, frameIndex_);

    // 2. Get the next swapchain image.
    uint32_t imageIndex = 0;
    VkResult acquire = vkAcquireNextImageKHR(device, swapchain_.handle, UINT64_MAX, frame.imageAcquired,
                                             VK_NULL_HANDLE, &imageIndex);
    if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
        recreateSwapchain();
        return;
    }
    if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) VK_CHECK(acquire);

    // Reset only once we know we will submit, otherwise the next wait would hang.
    VK_CHECK(vkResetFences(device, 1, &frame.inFlight));

    // 3. Update this frame's uniforms. Safe: the fence says the GPU is no longer reading them.
    const float aspect = static_cast<float>(swapchain_.extent.width) / static_cast<float>(swapchain_.extent.height);
    FrameUniforms uniforms{};
    uniforms.viewProj = camera.projection(aspect) * camera.view();
    uniforms.lightViewProj = lightViewProj();
    uniforms.invViewProj = glm::inverse(uniforms.viewProj);
    uniforms.cameraPosition = glm::vec4(camera.position, 1.0f);
    uniforms.sunDirection = glm::vec4(glm::normalize(sunDirection), 0.0f);
    uniforms.sunColor = glm::vec4(sunColor, 0.0f);
    uniforms.params = glm::vec4(exposure, 1.0f / static_cast<float>(kShadowMapSize),
                                timeOverride >= 0.0f ? timeOverride : static_cast<float>(glfwGetTime() - startTime_),
                                windStrength);
    std::memcpy(frame.uniforms.mapped, &uniforms, sizeof(uniforms));
    VK_CHECK(vmaFlushAllocation(ctx_->allocator, frame.uniforms.allocation, 0, sizeof(uniforms)));

    // 4. Record this frame's commands.
    VkCommandBuffer cmd = frame.commandBuffer;
    VK_CHECK(vkResetCommandPool(device, frame.commandPool, 0));
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &begin));
    const bool capture = !capturePath_.empty();
    stats_ = FrameStats{};
    profiler_.reset(cmd, frameIndex_);
    profiler_.begin(cmd, frameIndex_, GpuScope::Frame);

    const uint32_t repeat = std::max(sceneRepeat, 1u);
    profiler_.begin(cmd, frameIndex_, GpuScope::Shadow);
    for (uint32_t i = 0; i < repeat; ++i) recordShadowPass(cmd, frame.descriptorSet);
    profiler_.end(cmd, frameIndex_, GpuScope::Shadow);

    profiler_.begin(cmd, frameIndex_, GpuScope::Main);
    for (uint32_t i = 0; i < repeat; ++i) recordMainPass(cmd, frame.descriptorSet);
    profiler_.end(cmd, frameIndex_, GpuScope::Main);

    profiler_.begin(cmd, frameIndex_, GpuScope::Composite);
    recordCompositePass(cmd, imageIndex, frame.descriptorSet);
    profiler_.end(cmd, frameIndex_, GpuScope::Composite);

    profiler_.begin(cmd, frameIndex_, GpuScope::Ui);
    recordUiPass(cmd, imageIndex);
    profiler_.end(cmd, frameIndex_, GpuScope::Ui);

    VkImage image = swapchain_.images[imageIndex];
    if (capture) {
        recordCapture(cmd, image);
    } else {
        // Hand the image to the presentation engine; the present waits on our semaphore.
        transitionImage(cmd, image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE);
    }
    profiler_.end(cmd, frameIndex_, GpuScope::Frame);
    VK_CHECK(vkEndCommandBuffer(cmd));

    // 5. Submit: start writing color only after the image is acquired; signal when done.
    VkSemaphoreSubmitInfo waitInfo{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    waitInfo.semaphore = frame.imageAcquired;
    waitInfo.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSemaphoreSubmitInfo signalInfo{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signalInfo.semaphore = renderFinished_[imageIndex];
    signalInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo cmdInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cmdInfo.commandBuffer = cmd;

    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.waitSemaphoreInfoCount = 1;
    submit.pWaitSemaphoreInfos = &waitInfo;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmdInfo;
    submit.signalSemaphoreInfoCount = 1;
    submit.pSignalSemaphoreInfos = &signalInfo;
    VK_CHECK(vkQueueSubmit2(ctx_->graphicsQueue, 1, &submit, frame.inFlight));

    if (capture) {
        // Debug feature, so simply wait for this frame instead of pipelining the readback.
        VK_CHECK(vkWaitForFences(device, 1, &frame.inFlight, VK_TRUE, UINT64_MAX));
    // That frame's timestamps are now available too.
    profiler_.collect(device, frameIndex_);
        writeCapture(capturePath_);
        capturePath_.clear();
    }

    // 6. Present once rendering has finished.
    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &renderFinished_[imageIndex];
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain_.handle;
    present.pImageIndices = &imageIndex;
    VkResult result = vkQueuePresentKHR(ctx_->graphicsQueue, &present);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || swapchainDirty_) {
        recreateSwapchain();
    } else {
        VK_CHECK(result);
    }

    frameIndex_ = (frameIndex_ + 1) % kFramesInFlight;
}

void Renderer::drawObjects(VkCommandBuffer cmd, bool shadowPass) {
    VkPipeline bound = VK_NULL_HANDLE;
    for (const RenderObject& o : objects_) {
        if (!o.visible) continue;
        if (!shadowPass) {
            VkPipeline wanted = o.twoSided ? foliagePipeline_ : meshPipeline_;
            if (wanted != bound) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, wanted);
                bound = wanted;
            }
        }
        ++stats_.drawCalls;
        stats_.triangles += static_cast<uint64_t>(o.mesh.indexCount / 3) * o.instanceCount;
        vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &o.model);
        const VkBuffer buffers[] = {o.mesh.vertices.buffer, o.instances.buffer};
        const VkDeviceSize offsets[] = {0, 0};
        vkCmdBindVertexBuffers(cmd, 0, 2, buffers, offsets);
        vkCmdBindIndexBuffer(cmd, o.mesh.indices.buffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, o.mesh.indexCount, o.instanceCount, 0, 0, 0);
    }
}

void Renderer::recordShadowPass(VkCommandBuffer cmd, VkDescriptorSet set) {
    // The previous frame's main pass may still be sampling the shadow map: wait for those reads
    // before overwriting it.
    transitionImage(cmd, shadowMap_.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, kDepthStages, kDepthReadWrite);

    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth.imageView = shadowMap_.view;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;  // the main pass reads it
    depth.clearValue.depthStencil = {1.0f, 0};

    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = {{0, 0}, shadowMap_.extent};
    rendering.layerCount = 1;
    rendering.pDepthAttachment = &depth;
    vkCmdBeginRendering(cmd, &rendering);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1, &set, 0, nullptr);
    setViewportAndScissor(cmd, shadowMap_.extent);
    vkCmdSetDepthBias(cmd, shadowBiasConstant, 0.0f, shadowBiasSlope);
    drawObjects(cmd, true);
    vkCmdEndRendering(cmd);

    // Depth writes must finish before the main pass samples the map.
    transitionImage(cmd, shadowMap_.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}

void Renderer::recordMainPass(VkCommandBuffer cmd, VkDescriptorSet set) {
    // The HDR image is shared by all frames: the previous frame's composite pass may still be reading
    // it. Previous contents don't matter (we clear), so transition from UNDEFINED.
    transitionImage(cmd, hdr_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    // Likewise the depth image: wait for the previous frame's depth writes first.
    transitionImage(cmd, depth_.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, kDepthStages,
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, kDepthStages, kDepthReadWrite);

    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = hdr_.view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};  // alpha 0 = sky, filled in by the composite pass

    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth.imageView = depth_.view;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;  // not needed after this pass
    depth.clearValue.depthStencil = {1.0f, 0};

    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = {{0, 0}, hdr_.extent};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;
    rendering.pDepthAttachment = &depth;
    vkCmdBeginRendering(cmd, &rendering);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1, &set, 0, nullptr);
    setViewportAndScissor(cmd, hdr_.extent);
    drawObjects(cmd, false);  // binds the mesh / foliage pipeline per object
    vkCmdEndRendering(cmd);

    // Color writes must finish before the composite pass reads the image.
    transitionImage(cmd, hdr_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}

// HDR image -> swapchain image. The first command that writes the swapchain image.
void Renderer::recordCompositePass(VkCommandBuffer cmd, uint32_t imageIndex, VkDescriptorSet set) {
    // Every pixel is overwritten, so the old contents don't matter.
    transitionImage(cmd, swapchain_.images[imageIndex], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = swapchain_.views[imageIndex];
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = {{0, 0}, swapchain_.extent};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;
    vkCmdBeginRendering(cmd, &rendering);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, compositePipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1, &set, 0, nullptr);
    setViewportAndScissor(cmd, swapchain_.extent);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
}

// Draws ImGui on top of the composited image. The image stays in COLOR_ATTACHMENT_OPTIMAL.
void Renderer::recordUiPass(VkCommandBuffer cmd, uint32_t imageIndex) {
    ImDrawData* drawData = ImGui::GetCurrentContext() ? ImGui::GetDrawData() : nullptr;
    if (!drawData) return;

    // Separate rendering scope, so the composite pass's color writes must finish first (write after write).
    transitionImage(cmd, swapchain_.images[imageIndex], VK_IMAGE_ASPECT_COLOR_BIT,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = swapchain_.views[imageIndex];
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;  // keep the scene
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = {{0, 0}, swapchain_.extent};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;
    vkCmdBeginRendering(cmd, &rendering);
    ImGui_ImplVulkan_RenderDrawData(drawData, cmd);  // sets its own pipeline, viewport and scissor
    vkCmdEndRendering(cmd);
}

// Copies the finished swapchain image into captureBuffer_, then hands the image to presentation.
void Renderer::recordCapture(VkCommandBuffer cmd, VkImage image) {
    const VkDeviceSize size = VkDeviceSize{swapchain_.extent.width} * swapchain_.extent.height * 4;
    if (captureBuffer_.size != size) {
        destroyBuffer(ctx_->allocator, captureBuffer_);
        captureBuffer_ = createBuffer(ctx_->allocator, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                      VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    }

    transitionImage(cmd, image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {swapchain_.extent.width, swapchain_.extent.height, 1};
    vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, captureBuffer_.buffer, 1, &region);

    // The CPU reads the buffer after the fence; the image goes on to presentation.
    VkMemoryBarrier2 toHost{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    toHost.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    toHost.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    toHost.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    toHost.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &toHost;
    vkCmdPipelineBarrier2(cmd, &dep);
    transitionImage(cmd, image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_NONE,
                    VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE);
}

void Renderer::writeCapture(const std::string& path) {
    if (swapchain_.format != VK_FORMAT_B8G8R8A8_SRGB && swapchain_.format != VK_FORMAT_B8G8R8A8_UNORM) {
        std::fprintf(stderr, "Capture skipped: unsupported swapchain format\n");
        return;
    }
    VK_CHECK(vmaInvalidateAllocation(ctx_->allocator, captureBuffer_.allocation, 0, captureBuffer_.size));
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) {
        std::fprintf(stderr, "Capture failed: cannot open %s\n", path.c_str());
        return;
    }
    const uint32_t w = swapchain_.extent.width, h = swapchain_.extent.height;
    std::fprintf(file, "P6\n%u %u\n255\n", w, h);
    const auto* bgra = static_cast<const uint8_t*>(captureBuffer_.mapped);
    std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3);
    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
        rgb[i * 3 + 0] = bgra[i * 4 + 2];
        rgb[i * 3 + 1] = bgra[i * 4 + 1];
        rgb[i * 3 + 2] = bgra[i * 4 + 0];
    }
    std::fwrite(rgb.data(), 1, rgb.size(), file);
    std::fclose(file);
    std::printf("Saved %ux%u capture to %s\n", w, h, path.c_str());
}

#include "renderer/Renderer.h"

#include "app/Paths.h"

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
#include <chrono>
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

void memoryBarrier(VkCommandBuffer cmd, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                   VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
    VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    barrier.srcStageMask = srcStage;
    barrier.srcAccessMask = srcAccess;
    barrier.dstStageMask = dstStage;
    barrier.dstAccessMask = dstAccess;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dep);
}

// Whether the IBL cube maps made from sky b still do for sky a: the sun has moved less than 0.2
// degrees and no sky color changed by more than 2% of the sky's brightness. Ambient light changes
// slowly, so with the day cycle running this remakes the maps every few frames instead of every
// frame, without visible steps. (Only what the maps are made from counts; the moon and stars are
// not in them.) The tolerance is relative to the whole sky, not to each color: the sunset glow, for
// one, fades to almost nothing by day, and 1% of almost nothing would be no tolerance at all.
bool closeSky(const SkyState& a, const SkyState& b) {
    const glm::vec3 bright = glm::max(b.zenith, b.horizon);
    const float tolerance = 0.02f * std::max({bright.x, bright.y, bright.z, 0.001f});
    auto close = [&](glm::vec3 x, glm::vec3 y) {
        const glm::vec3 d = glm::abs(x - y);
        return std::max({d.x, d.y, d.z}) <= tolerance;
    };
    return glm::dot(a.sunDirection, b.sunDirection) >= std::cos(glm::radians(0.2f)) && close(a.zenith, b.zenith) &&
           close(a.horizon, b.horizon) && close(a.below, b.below) && close(a.glow, b.glow) &&
           close(a.sunDisk * 0.08f, b.sunDisk * 0.08f);  // only the halo is in the maps (SkyRadiance)
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
    createIblResources();
    createFrameResources();
    createPresentSemaphores();
    createDescriptors();
    createIblDescriptors();
    writeHdrDescriptors();
    // Pipelines compile their shaders for this GPU; the cache from the last run lets them skip that.
    pipelineCache_.init(ctx.device, ctx.physicalDevice, pipelineCachePath());
    const auto pipelinesStart = std::chrono::steady_clock::now();
    createPipelines();
    const double pipelineMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - pipelinesStart).count();
    std::printf("Pipelines: created in %.1f ms (%s)\n", pipelineMs,
                pipelineCache_.loadedBytes() > 0 ? "from the pipeline cache" : "no pipeline cache yet");
    pipelineCache_.save();
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
    pipelineCache_.destroy();
    vkDestroyPipeline(device, meshPipeline_, nullptr);
    vkDestroyPipeline(device, cloudPipeline_, nullptr);
    vkDestroyPipeline(device, foliagePipeline_, nullptr);
    vkDestroyPipeline(device, compositePipeline_, nullptr);
    vkDestroyPipeline(device, shadowPipeline_, nullptr);
    vkDestroyPipeline(device, iblSkyPipeline_, nullptr);
    vkDestroyPipeline(device, iblPrefilterPipeline_, nullptr);
    vkDestroyPipeline(device, iblIrradiancePipeline_, nullptr);
    vkDestroyPipelineLayout(device, iblPipelineLayout_, nullptr);
    vkDestroyDescriptorPool(device, iblPool_, nullptr);
    vkDestroyDescriptorSetLayout(device, iblSetLayout_, nullptr);
    destroyIblResources();
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

uint32_t Renderer::addObject(const MeshData& mesh, const glm::mat4& model, std::string name, float tileSize) {
    RenderObject o;
    std::vector<uint32_t> indices = mesh.indices;
    if (tileSize > 0.0f) {
        // Sort the triangles by the tile their centre falls in, row by row; each tile becomes one
        // contiguous range of the index buffer (drawn with firstIndex / indexCount).
        const size_t triangles = indices.size() / 3;
        std::vector<glm::ivec2> cell(triangles);
        std::vector<uint32_t> order(triangles);
        for (size_t t = 0; t < triangles; ++t) {
            const glm::vec3 centre = (mesh.vertices[indices[3 * t]].position + mesh.vertices[indices[3 * t + 1]].position +
                                      mesh.vertices[indices[3 * t + 2]].position) / 3.0f;
            const glm::vec3 world{model * glm::vec4(centre, 1.0f)};
            cell[t] = glm::ivec2(glm::floor(glm::vec2(world.x, world.z) / tileSize));
            order[t] = static_cast<uint32_t>(t);
        }
        std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
            return cell[a].y != cell[b].y ? cell[a].y < cell[b].y : cell[a].x < cell[b].x;
        });
        indices.clear();
        for (size_t k = 0; k < triangles; ++k) {
            const uint32_t t = order[k];
            if (k == 0 || cell[t] != cell[order[k - 1]]) {
                o.chunks.push_back({{}, 0.0f, {static_cast<uint32_t>(indices.size()), 0, 0, 1}});
            }
            Chunk& chunk = o.chunks.back();
            for (int v = 0; v < 3; ++v) {
                const uint32_t index = mesh.indices[3 * t + v];
                indices.push_back(index);
                chunk.bounds.expand(glm::vec3(model * glm::vec4(mesh.vertices[index].position, 1.0f)));
            }
            chunk.range.indexCount += 3;
        }
    } else {
        Aabb local;
        for (const Vertex& v : mesh.vertices) local.expand(v.position);
        o.chunks.push_back({transformAabb(local, model), 0.0f, {0, static_cast<uint32_t>(indices.size()), 0, 1}});
    }
    o.mesh.vertices = uploadBuffer(*ctx_, mesh.vertices.data(), mesh.vertices.size() * sizeof(Vertex),
                                   VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    o.mesh.indices = uploadBuffer(*ctx_, indices.data(), indices.size() * sizeof(uint32_t),
                                  VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    o.mesh.indexCount = static_cast<uint32_t>(indices.size());
    o.model = model;
    o.instances = identityInstance_;
    o.name = std::move(name);
    objects_.push_back(std::move(o));
    return static_cast<uint32_t>(objects_.size() - 1);
}

uint32_t Renderer::addInstanced(const MeshData& mesh, const std::vector<InstanceData>& instances, bool twoSided,
                                std::string name, float chunkSize, bool distanceLod) {
    if (instances.empty()) throw std::runtime_error("addInstanced: no instances for " + name);
    const uint32_t index = addObject(mesh, glm::mat4(1.0f), std::move(name));  // one chunk, replaced below
    RenderObject& o = objects_[index];

    // The mesh's extent around the instance origin. Instances turn about Y, so sideways it is the
    // largest distance from the Y axis.
    float radius = 0.0f, minY = 1e30f, maxY = -1e30f;
    for (const Vertex& v : mesh.vertices) {
        radius = std::max(radius, glm::length(glm::vec2(v.position.x, v.position.z)));
        minY = std::min(minY, v.position.y);
        maxY = std::max(maxY, v.position.y);
    }

    // Sort the instances by the square cell of the ground they stand on, row by row, so each chunk
    // is one contiguous range of the instance buffer (drawn with firstInstance), and neighbouring
    // chunks in a row are neighbouring ranges (so visible runs merge into one draw).
    auto cellOf = [&](const InstanceData& d) {
        return glm::ivec2(glm::floor(glm::vec2(d.positionScale.x, d.positionScale.z) / chunkSize));
    };
    std::vector<InstanceData> sorted = instances;
    std::stable_sort(sorted.begin(), sorted.end(), [&](const InstanceData& a, const InstanceData& b) {
        const glm::ivec2 ca = cellOf(a), cb = cellOf(b);
        return ca.y != cb.y ? ca.y < cb.y : ca.x < cb.x;
    });

    o.chunks.clear();
    for (uint32_t i = 0; i < sorted.size(); ++i) {
        const InstanceData& d = sorted[i];
        if (i == 0 || cellOf(d) != cellOf(sorted[i - 1])) o.chunks.push_back({{}, 0.0f, {0, o.mesh.indexCount, i, 0}});
        Chunk& chunk = o.chunks.back();
        ++chunk.range.instanceCount;
        const glm::vec3 p{d.positionScale};
        // Distance LOD can grow an instance by up to kMaxLodGrowth (LodScale in the shader).
        const float s = d.positionScale.w * (distanceLod ? kMaxLodGrowth : 1.0f);
        chunk.bounds.expand(p + glm::vec3(-radius * s, minY * s, -radius * s));
        chunk.bounds.expand(p + glm::vec3(radius * s, maxY * s, radius * s));
        // Matches InstanceToWorld in shaders/common.hlsli: sway = wind * y^2 * scale * gust, with
        // |gust| <= 1.3 and a unit-length direction.
        chunk.swayReach = std::max(chunk.swayReach, d.params.z * maxY * maxY * s * 1.3f);
    }

    if (distanceLod) {
        // Shuffle each chunk and rank its instances 0..1: distance LOD keeps a prefix of the chunk
        // (the lowest ranks), and a shuffled prefix is a random, even thinning of the chunk.
        uint32_t state = 0x9E3779B9u;
        auto next = [&]() {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            return state;
        };
        for (const Chunk& c : o.chunks) {
            InstanceData* first = sorted.data() + c.range.firstInstance;
            for (uint32_t i = c.range.instanceCount; i > 1; --i) std::swap(first[i - 1], first[next() % i]);
            for (uint32_t i = 0; i < c.range.instanceCount; ++i) {
                first[i].params.w = static_cast<float>(i) / static_cast<float>(c.range.instanceCount);
            }
        }
    }
    o.distanceLod = distanceLod;
    o.instances = uploadBuffer(*ctx_, sorted.data(), sorted.size() * sizeof(InstanceData),
                               VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    o.instanceCount = static_cast<uint32_t>(sorted.size());
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
    // Bindings 3 and 4: the IBL specular and irradiance cube maps, with the trilinear sampler.
    VkDescriptorSetLayoutBinding bindings[5]{};
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
    for (uint32_t b = 3; b <= 4; ++b) {
        bindings[b].binding = b;
        bindings[b].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[b].descriptorCount = 1;
        bindings[b].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[b].pImmutableSamplers = &envSampler_;
    }
    VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = 5;
    layoutInfo.pBindings = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &setLayout_));

    const VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kFramesInFlight},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3 * kFramesInFlight},
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
        const VkDescriptorImageInfo specularInfo{VK_NULL_HANDLE, specularCube_.view, VK_IMAGE_LAYOUT_GENERAL};
        const VkDescriptorImageInfo irradianceInfo{VK_NULL_HANDLE, irradianceCube_.view, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet writes[4]{};
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
        writes[2] = writes[1];
        writes[2].dstBinding = 3;
        writes[2].pImageInfo = &specularInfo;
        writes[3] = writes[1];
        writes[3].dstBinding = 4;
        writes[3].pImageInfo = &irradianceInfo;
        vkUpdateDescriptorSets(device, 4, writes, 0, nullptr);
    }
}

void Renderer::createIblResources() {
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    envCube_ = createCubeImage(*ctx_, kEnvSize, 1, kIblFormat, usage);
    specularCube_ = createCubeImage(*ctx_, kEnvSize, kSpecularMips, kIblFormat, usage);
    irradianceCube_ = createCubeImage(*ctx_, kIrradianceSize, 1, kIblFormat, usage);
    envTargetView_ = createLayerView(*ctx_, envCube_, 0, 6);
    for (uint32_t m = 0; m < kSpecularMips; ++m) specularTargetViews_[m] = createLayerView(*ctx_, specularCube_, m, 6);
    irradianceTargetView_ = createLayerView(*ctx_, irradianceCube_, 0, 6);

    // Trilinear: between texels and, for the specular cube, between roughness mips. Cube maps are
    // sampled seamlessly across faces in Vulkan, so the address mode hardly matters.
    VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    info.magFilter = VK_FILTER_LINEAR;
    info.minFilter = VK_FILTER_LINEAR;
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.maxLod = static_cast<float>(kSpecularMips - 1);
    VK_CHECK(vkCreateSampler(ctx_->device, &info, nullptr, &envSampler_));

    // Into GENERAL once; they stay there (see kIblFormat).
    ctx_->immediateSubmit([&](VkCommandBuffer cmd) {
        VkImageMemoryBarrier2 barriers[3]{};
        const VkImage images[3] = {envCube_.image, specularCube_.image, irradianceCube_.image};
        for (int i = 0; i < 3; ++i) {
            barriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            barriers[i].srcStageMask = VK_PIPELINE_STAGE_2_NONE;
            barriers[i].dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            barriers[i].dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            barriers[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barriers[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            barriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barriers[i].image = images[i];
            barriers[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, 6};
        }
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 3;
        dep.pImageMemoryBarriers = barriers;
        vkCmdPipelineBarrier2(cmd, &dep);
    });
}

void Renderer::destroyIblResources() {
    VkDevice device = ctx_->device;
    vkDestroySampler(device, envSampler_, nullptr);
    vkDestroyImageView(device, envTargetView_, nullptr);
    for (VkImageView& v : specularTargetViews_) vkDestroyImageView(device, v, nullptr);
    vkDestroyImageView(device, irradianceTargetView_, nullptr);
    destroyImage(*ctx_, envCube_);
    destroyImage(*ctx_, specularCube_);
    destroyImage(*ctx_, irradianceCube_);
}

void Renderer::createIblDescriptors() {
    VkDevice device = ctx_->device;
    // Set 0 of the IBL compute passes: binding 0 = FrameUniforms (for the sky colors), binding 1 =
    // the cube map read (with the trilinear sampler), binding 2 = the mip written.
    VkDescriptorSetLayoutBinding bindings[3]{};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, &envSampler_};
    bindings[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = 3;
    layoutInfo.pBindings = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &iblSetLayout_));

    constexpr uint32_t kSets = kIblSets * kFramesInFlight;
    const VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kSets},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kSets},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kSets},
    };
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = kSets;
    poolInfo.poolSizeCount = 3;
    poolInfo.pPoolSizes = sizes;
    VK_CHECK(vkCreateDescriptorPool(device, &poolInfo, nullptr, &iblPool_));

    // What each pass writes. Every pass reads envCube_ (the sky pass doesn't, but the binding
    // must still hold a valid image).
    VkImageView targets[kIblSets];
    targets[0] = envTargetView_;
    for (uint32_t m = 0; m < kSpecularMips; ++m) targets[1 + m] = specularTargetViews_[m];
    targets[kIblSets - 1] = irradianceTargetView_;

    for (FrameData& f : frames_) {
        VkDescriptorSetLayout layouts[kIblSets];
        for (VkDescriptorSetLayout& l : layouts) l = iblSetLayout_;
        VkDescriptorSetAllocateInfo allocInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocInfo.descriptorPool = iblPool_;
        allocInfo.descriptorSetCount = kIblSets;
        allocInfo.pSetLayouts = layouts;
        VK_CHECK(vkAllocateDescriptorSets(device, &allocInfo, f.iblSets));

        const VkDescriptorBufferInfo bufferInfo{f.uniforms.buffer, 0, sizeof(FrameUniforms)};
        const VkDescriptorImageInfo sourceInfo{VK_NULL_HANDLE, envCube_.view, VK_IMAGE_LAYOUT_GENERAL};
        for (uint32_t i = 0; i < kIblSets; ++i) {
            const VkDescriptorImageInfo targetInfo{VK_NULL_HANDLE, targets[i], VK_IMAGE_LAYOUT_GENERAL};
            VkWriteDescriptorSet writes[3]{};
            for (uint32_t b = 0; b < 3; ++b) {
                writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[b].dstSet = f.iblSets[i];
                writes[b].dstBinding = b;
                writes[b].descriptorCount = 1;
                writes[b].descriptorType = bindings[b].descriptorType;
            }
            writes[0].pBufferInfo = &bufferInfo;
            writes[1].pImageInfo = &sourceInfo;
            writes[2].pImageInfo = &targetInfo;
            vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);
        }
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

    VkShaderModule meshVs = loadShaderModule(device, shaderDir() + "/mesh.vs.spv");
    VkShaderModule meshPs = loadShaderModule(device, shaderDir() + "/mesh.ps.spv");
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
    meshPipeline_ = mesh.build(device, pipelineCache_.handle());
    foliagePipeline_ = mesh.cull(VK_CULL_MODE_NONE).build(device, pipelineCache_.handle());
    vkDestroyShaderModule(device, meshVs, nullptr);
    vkDestroyShaderModule(device, meshPs, nullptr);

    // Shadow pass: positions only, depth only. The light projection has no Y flip, so faces wind the
    // other way on screen; culling is off anyway, and the depth bias pushes depths back to avoid
    // surfaces shadowing themselves ("shadow acne").
    VkShaderModule shadowVs = loadShaderModule(device, shaderDir() + "/shadow.vs.spv");
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
                          .build(device, pipelineCache_.handle());
    vkDestroyShaderModule(device, shadowVs, nullptr);

    // Composite: fullscreen triangle generated from the vertex index, no depth, no culling.
    VkShaderModule compositeVs = loadShaderModule(device, shaderDir() + "/composite.vs.spv");
    VkShaderModule compositePs = loadShaderModule(device, shaderDir() + "/composite.ps.spv");
    compositePipeline_ = GraphicsPipelineBuilder()
                             .stage(VK_SHADER_STAGE_VERTEX_BIT, compositeVs, "VSMain")
                             .stage(VK_SHADER_STAGE_FRAGMENT_BIT, compositePs, "PSMain")
                             .cull(VK_CULL_MODE_NONE)
                             .depth(false, false)
                             .colorFormat(swapchain_.format)
                             .layout(pipelineLayout_)
                             .build(device, pipelineCache_.handle());
    vkDestroyShaderModule(device, compositeVs, nullptr);
    vkDestroyShaderModule(device, compositePs, nullptr);

    // Sea of clouds: a quad generated from the vertex index, depth tested like the scene.
    VkShaderModule cloudsVs = loadShaderModule(device, shaderDir() + "/clouds.vs.spv");
    VkShaderModule cloudsPs = loadShaderModule(device, shaderDir() + "/clouds.ps.spv");
    cloudPipeline_ = GraphicsPipelineBuilder()
                         .stage(VK_SHADER_STAGE_VERTEX_BIT, cloudsVs, "VSMain")
                         .stage(VK_SHADER_STAGE_FRAGMENT_BIT, cloudsPs, "PSMain")
                         .cull(VK_CULL_MODE_NONE)
                         .colorFormat(kHdrFormat)
                         .depthFormat(kDepthFormat)
                         .layout(pipelineLayout_)
                         .build(device, pipelineCache_.handle());
    vkDestroyShaderModule(device, cloudsVs, nullptr);
    vkDestroyShaderModule(device, cloudsPs, nullptr);

    // IBL compute passes: their own set layout, and 16 bytes of push constants (IblParams).
    VkPushConstantRange iblPush{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    VkPipelineLayoutCreateInfo iblLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    iblLayoutInfo.setLayoutCount = 1;
    iblLayoutInfo.pSetLayouts = &iblSetLayout_;
    iblLayoutInfo.pushConstantRangeCount = 1;
    iblLayoutInfo.pPushConstantRanges = &iblPush;
    VK_CHECK(vkCreatePipelineLayout(device, &iblLayoutInfo, nullptr, &iblPipelineLayout_));
    auto compute = [&](const char* file, const char* entry) {
        VkShaderModule module = loadShaderModule(device, shaderDir() + "/" + file);
        VkPipeline pipeline = buildComputePipeline(device, module, entry, iblPipelineLayout_, pipelineCache_.handle());
        vkDestroyShaderModule(device, module, nullptr);
        return pipeline;
    };
    iblSkyPipeline_ = compute("ibl_sky.cs.spv", "CSSky");
    iblPrefilterPipeline_ = compute("ibl_prefilter.cs.spv", "CSPrefilter");
    iblIrradiancePipeline_ = compute("ibl_irradiance.cs.spv", "CSIrradiance");
}

// Remakes the three IBL cube maps from this frame's sky. Before the shadow pass, outside rendering.
void Renderer::recordIblUpdate(VkCommandBuffer cmd, const FrameData& frame) {
    struct Push {
        uint32_t size;
        float roughness;
        uint32_t sampleCount;
        uint32_t pad;
    };
    auto dispatch = [&](VkPipeline pipeline, uint32_t set, Push push) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, iblPipelineLayout_, 0, 1, &frame.iblSets[set], 0,
                                nullptr);
        vkCmdPushConstants(cmd, iblPipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        const uint32_t groups = (push.size + 7) / 8;  // 8 x 8 threads per group, one group row per face
        vkCmdDispatch(cmd, groups, groups, 6);
    };

    // The cube maps are shared by the frames in flight: the previous frame's shaders may still be
    // reading them.
    memoryBarrier(cmd, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    dispatch(iblSkyPipeline_, 0, {kEnvSize, 0.0f, 0, 0});

    // The sky must be written before it is blurred.
    memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    for (uint32_t m = 0; m < kSpecularMips; ++m) {
        const float roughness = static_cast<float>(m) / static_cast<float>(kSpecularMips - 1);
        // The sky (without the sun disk) is smooth, so few samples suffice: 32 per texel here and 64
        // for the irradiance look the same as 64 and 256, at about half the cost.
        dispatch(iblPrefilterPipeline_, 1 + m, {kEnvSize >> m, roughness, 32, 0});
    }
    dispatch(iblIrradiancePipeline_, kIblSets - 1, {kIrradianceSize, 0.0f, 64, 0});

    // Done before the main pass's fragment shaders sample them.
    memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
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
    uniforms.params = glm::vec4(exposure * sky.exposureScale, 1.0f / static_cast<float>(kShadowMapSize),
                                timeOverride >= 0.0f ? timeOverride : static_cast<float>(glfwGetTime() - startTime_),
                                windStrength);
    // Disabled LOD: start beyond any view distance, so every instance keeps its full size.
    lodMinFraction = std::clamp(lodMinFraction, kMinLodFraction, 1.0f);
    uniforms.lod = distanceLod ? glm::vec4(lodStart, std::max(lodEnd, lodStart + 1.0f), lodMinFraction, kLodFade)
                               : glm::vec4(1e9f, 2e9f, 1.0f, kLodFade);
    uniforms.skyZenith = glm::vec4(sky.zenith, sky.night);
    uniforms.skyHorizon = glm::vec4(sky.horizon, 0.0f);
    uniforms.skyBelow = glm::vec4(sky.below, 0.0f);
    uniforms.skyGlow = glm::vec4(sky.glow, 0.0f);
    uniforms.skySun = glm::vec4(sky.sunDirection, 0.0f);
    uniforms.skySunDisk = glm::vec4(sky.sunDisk, 0.0f);
    uniforms.ibl = glm::vec4(iblDiffuse, static_cast<float>(kSpecularMips - 1), iblSpecular, 0.0f);
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
    if (!freezeCulling) cullViewProj_ = uniforms.viewProj;
    cullObjects(cullViewProj_, camera.position);
    profiler_.reset(cmd, frameIndex_);
    profiler_.begin(cmd, frameIndex_, GpuScope::Frame);

    // Timed every frame (empty when nothing changed): the profiler reads all of a frame's queries
    // at once, and one never written would hold up the rest.
    profiler_.begin(cmd, frameIndex_, GpuScope::Ibl);
    if (iblEveryFrame || !iblValid_ || !closeSky(sky, iblSky_)) {
        recordIblUpdate(cmd, frame);
        iblSky_ = sky;
        iblValid_ = true;
        stats_.iblUpdated = true;
        ++iblUpdateCount_;
    }
    profiler_.end(cmd, frameIndex_, GpuScope::Ibl);

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

namespace {

// Appends a chunk's range to a draw list, extending the last draw when the two are neighbours in
// the instance buffer (same triangles, next instances) or in the index buffer (same instances,
// next triangles).
template <typename Range>
void appendDraw(std::vector<Range>& draws, const Range& r) {
    if (!draws.empty()) {
        Range& last = draws.back();
        const bool sameIndices = last.firstIndex == r.firstIndex && last.indexCount == r.indexCount;
        const bool sameInstances = last.firstInstance == r.firstInstance && last.instanceCount == r.instanceCount;
        if (sameIndices && last.firstInstance + last.instanceCount == r.firstInstance) {
            last.instanceCount += r.instanceCount;
            return;
        }
        if (sameInstances && last.firstIndex + last.indexCount == r.firstIndex) {
            last.indexCount += r.indexCount;
            return;
        }
    }
    draws.push_back(r);
}

}  // namespace

float Renderer::lodFraction(float distance) const {
    if (!distanceLod) return 1.0f + kLodFade;
    const float end = std::max(lodEnd, lodStart + 1.0f);
    const float t = std::clamp((distance - lodStart) / (end - lodStart), 0.0f, 1.0f);
    return (1.0f + kLodFade) + t * (lodMinFraction - (1.0f + kLodFade));
}

void Renderer::cullObjects(const glm::mat4& viewProj, glm::vec3 cameraPosition) {
    const Frustum frustum = extractFrustum(viewProj);
    // A chunk's shadow falls away from the sun. Stretch its box that way, far enough to cross the
    // whole shadow volume: if even the stretched box misses the view, so does the shadow.
    const glm::vec3 shadowReach = -glm::normalize(sunDirection) * (4.0f * shadowRadius_);
    for (RenderObject& o : objects_) {
        o.mainDraws.clear();
        o.shadowDraws.clear();
        if (!o.visible) continue;
        stats_.chunksTotal += static_cast<uint32_t>(o.chunks.size());
        if (o.castsShadow) stats_.shadowChunksTotal += static_cast<uint32_t>(o.chunks.size());
        for (const Chunk& c : o.chunks) {
            Aabb box = c.bounds;
            const float sway = c.swayReach * windStrength;
            box.min -= glm::vec3(sway, 0.0f, sway);
            box.max += glm::vec3(sway, 0.0f, sway);
            DrawRange range = c.range;
            if (o.distanceLod) {
                // The nearest point of the box keeps the largest fraction; instances ranked above it
                // have shrunk to nothing everywhere in the chunk, so they need not be drawn at all.
                const glm::vec3 nearest = glm::clamp(cameraPosition, box.min, box.max);
                const float fraction = lodFraction(glm::length(nearest - cameraPosition));
                const auto keep = static_cast<uint32_t>(std::ceil(fraction * static_cast<float>(range.instanceCount)));
                range.instanceCount = std::min(range.instanceCount, keep);
            }
            if (!frustumCulling || intersects(frustum, box)) {
                ++stats_.chunksVisible;
                stats_.instancesDrawn += range.instanceCount;
                appendDraw(o.mainDraws, range);
            }
            if (!o.castsShadow) continue;
            Aabb swept = box;
            swept.expand(Aabb{box.min + shadowReach, box.max + shadowReach});
            if (!frustumCulling || intersects(frustum, swept)) {
                ++stats_.shadowChunksVisible;
                appendDraw(o.shadowDraws, range);
            }
        }
    }
}

void Renderer::drawObjects(VkCommandBuffer cmd, bool shadowPass) {
    VkPipeline bound = VK_NULL_HANDLE;
    for (const RenderObject& o : objects_) {
        if (!o.visible) continue;
        const std::vector<DrawRange>& draws = shadowPass ? o.shadowDraws : o.mainDraws;
        if (draws.empty()) continue;
        if (!shadowPass) {
            VkPipeline wanted = o.twoSided ? foliagePipeline_ : meshPipeline_;
            if (wanted != bound) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, wanted);
                bound = wanted;
            }
        }
        vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &o.model);
        const VkBuffer buffers[] = {o.mesh.vertices.buffer, o.instances.buffer};
        const VkDeviceSize offsets[] = {0, 0};
        vkCmdBindVertexBuffers(cmd, 0, 2, buffers, offsets);
        vkCmdBindIndexBuffer(cmd, o.mesh.indices.buffer, 0, VK_INDEX_TYPE_UINT32);
        for (const DrawRange& d : draws) {
            // firstIndex picks the triangles (a tile), firstInstance where per-instance attributes
            // start in the instance buffer.
            ++stats_.drawCalls;
            stats_.triangles += static_cast<uint64_t>(d.indexCount / 3) * d.instanceCount;
            vkCmdDrawIndexed(cmd, d.indexCount, d.instanceCount, d.firstIndex, 0, d.firstInstance);
        }
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
    if (cloudSea) {
        // After the scene: wherever the island is in front, the depth test skips the cloud shader.
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, cloudPipeline_);
        vkCmdDraw(cmd, 6, 1, 0, 0);
        ++stats_.drawCalls;
        stats_.triangles += 2;
    }
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

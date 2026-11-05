#pragma once

#include <vulkan/vulkan.h>

#include <string>
#include <vector>

// Loads a SPIR-V file produced by DXC at build time.
VkShaderModule loadShaderModule(VkDevice device, const std::string& path);

// Fills in the ~10 structs a graphics pipeline needs, with defaults suited to this renderer:
// triangle lists, dynamic viewport/scissor, back-face culling (CCW front), depth test LESS,
// no blending, dynamic rendering (attachment formats instead of a VkRenderPass).
class GraphicsPipelineBuilder {
public:
    GraphicsPipelineBuilder& stage(VkShaderStageFlagBits stage, VkShaderModule module, const char* entry);
    GraphicsPipelineBuilder& vertexInput(std::vector<VkVertexInputBindingDescription> bindings,
                                         std::vector<VkVertexInputAttributeDescription> attributes);
    GraphicsPipelineBuilder& cull(VkCullModeFlags mode, VkFrontFace front = VK_FRONT_FACE_COUNTER_CLOCKWISE);
    GraphicsPipelineBuilder& depth(bool test, bool write, VkCompareOp compare = VK_COMPARE_OP_LESS);
    GraphicsPipelineBuilder& depthBias(float constantFactor, float slopeFactor);
    // Extra state set with vkCmdSet* at draw time, e.g. VK_DYNAMIC_STATE_DEPTH_BIAS.
    GraphicsPipelineBuilder& dynamicState(VkDynamicState state);
    GraphicsPipelineBuilder& colorFormat(VkFormat format);
    GraphicsPipelineBuilder& depthFormat(VkFormat format);
    GraphicsPipelineBuilder& layout(VkPipelineLayout layout);

    // cache: optional; pipelines found in it are not compiled again.
    VkPipeline build(VkDevice device, VkPipelineCache cache = VK_NULL_HANDLE) const;

private:
    std::vector<VkPipelineShaderStageCreateInfo> stages_;
    std::vector<VkVertexInputBindingDescription> bindings_;
    std::vector<VkVertexInputAttributeDescription> attributes_;
    VkCullModeFlags cullMode_ = VK_CULL_MODE_BACK_BIT;
    VkFrontFace frontFace_ = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    bool depthTest_ = true;
    bool depthWrite_ = true;
    VkCompareOp depthCompare_ = VK_COMPARE_OP_LESS;
    bool depthBias_ = false;
    float biasConstant_ = 0.0f;
    float biasSlope_ = 0.0f;
    std::vector<VkDynamicState> dynamicStates_{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    std::vector<VkFormat> colorFormats_;
    VkFormat depthFormat_ = VK_FORMAT_UNDEFINED;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
};

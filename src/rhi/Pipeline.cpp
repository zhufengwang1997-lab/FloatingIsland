#include "rhi/Pipeline.h"

#include "rhi/VkCheck.h"

#include <fstream>
#include <stdexcept>

VkShaderModule loadShaderModule(VkDevice device, const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("Cannot open shader " + path);
    const auto size = static_cast<size_t>(file.tellg());
    std::vector<uint32_t> code(size / sizeof(uint32_t));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(code.data()), static_cast<std::streamsize>(size));

    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = size;
    info.pCode = code.data();
    VkShaderModule module;
    VK_CHECK(vkCreateShaderModule(device, &info, nullptr, &module));
    return module;
}

VkPipeline buildComputePipeline(VkDevice device, VkShaderModule module, const char* entry, VkPipelineLayout layout,
                                VkPipelineCache cache) {
    VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    info.stage.module = module;
    info.stage.pName = entry;
    info.layout = layout;
    VkPipeline pipeline;
    VK_CHECK(vkCreateComputePipelines(device, cache, 1, &info, nullptr, &pipeline));
    return pipeline;
}

GraphicsPipelineBuilder& GraphicsPipelineBuilder::stage(VkShaderStageFlagBits stage, VkShaderModule module,
                                                        const char* entry) {
    VkPipelineShaderStageCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    info.stage = stage;
    info.module = module;
    info.pName = entry;
    stages_.push_back(info);
    return *this;
}

GraphicsPipelineBuilder& GraphicsPipelineBuilder::vertexInput(
    std::vector<VkVertexInputBindingDescription> bindings, std::vector<VkVertexInputAttributeDescription> attributes) {
    bindings_ = std::move(bindings);
    attributes_ = std::move(attributes);
    return *this;
}

GraphicsPipelineBuilder& GraphicsPipelineBuilder::cull(VkCullModeFlags mode, VkFrontFace front) {
    cullMode_ = mode;
    frontFace_ = front;
    return *this;
}

GraphicsPipelineBuilder& GraphicsPipelineBuilder::depth(bool test, bool write, VkCompareOp compare) {
    depthTest_ = test;
    depthWrite_ = write;
    depthCompare_ = compare;
    return *this;
}

GraphicsPipelineBuilder& GraphicsPipelineBuilder::depthBias(float constantFactor, float slopeFactor) {
    depthBias_ = true;
    biasConstant_ = constantFactor;
    biasSlope_ = slopeFactor;
    return *this;
}

GraphicsPipelineBuilder& GraphicsPipelineBuilder::dynamicState(VkDynamicState state) {
    dynamicStates_.push_back(state);
    return *this;
}

GraphicsPipelineBuilder& GraphicsPipelineBuilder::colorFormat(VkFormat format) {
    colorFormats_.push_back(format);
    return *this;
}

GraphicsPipelineBuilder& GraphicsPipelineBuilder::depthFormat(VkFormat format) {
    depthFormat_ = format;
    return *this;
}

GraphicsPipelineBuilder& GraphicsPipelineBuilder::layout(VkPipelineLayout layout) {
    layout_ = layout;
    return *this;
}

VkPipeline GraphicsPipelineBuilder::build(VkDevice device, VkPipelineCache cache) const {
    VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertexInput.vertexBindingDescriptionCount = static_cast<uint32_t>(bindings_.size());
    vertexInput.pVertexBindingDescriptions = bindings_.data();
    vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes_.size());
    vertexInput.pVertexAttributeDescriptions = attributes_.data();

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    // Viewport and scissor are set per pass, so a resize doesn't need new pipelines.
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = static_cast<uint32_t>(dynamicStates_.size());
    dynamic.pDynamicStates = dynamicStates_.data();

    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = cullMode_;
    raster.frontFace = frontFace_;
    raster.lineWidth = 1.0f;
    raster.depthBiasEnable = depthBias_ ? VK_TRUE : VK_FALSE;
    raster.depthBiasConstantFactor = biasConstant_;
    raster.depthBiasSlopeFactor = biasSlope_;

    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthTestEnable = depthTest_ ? VK_TRUE : VK_FALSE;
    depth.depthWriteEnable = depthWrite_ ? VK_TRUE : VK_FALSE;
    depth.depthCompareOp = depthCompare_;

    std::vector<VkPipelineColorBlendAttachmentState> blendAttachments(colorFormats_.size());
    for (auto& a : blendAttachments) {
        a.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                           VK_COLOR_COMPONENT_A_BIT;
    }
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = static_cast<uint32_t>(blendAttachments.size());
    blend.pAttachments = blendAttachments.data();

    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = static_cast<uint32_t>(colorFormats_.size());
    rendering.pColorAttachmentFormats = colorFormats_.data();
    rendering.depthAttachmentFormat = depthFormat_;

    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pNext = &rendering;
    info.stageCount = static_cast<uint32_t>(stages_.size());
    info.pStages = stages_.data();
    info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &inputAssembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = layout_;

    VkPipeline pipeline;
    VK_CHECK(vkCreateGraphicsPipelines(device, cache, 1, &info, nullptr, &pipeline));
    return pipeline;
}

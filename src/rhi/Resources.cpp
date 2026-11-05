#include "rhi/Resources.h"

#include "rhi/VkCheck.h"
#include "rhi/VulkanContext.h"

#include <cstring>

Buffer createBuffer(VmaAllocator allocator, VkDeviceSize size, VkBufferUsageFlags usage,
                    VmaAllocationCreateFlags allocFlags) {
    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocInfo.flags = allocFlags;

    Buffer buffer;
    buffer.size = size;
    VmaAllocationInfo info{};
    VK_CHECK(vmaCreateBuffer(allocator, &bufferInfo, &allocInfo, &buffer.buffer, &buffer.allocation, &info));
    buffer.mapped = info.pMappedData;
    return buffer;
}

void destroyBuffer(VmaAllocator allocator, Buffer& buffer) {
    if (buffer.buffer) vmaDestroyBuffer(allocator, buffer.buffer, buffer.allocation);
    buffer = Buffer{};
}

Buffer uploadBuffer(VulkanContext& ctx, const void* data, VkDeviceSize size, VkBufferUsageFlags usage) {
    // 1. CPU writes into a host-visible staging buffer.
    Buffer staging = createBuffer(ctx.allocator, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                  VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                      VMA_ALLOCATION_CREATE_MAPPED_BIT);
    std::memcpy(staging.mapped, data, static_cast<size_t>(size));
    VK_CHECK(vmaFlushAllocation(ctx.allocator, staging.allocation, 0, size));

    // 2. The GPU copies it into fast device-local memory.
    Buffer gpu = createBuffer(ctx.allocator, size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    ctx.immediateSubmit([&](VkCommandBuffer cmd) {
        VkBufferCopy region{0, 0, size};
        vkCmdCopyBuffer(cmd, staging.buffer, gpu.buffer, 1, &region);

        // Make the copy visible to whatever reads the buffer next (vertex input, shaders...).
        VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dep);
    });

    destroyBuffer(ctx.allocator, staging);
    return gpu;
}

Image createImage(const VulkanContext& ctx, VkExtent2D extent, VkFormat format, VkImageUsageFlags usage,
                  VkImageAspectFlags aspect) {
    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = {extent.width, extent.height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = usage;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    // Render targets get their own memory block; they're large and recreated on resize.
    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocInfo.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;

    Image image;
    image.format = format;
    image.extent = extent;
    VK_CHECK(vmaCreateImage(ctx.allocator, &imageInfo, &allocInfo, &image.image, &image.allocation, nullptr));

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = image.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange = {aspect, 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(ctx.device, &viewInfo, nullptr, &image.view));
    return image;
}

void destroyImage(const VulkanContext& ctx, Image& image) {
    if (image.view) vkDestroyImageView(ctx.device, image.view, nullptr);
    if (image.image) vmaDestroyImage(ctx.allocator, image.image, image.allocation);
    image = Image{};
}

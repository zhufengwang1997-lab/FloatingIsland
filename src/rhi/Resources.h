#pragma once

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

class VulkanContext;

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    void* mapped = nullptr;  // set when created with VMA_ALLOCATION_CREATE_MAPPED_BIT
};

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
};

// `allocFlags` picks where the memory lives, e.g. HOST_ACCESS_SEQUENTIAL_WRITE for CPU-written
// staging data; 0 lets VMA choose device-local memory.
Buffer createBuffer(VmaAllocator allocator, VkDeviceSize size, VkBufferUsageFlags usage,
                    VmaAllocationCreateFlags allocFlags = 0);
void destroyBuffer(VmaAllocator allocator, Buffer& buffer);

// Creates a device-local buffer and fills it with `data` through a staging buffer.
Buffer uploadBuffer(VulkanContext& ctx, const void* data, VkDeviceSize size, VkBufferUsageFlags usage);

// A 2D image with one mip level, plus a view of it. Used for render targets such as depth.
Image createImage(const VulkanContext& ctx, VkExtent2D extent, VkFormat format, VkImageUsageFlags usage,
                  VkImageAspectFlags aspect);
void destroyImage(const VulkanContext& ctx, Image& image);

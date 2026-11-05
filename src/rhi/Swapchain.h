#pragma once

#include <vulkan/vulkan.h>

#include <vector>

class VulkanContext;

// The images the window shows. Recreated whenever the window size changes.
class Swapchain {
public:
    void create(const VulkanContext& ctx, VkExtent2D framebufferSize);
    void destroy(VkDevice device);

    VkSwapchainKHR handle = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
};

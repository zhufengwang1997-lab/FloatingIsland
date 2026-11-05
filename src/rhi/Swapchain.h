#pragma once

#include <vulkan/vulkan.h>

#include <vector>

class VulkanContext;

// The images the window shows. Recreated whenever the window size changes.
const char* presentModeName(VkPresentModeKHR mode);

class Swapchain {
public:
    // vsync = false asks for MAILBOX or IMMEDIATE (uncapped frame rate) and falls back to FIFO.
    void create(const VulkanContext& ctx, VkExtent2D framebufferSize, bool vsync = true);
    void destroy(VkDevice device);

    VkSwapchainKHR handle = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
};

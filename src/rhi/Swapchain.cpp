#include "rhi/Swapchain.h"

#include "rhi/VkCheck.h"
#include "rhi/VulkanContext.h"

#include <algorithm>

namespace {

// Prefer an sRGB format so the hardware converts our linear shader output for display.
VkSurfaceFormatKHR chooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) {
    for (const auto& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_SRGB && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) return f;
    }
    return formats.front();
}

VkCompositeAlphaFlagBitsKHR chooseCompositeAlpha(VkCompositeAlphaFlagsKHR supported) {
    const VkCompositeAlphaFlagBitsKHR order[] = {
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR};
    for (auto bit : order) {
        if (supported & bit) return bit;
    }
    return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
}

VkPresentModeKHR choosePresentMode(const VulkanContext& ctx, bool vsync) {
    if (vsync) return VK_PRESENT_MODE_FIFO_KHR;  // the only mode every device must support
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(ctx.physicalDevice, ctx.surface, &count, nullptr);
    std::vector<VkPresentModeKHR> modes(count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(ctx.physicalDevice, ctx.surface, &count, modes.data());
    // MAILBOX: no tearing, newest frame wins. IMMEDIATE: may tear, but never waits.
    for (VkPresentModeKHR wanted : {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR}) {
        if (std::find(modes.begin(), modes.end(), wanted) != modes.end()) return wanted;
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

}  // namespace

const char* presentModeName(VkPresentModeKHR mode) {
    switch (mode) {
        case VK_PRESENT_MODE_FIFO_KHR: return "FIFO (vsync)";
        case VK_PRESENT_MODE_MAILBOX_KHR: return "MAILBOX";
        case VK_PRESENT_MODE_IMMEDIATE_KHR: return "IMMEDIATE";
        case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "FIFO_RELAXED";
        default: return "other";
    }
}

void Swapchain::create(const VulkanContext& ctx, VkExtent2D framebufferSize, bool vsync) {
    VkSurfaceCapabilitiesKHR caps;
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx.physicalDevice, ctx.surface, &caps));

    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(ctx.physicalDevice, ctx.surface, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(ctx.physicalDevice, ctx.surface, &formatCount, formats.data());
    VkSurfaceFormatKHR surfaceFormat = chooseSurfaceFormat(formats);

    // currentExtent is 0xFFFFFFFF when the surface lets the swapchain pick its size.
    if (caps.currentExtent.width != UINT32_MAX) {
        extent = caps.currentExtent;
    } else {
        extent.width = std::clamp(framebufferSize.width, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(framebufferSize.height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }

    // One more than the minimum so we rarely wait on the presentation engine.
    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0) imageCount = std::min(imageCount, caps.maxImageCount);

    VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = ctx.surface;
    info.minImageCount = imageCount;
    info.imageFormat = surfaceFormat.format;
    info.imageColorSpace = surfaceFormat.colorSpace;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    // Lets the renderer copy a frame out for screenshots.
    if (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) info.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = chooseCompositeAlpha(caps.supportedCompositeAlpha);
    presentMode = choosePresentMode(ctx, vsync);
    info.presentMode = presentMode;
    info.clipped = VK_TRUE;
    VK_CHECK(vkCreateSwapchainKHR(ctx.device, &info, nullptr, &handle));
    format = surfaceFormat.format;

    uint32_t count = 0;
    vkGetSwapchainImagesKHR(ctx.device, handle, &count, nullptr);
    images.resize(count);
    vkGetSwapchainImagesKHR(ctx.device, handle, &count, images.data());

    views.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = images[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(ctx.device, &viewInfo, nullptr, &views[i]));
    }
}

void Swapchain::destroy(VkDevice device) {
    for (VkImageView view : views) vkDestroyImageView(device, view, nullptr);
    if (handle) vkDestroySwapchainKHR(device, handle, nullptr);
    *this = Swapchain{};
}

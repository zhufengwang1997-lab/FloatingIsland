#pragma once

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>

struct GLFWwindow;

// Owns the Vulkan objects that live for the whole program:
// instance, debug messenger, window surface, physical device, logical device, queue
// and the VMA allocator that hands out GPU memory.
class VulkanContext {
public:
    void init(GLFWwindow* window);
    void shutdown();

    // Records commands with `record`, submits them and waits until the GPU has finished.
    // For one-off work such as uploads; never call it inside a frame.
    void immediateSubmit(const std::function<void(VkCommandBuffer)>& record);

    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;   // also used for present and uploads
    uint32_t graphicsQueueFamily = 0;
    VmaAllocator allocator = VK_NULL_HANDLE;
    bool validationEnabled = false;

private:
    void createInstance();
    void createDebugMessenger();
    void pickPhysicalDevice();
    void createDevice();
    void createAllocator();
    void createUploadResources();

    VkDebugUtilsMessengerEXT debugMessenger_ = VK_NULL_HANDLE;
    VkCommandPool uploadPool_ = VK_NULL_HANDLE;
    VkCommandBuffer uploadCmd_ = VK_NULL_HANDLE;
    VkFence uploadFence_ = VK_NULL_HANDLE;
};

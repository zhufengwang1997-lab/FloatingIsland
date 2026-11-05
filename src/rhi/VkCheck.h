#pragma once

#include <vulkan/vk_enum_string_helper.h>
#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstdlib>

// Aborts with the call, the VkResult name and the source location when a Vulkan call fails.
#define VK_CHECK(expr)                                                                   \
    do {                                                                                 \
        VkResult vkCheckResult_ = (expr);                                                \
        if (vkCheckResult_ != VK_SUCCESS) {                                              \
            std::fprintf(stderr, "%s failed: %s (%s:%d)\n", #expr,                       \
                         string_VkResult(vkCheckResult_), __FILE__, __LINE__);           \
            std::abort();                                                                \
        }                                                                                \
    } while (0)

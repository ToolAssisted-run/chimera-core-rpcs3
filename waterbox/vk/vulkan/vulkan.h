/* <vulkan/vulkan.h> for the sandbox build.
 *
 * miniBox carries the two headers its Vulkan bridge needs (vulkan_core.h and
 * vk_platform.h) and nothing of any window system, because a sandboxed core
 * has no window. vulkan.h proper is those two and the window systems' own
 * headers, so this is all of it that applies here.
 *
 * No prototypes: in the box a Vulkan command is not a function a library
 * exports. It is a wrapper in the guest half of miniBox's bridge, found by
 * name, and volk (volk.h, beside this) holds the table of them under the
 * commands' own names. RPCS3 calls vkCreateBuffer(...) as it always did. */
#ifndef VULKAN_H_
#define VULKAN_H_ 1

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES 1
#endif

#include <vulkan/vk_platform.h>
#include <vulkan/vulkan_core.h>

#endif

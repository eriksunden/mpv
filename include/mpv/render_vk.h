/* Copyright (C) 2024 the mpv developers
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef MPV_CLIENT_API_RENDER_VK_H_
#define MPV_CLIENT_API_RENDER_VK_H_

/*
 * Note: <vulkan/vulkan.h> (or equivalent) MUST be included before this file.
 *       mpv does not pull in Vulkan headers on its own to avoid hard-coding
 *       which Vulkan loader header the application uses.
 */

#include "render.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Vulkan backend (libplacebo)
 * ---------------------------
 *
 * The Vulkan backend renders via libplacebo (the vo=gpu-next renderer) using
 * a Vulkan device provided by the host application.
 *
 * Use mpv_render_context_create() with MPV_RENDER_PARAM_API_TYPE set to
 * MPV_RENDER_API_TYPE_VULKAN and MPV_RENDER_PARAM_VULKAN_INIT_PARAMS provided.
 *
 * Call mpv_render_context_render() with MPV_RENDER_PARAM_VULKAN_FBO to render
 * the video frame into a caller-owned VkImage.
 *
 * Synchronization
 * ---------------
 *
 * The host application is responsible for ensuring the image is not in use
 * before calling mpv_render_context_render(). When the call returns, all GPU
 * work has been submitted and waited on (via pl_gpu_finish); the image is safe
 * to use immediately.
 *
 * The image is expected to be in VK_IMAGE_LAYOUT_GENERAL when passed in.
 * After rendering it will remain in VK_IMAGE_LAYOUT_GENERAL (or whatever
 * layout libplacebo last transitioned it to), so the caller must issue an
 * explicit layout transition to whatever layout is needed next.
 *
 * For queue family ownership, pass images created with
 * VK_SHARING_MODE_CONCURRENT, or ensure the image is not owned by any
 * specific queue family (qf = VK_QUEUE_FAMILY_IGNORED is used in the
 * release call to libplacebo).
 *
 * Usage flags
 * -----------
 *
 * The VkImage must be created with at least:
 *   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
 *
 * Additional flags (e.g. VK_IMAGE_USAGE_SAMPLED_BIT) may improve internal
 * rendering efficiency if available.
 */

/**
 * Vulkan initialization parameters.
 * Passed as MPV_RENDER_PARAM_VULKAN_INIT_PARAMS data.
 */
typedef struct mpv_vulkan_init_params {
    /**
     * The VkInstance created by the host application. Required.
     * Must have been created with apiVersion >= PL_VK_MIN_VERSION
     * (available from <libplacebo/vulkan.h> if needed, typically Vulkan 1.2).
     */
    VkInstance instance;

    /**
     * Pointer to the host's vkGetInstanceProcAddr implementation. Optional.
     * If NULL, libplacebo uses the function linked at compile time.
     */
    PFN_vkGetInstanceProcAddr get_instance_proc_addr;

    /**
     * The physical device selected by the host application. Required.
     */
    VkPhysicalDevice phys_device;

    /**
     * The logical device created by the host application. Required.
     */
    VkDevice device;

    /**
     * Graphics queue family and queue index within that family.
     * queue_graphics.count should be the number of queues in this family
     * that the application created (used for parallel submission).
     * Set count to 0 to let libplacebo use all available queues.
     *
     * At least queue_graphics must be valid. queue_compute and
     * queue_transfer default to queue_graphics if not provided.
     */
    uint32_t queue_graphics_family;
    uint32_t queue_graphics_index;
    uint32_t queue_graphics_count;

    /**
     * Optional: dedicated compute queue.
     * Set count to 0 if not available; libplacebo falls back to graphics.
     */
    uint32_t queue_compute_family;
    uint32_t queue_compute_index;
    uint32_t queue_compute_count;

    /**
     * Optional: dedicated transfer queue.
     * Set count to 0 if not available; libplacebo falls back to graphics.
     */
    uint32_t queue_transfer_family;
    uint32_t queue_transfer_index;
    uint32_t queue_transfer_count;

    /**
     * Null-terminated list of device-level extension names that were enabled
     * when creating `device`. Optional (may be NULL with num_extensions = 0).
     * libplacebo uses this to determine which optional features are available.
     */
    const char * const *extensions;
    int num_extensions;
} mpv_vulkan_init_params;

/**
 * VkImage descriptor passed to mpv_render_context_render().
 * Passed as MPV_RENDER_PARAM_VULKAN_FBO data.
 */
typedef struct mpv_vulkan_fbo {
    /**
     * The VkImage to render video into. Required.
     * See the Synchronization and Usage flags sections above.
     */
    VkImage image;

    /**
     * The VkFormat of `image`. Required.
     */
    VkFormat format;

    /**
     * The usage flags `image` was created with. Required.
     * Must include at least VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT.
     */
    VkImageUsageFlags usage;

    /**
     * Current image layout. Required.
     * The image must be in this layout when the call is made.
     * VK_IMAGE_LAYOUT_GENERAL is recommended for simplicity.
     */
    VkImageLayout layout;

    /**
     * Image dimensions. Required.
     */
    int w, h;
} mpv_vulkan_fbo;

#ifdef __cplusplus
}
#endif

#endif /* MPV_CLIENT_API_RENDER_VK_H_ */

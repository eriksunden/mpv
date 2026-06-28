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

#ifndef MPV_CLIENT_API_RENDER_MTL_H_
#define MPV_CLIENT_API_RENDER_MTL_H_

#include "render.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Metal backend (libplacebo, swapchain model)
 * -------------------------------------------
 *
 * The Metal backend renders via libplacebo using a CAMetalLayer supplied by
 * the host application.  libplacebo manages drawable acquisition, command
 * buffer submission, and presentation internally via pl_metal_create_swapchain.
 *
 * Use mpv_render_context_create() with MPV_RENDER_PARAM_API_TYPE set to
 * MPV_RENDER_API_TYPE_METAL and MPV_RENDER_PARAM_METAL_INIT_PARAMS provided.
 *
 * Call mpv_render_context_render() with no Metal-specific per-frame params —
 * libmpv acquires and presents drawables automatically via the swapchain.
 *
 * Typical rendering loop
 * ----------------------
 *
 *   mpv_render_param params[] = {
 *       {MPV_RENDER_PARAM_FLIP_Y, &(int){0}},
 *       {0},
 *   };
 *   mpv_render_context_render(ctx, params);
 *   // libmpv submits and presents the frame internally.
 *
 * Synchronization
 * ---------------
 *
 * mpv_render_context_render() returns after submitting the Metal command
 * buffer (including a presentDrawable schedule).  Presentation is asynchronous
 * — the drawable is retired by the display hardware without any CPU stall.
 */

/**
 * Metal initialization parameters.
 * Passed as MPV_RENDER_PARAM_METAL_INIT_PARAMS data.
 */
typedef struct mpv_metal_init_params {
    /**
     * The CAMetalLayer to render into (CAMetalLayer*, passed as void*).
     * Required.  libmpv creates a libplacebo Metal swapchain from this layer
     * and manages all drawable acquisition and presentation internally.
     * The layer must remain valid for the lifetime of the render context.
     */
    void *layer;

    /**
     * Optional Metal device (id<MTLDevice>, passed as void*).
     * If NULL, libplacebo selects the system-default device, which is correct
     * for virtually all Apple Silicon and single-GPU Mac configurations.
     * Only set this when targeting a specific GPU on a multi-GPU Mac.
     */
    void *metal_device;
} mpv_metal_init_params;

#ifdef __cplusplus
}
#endif

#endif /* MPV_CLIENT_API_RENDER_MTL_H_ */

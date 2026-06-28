/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <libplacebo/gpu.h>
#include <libplacebo/log.h>
#include "mpv/render.h"

struct mpv_global;
struct mp_log;

// Per-API context shared between libmpv_gpu_next.c and its backend drivers.
struct libmpv_gpu_next_context {
    struct mpv_global *global;
    struct mp_log *log;
    const struct libmpv_gpu_next_context_fns *fns;

    // Set by the backend's init():
    pl_gpu gpu;
    pl_log pllog;

    void *priv; // backend-private data

    // Target output color space.  Set from mpv options before start_frame();
    // swapchain backends update it to reflect the committed pixel format.
    struct pl_color_space target_color_space;
};

// vtable for per-API context backends (one per graphics API)
struct libmpv_gpu_next_context_fns {
    // Identifies the MPV_RENDER_API_TYPE_* string this backend handles.
    const char *api_name;

    // Create the libplacebo GPU context from user-supplied render params.
    // Must set ctx->gpu and ctx->pllog on success.
    int (*init)(struct libmpv_gpu_next_context *ctx, mpv_render_param *params);

    // Optional — NULL for FBO backends (OpenGL, Vulkan) that have no swapchain.
    // Swapchain backends call pl_swapchain_colorspace_hint() to configure the
    // pixel format before the next drawable is acquired in start_frame().
    void (*colorspace_hint)(struct libmpv_gpu_next_context *ctx,
                            const struct pl_color_space *csp);

    // Acquire a render target for the current frame.
    // FBO backends: wrap the user-supplied FBO from per-frame params.
    // Swapchain backends: call pl_swapchain_start_frame internally and update
    //   ctx->target_color_space from cur_frame.color_space.
    // On success *out_tex is a valid pl_tex render target.
    int (*start_frame)(struct libmpv_gpu_next_context *ctx,
                       mpv_render_param *params, pl_tex *out_tex);

    // Return the render target size without acquiring a frame.
    // FBO backends: read from per-frame params.
    // Swapchain backends: query the live drawableSize, not cached from start_frame.
    int (*get_target_size)(struct libmpv_gpu_next_context *ctx,
                           mpv_render_param *params, int *out_w, int *out_h);

    // Submit the current frame and release the render target.
    // FBO backends: destroy the wrapper (*target via pl_tex_destroy) then flush GPU.
    // Swapchain backends: call pl_swapchain_submit_frame + swap_buffers;
    //   *target is owned by the swapchain and must NOT be destroyed.
    void (*end_frame)(struct libmpv_gpu_next_context *ctx, pl_tex *target);

    // Destroy the context and free priv.
    void (*destroy)(struct libmpv_gpu_next_context *ctx);
};

// Available when HAVE_GL && PL_HAVE_OPENGL.
extern const struct libmpv_gpu_next_context_fns libmpv_gpu_next_context_gl;

// Available when HAVE_VULKAN && PL_HAVE_VULKAN.
extern const struct libmpv_gpu_next_context_fns libmpv_gpu_next_context_vk;

// Available when PL_HAVE_METAL.
extern const struct libmpv_gpu_next_context_fns libmpv_gpu_next_context_mtl;

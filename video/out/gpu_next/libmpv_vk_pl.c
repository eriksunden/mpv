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

#include "config.h"
#include <libplacebo/config.h>

#if HAVE_VULKAN && defined(PL_HAVE_VULKAN)

#include <vulkan/vulkan.h>
#include <libplacebo/vulkan.h>

#include "mpv_talloc.h"
#include "common/msg.h"
#include "mpv/render.h"
#include "mpv/render_vk.h"
#include "video/out/libmpv.h"
#include "video/out/gpu_next/libmpv_gpu_next.h"
#include "video/out/placebo/utils.h"

struct priv {
    pl_vulkan vulkan;
};

static int init(struct libmpv_gpu_next_context *ctx, mpv_render_param *params)
{
    mpv_vulkan_init_params *p_init =
        get_mpv_render_param(params, MPV_RENDER_PARAM_VULKAN_INIT_PARAMS, NULL);
    if (!p_init || !p_init->instance || !p_init->phys_device || !p_init->device)
        return MPV_ERROR_INVALID_PARAMETER;

    struct priv *p = talloc_zero(NULL, struct priv);
    ctx->priv = p;

    ctx->pllog = mppl_log_create(p, ctx->log);
    if (!ctx->pllog)
        return MPV_ERROR_GENERIC;

    mppl_log_set_probing(ctx->pllog, true);
    p->vulkan = pl_vulkan_import(ctx->pllog, pl_vulkan_import_params(
        .instance      = p_init->instance,
        .get_proc_addr = p_init->get_instance_proc_addr,
        .phys_device   = p_init->phys_device,
        .device        = p_init->device,
        .queue_graphics = {
            .index = p_init->queue_graphics_index,
            .count = p_init->queue_graphics_count,
        },
        .queue_compute = {
            .index = p_init->queue_compute_index,
            .count = p_init->queue_compute_count,
        },
        .queue_transfer = {
            .index = p_init->queue_transfer_index,
            .count = p_init->queue_transfer_count,
        },
        .extensions     = p_init->extensions,
        .num_extensions = p_init->num_extensions,
    ));
    mppl_log_set_probing(ctx->pllog, false);

    if (!p->vulkan) {
        MP_ERR(ctx, "Failed to import Vulkan device via libplacebo.\n");
        return MPV_ERROR_UNSUPPORTED;
    }

    ctx->gpu = p->vulkan->gpu;
    return 0;
}

static int start_frame(struct libmpv_gpu_next_context *ctx,
                       mpv_render_param *params, pl_tex *out_tex)
{
    mpv_vulkan_fbo *fbo =
        get_mpv_render_param(params, MPV_RENDER_PARAM_VULKAN_FBO, NULL);
    if (!fbo || !fbo->image)
        return MPV_ERROR_INVALID_PARAMETER;

    *out_tex = pl_vulkan_wrap(ctx->gpu, pl_vulkan_wrap_params(
        .image  = fbo->image,
        .width  = fbo->w,
        .height = fbo->h,
        .format = fbo->format,
        .usage  = fbo->usage,
    ));
    if (!*out_tex) {
        MP_ERR(ctx, "Failed to wrap VkImage (format %d, usage 0x%x) — "
               "is VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT set?\n",
               (int)fbo->format, (unsigned)fbo->usage);
        return MPV_ERROR_UNSUPPORTED;
    }

    // Release to libplacebo; skip queue family transition (caller handles ownership).
    pl_vulkan_release_ex(ctx->gpu, pl_vulkan_release_params(
        .tex    = *out_tex,
        .layout = fbo->layout,
        .qf     = VK_QUEUE_FAMILY_IGNORED,
    ));

    return 0;
}

static int get_target_size(struct libmpv_gpu_next_context *ctx,
                           mpv_render_param *params, int *out_w, int *out_h)
{
    mpv_vulkan_fbo *fbo =
        get_mpv_render_param(params, MPV_RENDER_PARAM_VULKAN_FBO, NULL);
    if (!fbo)
        return MPV_ERROR_INVALID_PARAMETER;
    *out_w = fbo->w;
    *out_h = fbo->h;
    return 0;
}

static void end_frame(struct libmpv_gpu_next_context *ctx, pl_tex *target)
{
    pl_tex_destroy(ctx->gpu, target);
    // Wait for GPU completion so the VkImage is safe to use immediately.
    pl_gpu_finish(ctx->gpu);
}

static void destroy(struct libmpv_gpu_next_context *ctx)
{
    struct priv *p = ctx->priv;
    if (!p)
        return;
    pl_vulkan_destroy(&p->vulkan);
    pl_log_destroy(&ctx->pllog);
    talloc_free(p);
    ctx->priv = NULL;
}

const struct libmpv_gpu_next_context_fns libmpv_gpu_next_context_vk = {
    .api_name        = MPV_RENDER_API_TYPE_VULKAN,
    .init            = init,
    .start_frame     = start_frame,
    .get_target_size = get_target_size,
    .end_frame       = end_frame,
    .destroy         = destroy,
};

#endif // HAVE_VULKAN && PL_HAVE_VULKAN

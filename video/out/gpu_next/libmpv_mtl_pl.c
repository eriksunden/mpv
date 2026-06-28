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

#if defined(PL_HAVE_METAL)

#include <libplacebo/metal.h>
#include <libplacebo/swapchain.h>

#include "mpv_talloc.h"
#include "common/msg.h"
#include "mpv/render.h"
#include "mpv/render_mtl.h"
#include "video/out/libmpv.h"
#include "video/out/placebo/utils.h"
#include "libmpv_gpu_next.h"

struct priv {
    pl_metal metal;
    pl_swapchain swapchain;
    struct pl_swapchain_frame cur_frame;
    bool has_frame;
};

static int init(struct libmpv_gpu_next_context *ctx, mpv_render_param *params)
{
    mpv_metal_init_params *p_init =
        get_mpv_render_param(params, MPV_RENDER_PARAM_METAL_INIT_PARAMS, NULL);
    if (!p_init || !p_init->layer)
        return MPV_ERROR_INVALID_PARAMETER;

    struct priv *p = talloc_zero(NULL, struct priv);
    ctx->priv = p;

    ctx->pllog = mppl_log_create(p, ctx->log);
    if (!ctx->pllog)
        return MPV_ERROR_GENERIC;

    mppl_log_set_probing(ctx->pllog, true);
    p->metal = pl_metal_create(ctx->pllog, pl_metal_params(
        .device = p_init->metal_device,
    ));
    mppl_log_set_probing(ctx->pllog, false);

    if (!p->metal) {
        MP_ERR(ctx, "Failed to create Metal context via libplacebo.\n");
        return MPV_ERROR_UNSUPPORTED;
    }

    p->swapchain = pl_metal_create_swapchain(p->metal, pl_metal_swapchain_params(
        .layer = p_init->layer,
    ));
    if (!p->swapchain) {
        MP_ERR(ctx, "Failed to create Metal swapchain for the supplied CAMetalLayer.\n");
        pl_metal_destroy(&p->metal);
        return MPV_ERROR_UNSUPPORTED;
    }

    ctx->gpu = p->metal->gpu;
    return 0;
}

static void colorspace_hint(struct libmpv_gpu_next_context *ctx,
                            const struct pl_color_space *csp)
{
    struct priv *p = ctx->priv;
    pl_swapchain_colorspace_hint(p->swapchain, csp);
}

static int start_frame(struct libmpv_gpu_next_context *ctx,
                       mpv_render_param *params, pl_tex *out_tex)
{
    struct priv *p = ctx->priv;

    if (!pl_swapchain_start_frame(p->swapchain, &p->cur_frame)) {
        MP_WARN(ctx, "pl_swapchain_start_frame failed — skipping frame.\n");
        p->has_frame = false;
        return MPV_ERROR_GENERIC;
    }

    p->has_frame = true;
    *out_tex     = p->cur_frame.fbo;
    ctx->target_color_space = p->cur_frame.color_space;
    return 0;
}

static int get_target_size(struct libmpv_gpu_next_context *ctx,
                           mpv_render_param *params, int *out_w, int *out_h)
{
    struct priv *p = ctx->priv;
    // Pass w=h=0 to read the live drawableSize without resizing.
    // Avoids a one-frame lag if the layer resizes between get_target_size() and render().
    int w = 0, h = 0;
    if (!pl_swapchain_resize(p->swapchain, &w, &h))
        return MPV_ERROR_GENERIC;
    *out_w = w;
    *out_h = h;
    return 0;
}

static void end_frame(struct libmpv_gpu_next_context *ctx, pl_tex *target)
{
    struct priv *p = ctx->priv;

    // target points to the swapchain FBO — the swapchain owns it, do not destroy.
    (void)target;

    if (!p->has_frame)
        return;

    if (!pl_swapchain_submit_frame(p->swapchain))
        MP_WARN(ctx, "pl_swapchain_submit_frame failed.\n");

    pl_swapchain_swap_buffers(p->swapchain);
    p->has_frame = false;
}

static void destroy(struct libmpv_gpu_next_context *ctx)
{
    struct priv *p = ctx->priv;
    if (!p)
        return;
    if (p->swapchain)
        pl_swapchain_destroy(&p->swapchain);
    if (p->metal)
        pl_metal_destroy(&p->metal);
    pl_log_destroy(&ctx->pllog);
    talloc_free(p);
    ctx->priv = NULL;
}

const struct libmpv_gpu_next_context_fns libmpv_gpu_next_context_mtl = {
    .api_name        = MPV_RENDER_API_TYPE_METAL,
    .init            = init,
    .colorspace_hint = colorspace_hint,
    .start_frame     = start_frame,
    .get_target_size = get_target_size,
    .end_frame       = end_frame,
    .destroy         = destroy,
};

#endif // PL_HAVE_METAL

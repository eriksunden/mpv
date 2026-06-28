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

#if HAVE_GL && defined(PL_HAVE_OPENGL)

#include <libplacebo/opengl.h>

#include "mpv_talloc.h"
#include "common/msg.h"
#include "mpv/render_gl.h"
#include "video/out/libmpv.h"
#include "video/out/gpu_next/libmpv_gpu_next.h"
#include "video/out/placebo/utils.h"

struct priv {
    pl_opengl gl;
};

static int init(struct libmpv_gpu_next_context *ctx, mpv_render_param *params)
{
    mpv_opengl_init_params *p_init =
        get_mpv_render_param(params, MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, NULL);
    if (!p_init || !p_init->get_proc_address)
        return MPV_ERROR_INVALID_PARAMETER;

    struct priv *p = talloc_zero(NULL, struct priv);
    ctx->priv = p;

    ctx->pllog = mppl_log_create(p, ctx->log);
    if (!ctx->pllog)
        return MPV_ERROR_GENERIC;

    mppl_log_set_probing(ctx->pllog, true);
    p->gl = pl_opengl_create(ctx->pllog, pl_opengl_params(
        .get_proc_addr_ex = (pl_voidfunc_t (*)(void *, const char *)) p_init->get_proc_address,
        .proc_ctx         = p_init->get_proc_address_ctx,
        .allow_software   = true,
    ));
    mppl_log_set_probing(ctx->pllog, false);

    if (!p->gl) {
        MP_ERR(ctx, "Failed to initialize OpenGL context via libplacebo.\n");
        return MPV_ERROR_UNSUPPORTED;
    }

    ctx->gpu = p->gl->gpu;
    return 0;
}

static int start_frame(struct libmpv_gpu_next_context *ctx,
                       mpv_render_param *params, pl_tex *out_tex)
{
    mpv_opengl_fbo *fbo =
        get_mpv_render_param(params, MPV_RENDER_PARAM_OPENGL_FBO, NULL);
    if (!fbo)
        return MPV_ERROR_INVALID_PARAMETER;

    // pl_tex_destroy frees the wrapper, not the underlying GL framebuffer.
    *out_tex = pl_opengl_wrap(ctx->gpu, pl_opengl_wrap_params(
        .framebuffer = fbo->fbo,
        .width       = fbo->w,
        .height      = fbo->h,
        .iformat     = fbo->internal_format,
    ));

    return *out_tex ? 0 : MPV_ERROR_UNSUPPORTED;
}

static int get_target_size(struct libmpv_gpu_next_context *ctx,
                           mpv_render_param *params, int *out_w, int *out_h)
{
    mpv_opengl_fbo *fbo =
        get_mpv_render_param(params, MPV_RENDER_PARAM_OPENGL_FBO, NULL);
    if (!fbo)
        return MPV_ERROR_INVALID_PARAMETER;
    *out_w = fbo->w;
    *out_h = fbo->h;
    return 0;
}

static void end_frame(struct libmpv_gpu_next_context *ctx, pl_tex *target)
{
    pl_tex_destroy(ctx->gpu, target);
    pl_gpu_flush(ctx->gpu);
}

static void destroy(struct libmpv_gpu_next_context *ctx)
{
    struct priv *p = ctx->priv;
    if (!p)
        return;
    pl_opengl_destroy(&p->gl);
    pl_log_destroy(&ctx->pllog);
    talloc_free(p);
    ctx->priv = NULL;
}

const struct libmpv_gpu_next_context_fns libmpv_gpu_next_context_gl = {
    .api_name        = MPV_RENDER_API_TYPE_OPENGL_NEXT,
    .init            = init,
    .start_frame     = start_frame,
    .get_target_size = get_target_size,
    .end_frame       = end_frame,
    .destroy         = destroy,
};

#endif // HAVE_GL && PL_HAVE_OPENGL

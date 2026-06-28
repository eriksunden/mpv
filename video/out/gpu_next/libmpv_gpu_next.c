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

#include <stdint.h>
#include <string.h>

#include <libplacebo/options.h>
#include <libplacebo/renderer.h>
#include <libplacebo/utils/frame_queue.h>
#include <libplacebo/utils/upload.h>

#include "config.h"
#include <libplacebo/config.h>
#include "mpv_talloc.h"
#include "common/common.h"
#include "common/global.h"
#include "common/msg.h"
#include "options/m_config.h"
#include "options/m_option.h"
#include "options/options.h"
#include "sub/draw_bmp.h"
#include "sub/osd.h"
#include "video/hwdec.h"
#include "video/mp_image.h"
#include "video/mp_image_pool.h"
#include "video/out/libmpv.h"
#include "video/out/vo.h"
#include "video/out/gpu/video.h"
#include "video/out/gpu/video_shaders.h"
#include "video/csputils.h"
#include "video/fmt-conversion.h"

#include "libmpv_gpu_next.h"
#include "pl_utils.h"

static const struct libmpv_gpu_next_context_fns *context_backends[] = {
#if defined(PL_HAVE_METAL)
    &libmpv_gpu_next_context_mtl,
#endif
#if HAVE_VULKAN && defined(PL_HAVE_VULKAN)
    &libmpv_gpu_next_context_vk,
#endif
#if HAVE_GL && defined(PL_HAVE_OPENGL)
    &libmpv_gpu_next_context_gl,
#endif
    NULL
};

// Renamed to avoid collision with sub/osd.h's opaque struct osd_state.
struct osd_tex_entry {
    pl_tex tex;
    struct pl_overlay_part *parts;
    int num_parts;
};

struct osd_tex_state {
    struct osd_tex_entry entries[MAX_OSD_PARTS];
    struct pl_overlay overlays[MAX_OSD_PARTS];
};

struct frame_priv {
    struct priv *p;
};

struct priv {
    struct libmpv_gpu_next_context *ctx;

    pl_renderer renderer;
    pl_queue queue;

    pl_fmt osd_fmt[SUBBITMAP_COUNT];
    pl_tex *sub_tex;
    int num_sub_tex;
    struct osd_tex_state osd_state;

    struct osd_state *osd_src; // NULL when no VO is attached

    struct mp_rect src, dst;
    struct mp_osd_res osd_res;
    struct mp_image_params img_params;
    struct mp_csp_equalizer_state *video_eq;
    struct m_config_cache *opts_cache;

    pl_options pars;
    struct pl_filter_config scaler_cfgs[SCALER_COUNT]; // owned by pars

    uint64_t last_id;
    double last_pts;
};

static bool map_frame(pl_gpu gpu, pl_tex *tex,
                      const struct pl_source_frame *src, struct pl_frame *frame)
{
    struct mp_image *mpi = src->frame_data;
    struct frame_priv *fp = mpi->priv;
    struct priv *p = fp->p;

    mp_image_params_guess_csp(&mpi->params);

    *frame = (struct pl_frame) {
        .color    = mpi->params.color,
        .repr     = mpi->params.repr,
        .rotation = mpi->params.rotate / 90,
        .user_data = mpi,
    };

    struct pl_plane_data data[4] = {0};

    // Try normalized first, fall back to integer if necessary
    int n = plane_data_from_imgfmt(data, &frame->repr.bits, mpi->imgfmt, false);
    if (!n) {
        n = plane_data_from_imgfmt(data, &frame->repr.bits, mpi->imgfmt, true);
        if (!n) {
            talloc_free(mpi);
            return false;
        }
    }

    frame->num_planes = n;
    for (int i = 0; i < n; i++) {
        struct pl_plane *plane = &frame->planes[i];
        data[i].width      = mp_image_plane_w(mpi, i);
        data[i].height     = mp_image_plane_h(mpi, i);
        if (mpi->stride[i] < 0) {
            data[i].pixels     = mpi->planes[i] + (data[i].height - 1) * mpi->stride[i];
            data[i].row_stride = -mpi->stride[i];
            plane->flipped     = true;
        } else {
            data[i].pixels     = mpi->planes[i];
            data[i].row_stride = mpi->stride[i];
        }

        if (!pl_upload_plane(gpu, plane, &tex[i], &data[i])) {
            MP_ERR(p->ctx, "Failed uploading video plane %d.\n", i);
            talloc_free(mpi);
            return false;
        }
    }

    pl_frame_set_chroma_location(frame, mpi->params.chroma_location);
    return true;
}

static void unmap_frame(pl_gpu gpu, struct pl_frame *frame,
                        const struct pl_source_frame *src)
{
    struct mp_image *mpi = src->frame_data;
    struct frame_priv *fp = mpi->priv;
    struct priv *p = fp->p;

    for (int i = 0; i < MAX_OSD_PARTS; i++) {
        pl_tex t = p->osd_state.entries[i].tex;
        if (t)
            MP_TARRAY_APPEND(p, p->sub_tex, p->num_sub_tex, t);
        p->osd_state.entries[i].tex = NULL;
    }

    talloc_free(mpi);
}

static void discard_frame(const struct pl_source_frame *src)
{
    struct mp_image *mpi = src->frame_data;
    talloc_free(mpi);
}

static void update_overlays(struct priv *p, struct mp_osd_res res,
                             int flags, enum pl_overlay_coords coords,
                             struct osd_tex_state *state, struct pl_frame *frame,
                             double pts)
{
    frame->overlays    = state->overlays;
    frame->num_overlays = 0;

    if (!p->osd_src)
        return;

    struct sub_bitmap_list *subs = osd_render(p->osd_src, res, pts, flags,
                                              mp_draw_sub_formats);
    if (!subs)
        return;

    for (int n = 0; n < subs->num_items; n++) {
        const struct sub_bitmaps *item = subs->items[n];
        if (!item->num_parts || !item->packed)
            continue;

        struct osd_tex_entry *entry = &state->entries[item->render_index];
        pl_fmt tex_fmt = p->osd_fmt[item->format];
        if (!entry->tex)
            MP_TARRAY_POP(p->sub_tex, p->num_sub_tex, &entry->tex);

        bool ok = pl_tex_recreate(p->ctx->gpu, &entry->tex, &(struct pl_tex_params) {
            .format       = tex_fmt,
            .w = MPMAX(item->packed_w, entry->tex ? entry->tex->params.w : 0),
            .h = MPMAX(item->packed_h, entry->tex ? entry->tex->params.h : 0),
            .host_writable = true,
            .sampleable    = true,
        });
        if (!ok) {
            MP_ERR(p->ctx, "Failed recreating OSD texture.\n");
            break;
        }

        ok = pl_tex_upload(p->ctx->gpu, &(struct pl_tex_transfer_params) {
            .tex       = entry->tex,
            .rc        = { .x1 = item->packed_w, .y1 = item->packed_h },
            .row_pitch = item->packed->stride[0],
            .ptr       = item->packed->planes[0],
        });
        if (!ok) {
            MP_ERR(p->ctx, "Failed uploading OSD texture.\n");
            break;
        }

        entry->num_parts = 0;
        for (int i = 0; i < item->num_parts; i++) {
            const struct sub_bitmap *b = &item->parts[i];
            if (b->dw == 0 || b->dh == 0)
                continue;
            uint32_t c = b->libass.color;
            struct pl_overlay_part part = {
                .src   = { b->src_x, b->src_y, b->src_x + b->w, b->src_y + b->h },
                .dst   = { b->x, b->y, b->x + b->dw, b->y + b->dh },
                .color = {
                    (c >> 24) / 255.0f,
                    ((c >> 16) & 0xFF) / 255.0f,
                    ((c >> 8)  & 0xFF) / 255.0f,
                    (255 - (c & 0xFF)) / 255.0f,
                },
            };
            MP_TARRAY_APPEND(p, entry->parts, entry->num_parts, part);
        }

        struct pl_overlay *ol = &state->overlays[frame->num_overlays++];
        *ol = (struct pl_overlay) {
            .tex       = entry->tex,
            .parts     = entry->parts,
            .num_parts = entry->num_parts,
            .color     = pl_color_space_srgb,
            .coords    = coords,
        };

        switch (item->format) {
        case SUBBITMAP_BGRA:
            ol->mode       = PL_OVERLAY_NORMAL;
            ol->repr.alpha = PL_ALPHA_PREMULTIPLIED;
            break;
        case SUBBITMAP_LIBASS:
            ol->mode       = PL_OVERLAY_MONOCHROME;
            ol->repr.alpha = PL_ALPHA_INDEPENDENT;
            break;
        }
    }

    talloc_free(subs);
}

static int init(struct render_backend *ctx, mpv_render_param *params)
{
    char *api = get_mpv_render_param(params, MPV_RENDER_PARAM_API_TYPE, NULL);
    if (!api)
        return MPV_ERROR_INVALID_PARAMETER;

    const struct libmpv_gpu_next_context_fns *backend = NULL;
    for (int n = 0; context_backends[n]; n++) {
        if (strcmp(context_backends[n]->api_name, api) == 0) {
            backend = context_backends[n];
            break;
        }
    }
    if (!backend)
        return MPV_ERROR_NOT_IMPLEMENTED;

    ctx->priv = talloc_zero(NULL, struct priv);
    struct priv *p = ctx->priv;

    p->ctx = talloc_zero(p, struct libmpv_gpu_next_context);
    *p->ctx = (struct libmpv_gpu_next_context) {
        .global = ctx->global,
        .log    = ctx->log,
        .fns    = backend,
    };

    int err = backend->init(p->ctx, params);
    if (err < 0)
        return err;

    p->renderer = pl_renderer_create(p->ctx->pllog, p->ctx->gpu);
    if (!p->renderer)
        return MPV_ERROR_UNSUPPORTED;

    p->queue = pl_queue_create(p->ctx->gpu);
    if (!p->queue)
        return MPV_ERROR_UNSUPPORTED;

    p->osd_fmt[SUBBITMAP_LIBASS] = pl_find_fmt(p->ctx->gpu, PL_FMT_UNORM, 1, 8, 8, 0);
    p->osd_fmt[SUBBITMAP_BGRA]   = pl_find_fmt(p->ctx->gpu, PL_FMT_UNORM, 4, 8, 8, 0);

    p->video_eq   = mp_csp_equalizer_create(p, ctx->global);
    p->opts_cache = m_config_cache_alloc(p, ctx->global, &gl_video_conf);

    p->pars = pl_options_alloc(p->ctx->pllog);
    if (!p->pars) {
        MP_ERR(ctx, "Failed to allocate pl_options.\n");
        return MPV_ERROR_UNSUPPORTED;
    }

    p->ctx->target_color_space = pl_color_space_srgb;

    ctx->hwdec_devs  = hwdec_devices_create();
    ctx->driver_caps = VO_CAP_ROTATE90 | VO_CAP_VFLIP;

    return 0;
}

static bool check_format(struct render_backend *ctx, int imgfmt)
{
    struct priv *p = ctx->priv;
    struct pl_bit_encoding bits;
    struct pl_plane_data data[4] = {0};
    int n = plane_data_from_imgfmt(data, &bits, imgfmt, false);
    if (!n)
        n = plane_data_from_imgfmt(data, &bits, imgfmt, true);
    if (!n)
        return false;

    for (int i = 0; i < n; i++) {
        if (!pl_plane_find_fmt(p->ctx->gpu, NULL, &data[i]))
            return false;
    }
    return true;
}

static int set_parameter(struct render_backend *ctx, mpv_render_param param)
{
    return MPV_ERROR_NOT_IMPLEMENTED;
}

static void reset(struct render_backend *ctx)
{
    struct priv *p = ctx->priv;
    pl_renderer_flush_cache(p->renderer);
    pl_queue_reset(p->queue);
    p->last_id  = 0;
    p->last_pts = 0;
}

static void reconfig(struct render_backend *ctx, struct mp_image_params *params)
{
    struct priv *p = ctx->priv;
    // Flush cached pipelines and queued frames from the previous format so
    // libplacebo doesn't try to use a pipeline compiled for the old pixel
    // format (e.g. RG16Unorm) against a new intermediate texture (RG8Unorm).
    reset(ctx);
    p->img_params = *params;
}

static void screenshot(struct render_backend *ctx, struct vo_frame *frame,
                       struct voctrl_screenshot *args)
{
    struct priv *p = ctx->priv;
    args->res = NULL;

    if (!frame || !frame->current)
        return;

    struct mp_image *mpi = frame->current;

    pl_fmt fmt = pl_find_fmt(p->ctx->gpu, PL_FMT_UNORM, 4, 8, 8,
                             PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE);
    if (!fmt) {
        MP_ERR(p->ctx, "No suitable screenshot format found.\n");
        return;
    }

    pl_tex fbo = pl_tex_create(p->ctx->gpu, pl_tex_params(
        .w           = mpi->w,
        .h           = mpi->h,
        .format      = fmt,
        .renderable  = true,
        .host_readable = true,
    ));
    if (!fbo) {
        MP_ERR(p->ctx, "Failed creating screenshot texture.\n");
        return;
    }

    struct pl_frame src_frame = {0};
    struct pl_plane_data data[4] = {0};
    src_frame.num_planes = plane_data_from_imgfmt(data, &src_frame.repr.bits,
                                                  mpi->imgfmt, false);
    if (!src_frame.num_planes) {
        src_frame.num_planes = plane_data_from_imgfmt(data, &src_frame.repr.bits,
                                                      mpi->imgfmt, true);
    }
    if (!src_frame.num_planes)
        goto done;

    src_frame.color    = mpi->params.color;
    src_frame.repr     = mpi->params.repr;
    src_frame.rotation = mpi->params.rotate / 90;
    src_frame.crop     = (struct pl_rect2df) { 0, 0, mpi->w, mpi->h };

    mp_image_params_guess_csp(&mpi->params);
    src_frame.color = mpi->params.color;
    src_frame.repr  = mpi->params.repr;

    for (int i = 0; i < src_frame.num_planes; i++) {
        data[i].width      = mp_image_plane_w(mpi, i);
        data[i].height     = mp_image_plane_h(mpi, i);
        data[i].pixels     = mpi->planes[i];
        data[i].row_stride = mpi->stride[i];
        if (!pl_upload_plane(p->ctx->gpu, &src_frame.planes[i], NULL, &data[i]))
            goto cleanup_planes;
    }

    pl_frame_set_chroma_location(&src_frame, mpi->params.chroma_location);

    struct pl_frame target = {
        .num_planes      = 1,
        .planes[0]       = { .texture = fbo, .components = 4,
                             .component_mapping = {0, 1, 2, 3} },
        .crop            = { 0, 0, mpi->w, mpi->h },
        .repr            = pl_color_repr_rgb,
        .color           = pl_color_space_srgb,
    };

    pl_render_image(p->renderer, &src_frame, &target, &pl_render_default_params);

    struct mp_image *res = mp_image_alloc(IMGFMT_RGBA, mpi->w, mpi->h);
    if (res) {
        if (!pl_tex_download(p->ctx->gpu, &(struct pl_tex_transfer_params) {
                .tex       = fbo,
                .ptr       = res->planes[0],
                .row_pitch = res->stride[0],
            }))
        {
            talloc_free(res);
            res = NULL;
        }
        args->res = res;
    }

cleanup_planes:
    for (int i = 0; i < src_frame.num_planes; i++)
        pl_tex_destroy(p->ctx->gpu, &src_frame.planes[i].texture);
done:
    pl_tex_destroy(p->ctx->gpu, &fbo);
}

static void perfdata(struct render_backend *ctx,
                     struct voctrl_performance_data *out)
{
    // TODO: collect from pl_render_info callback
}

static struct mp_image *get_image(struct render_backend *ctx, int imgfmt,
                                  int w, int h, int stride_align, int flags)
{
    return NULL; // Direct Rendering not implemented
}

static void update_external(struct render_backend *ctx, struct vo *vo)
{
    struct priv *p = ctx->priv;
    p->osd_src = vo ? vo->osd : NULL;
}

static void resize(struct render_backend *ctx, struct mp_rect *src,
                   struct mp_rect *dst, struct mp_osd_res *osd)
{
    struct priv *p = ctx->priv;
    p->src     = *src;
    p->dst     = *dst;
    p->osd_res = *osd;
}

static const struct pl_filter_config *map_scaler_libmpv(struct priv *p,
                                                         enum scaler_unit unit)
{
    static const struct pl_filter_preset fixed_scalers[] = {
        { "bilinear",     &pl_filter_bilinear },
        { "bicubic_fast", &pl_filter_bicubic  },
        { "nearest",      &pl_filter_nearest  },
        { "oversample",   &pl_filter_oversample },
        {0},
    };
    static const struct pl_filter_preset fixed_frame_mixers[] = {
        { "linear",     &pl_filter_bilinear   },
        { "oversample", &pl_filter_oversample },
        {0},
    };

    const struct pl_filter_preset *fixed =
        unit == SCALER_TSCALE ? fixed_frame_mixers : fixed_scalers;

    const struct gl_video_opts *opts = p->opts_cache->opts;
    const struct scaler_config *cfg  = &opts->scaler[unit];
    struct scaler_config tmp;
    if (cfg->kernel.function == SCALER_INHERIT) {
        tmp = *cfg;
        scaler_conf_merge(&tmp, &opts->scaler[SCALER_SCALE], unit);
        cfg = &tmp;
    }

    const char *kname = m_opt_choice_str(cfg->kernel.functions, cfg->kernel.function);

    for (int i = 0; fixed[i].name; i++) {
        if (strcmp(kname, fixed[i].name) == 0)
            return fixed[i].filter;
    }

    struct pl_filter_config *par = &p->scaler_cfgs[unit];
    const struct pl_filter_preset *preset;
    const struct pl_filter_function_preset *fpreset;
    if ((preset = pl_find_filter_preset(kname))) {
        *par = *preset->filter;
    } else if ((fpreset = pl_find_filter_function_preset(kname))) {
        *par = (struct pl_filter_config) {
            .kernel    = fpreset->function,
            .params[0] = fpreset->function->params[0],
            .params[1] = fpreset->function->params[1],
        };
    } else {
        MP_WARN(p->ctx, "Unknown scaler '%s', falling back to bilinear.\n", kname);
        return &pl_filter_bilinear;
    }

    const char *wname = m_opt_choice_str(cfg->window.functions, cfg->window.function);
    const struct pl_filter_function_preset *wpreset;
    if ((wpreset = pl_find_filter_function_preset(wname))) {
        par->window    = wpreset->function;
        par->wparams[0] = wpreset->function->params[0];
        par->wparams[1] = wpreset->function->params[1];
    }

    for (int i = 0; i < 2; i++) {
        if (!isnan(cfg->kernel.params[i])) par->params[i]  = cfg->kernel.params[i];
        if (!isnan(cfg->window.params[i])) par->wparams[i] = cfg->window.params[i];
    }
    par->clamp = cfg->clamp;
    if (cfg->antiring > 0.0)        par->antiring = cfg->antiring;
    if (cfg->kernel.blur  > 0.0)    par->blur     = cfg->kernel.blur;
    if (cfg->kernel.taper > 0.0)    par->taper    = cfg->kernel.taper;
    if (cfg->radius > 0.0 && par->kernel->resizable)
        par->radius = cfg->radius;

    return par;
}

static void update_render_options_libmpv(struct priv *p)
{
    pl_options pars = p->pars;
    const struct gl_video_opts *opts = p->opts_cache->opts;

    // Background and presentation
    pars->params.background_color[0] = opts->background_color.r / 255.0f;
    pars->params.background_color[1] = opts->background_color.g / 255.0f;
    pars->params.background_color[2] = opts->background_color.b / 255.0f;
    pars->params.background_transparency = 1.0f - opts->background_color.a / 255.0f;
    pars->params.skip_anti_aliasing      = !opts->correct_downscaling;
    pars->params.disable_linear_scaling  = !opts->linear_downscaling && !opts->linear_upscaling;
    pars->params.disable_fbos            = opts->dumb_mode == 1;
    pars->params.correct_subpixel_offsets = !opts->scaler_resizes_only;

    // Scalers
    pars->params.upscaler     = map_scaler_libmpv(p, SCALER_SCALE);
    pars->params.downscaler   = map_scaler_libmpv(p, SCALER_DSCALE);
    pars->params.plane_upscaler = map_scaler_libmpv(p, SCALER_CSCALE);
    pars->params.frame_mixer  = opts->interpolation
                                 ? map_scaler_libmpv(p, SCALER_TSCALE) : NULL;

    // Debanding
    pars->params.deband_params = opts->deband ? &pars->deband_params : NULL;
    if (opts->deband) {
        pars->deband_params.iterations = opts->deband_opts->iterations;
        pars->deband_params.radius     = opts->deband_opts->range;
        pars->deband_params.threshold  = opts->deband_opts->threshold / 16.384f;
        pars->deband_params.grain      = opts->deband_opts->grain    /  8.192f;
    }

    // Sigmoid upscaling
    pars->params.sigmoid_params = opts->sigmoid_upscaling ? &pars->sigmoid_params : NULL;
    if (opts->sigmoid_upscaling) {
        pars->sigmoid_params.center = opts->sigmoid_center;
        pars->sigmoid_params.slope  = opts->sigmoid_slope;
    }

    // Peak detection
    pars->params.peak_detect_params = opts->tone_map.compute_peak >= 0
                                       ? &pars->peak_detect_params : NULL;
    pars->peak_detect_params.smoothing_period    = opts->tone_map.decay_rate;
    pars->peak_detect_params.scene_threshold_low = opts->tone_map.scene_threshold_low;
    pars->peak_detect_params.scene_threshold_high= opts->tone_map.scene_threshold_high;
    pars->peak_detect_params.percentile          = opts->tone_map.peak_percentile;

    // Tone mapping and gamut mapping
    static const struct pl_tone_map_function * const tone_map_funs[] = {
        [TONE_MAPPING_AUTO]      = &pl_tone_map_auto,
        [TONE_MAPPING_CLIP]      = &pl_tone_map_clip,
        [TONE_MAPPING_MOBIUS]    = &pl_tone_map_mobius,
        [TONE_MAPPING_REINHARD]  = &pl_tone_map_reinhard,
        [TONE_MAPPING_HABLE]     = &pl_tone_map_hable,
        [TONE_MAPPING_GAMMA]     = &pl_tone_map_gamma,
        [TONE_MAPPING_LINEAR]    = &pl_tone_map_linear,
        [TONE_MAPPING_SPLINE]    = &pl_tone_map_spline,
        [TONE_MAPPING_BT_2390]   = &pl_tone_map_bt2390,
        [TONE_MAPPING_BT_2446A]  = &pl_tone_map_bt2446a,
        [TONE_MAPPING_ST2094_40] = &pl_tone_map_st2094_40,
        [TONE_MAPPING_ST2094_10] = &pl_tone_map_st2094_10,
    };
    static const struct pl_gamut_map_function * const gamut_modes[] = {
        [GAMUT_AUTO]       = NULL, // resolved below
        [GAMUT_CLIP]       = &pl_gamut_map_clip,
        [GAMUT_PERCEPTUAL] = &pl_gamut_map_perceptual,
        [GAMUT_RELATIVE]   = &pl_gamut_map_relative,
        [GAMUT_SATURATION] = &pl_gamut_map_saturation,
        [GAMUT_ABSOLUTE]   = &pl_gamut_map_absolute,
        [GAMUT_DESATURATE] = &pl_gamut_map_desaturate,
        [GAMUT_DARKEN]     = &pl_gamut_map_darken,
        [GAMUT_WARN]       = &pl_gamut_map_highlight,
        [GAMUT_LINEAR]     = &pl_gamut_map_linear,
    };

    pars->color_map_params.tone_mapping_function = tone_map_funs[opts->tone_map.curve];
AV_NOWARN_DEPRECATED(
    pars->color_map_params.tone_mapping_param = opts->tone_map.curve_param;
    if (isnan(pars->color_map_params.tone_mapping_param))
        pars->color_map_params.tone_mapping_param = 0.0;
)
    pars->color_map_params.inverse_tone_mapping  = opts->tone_map.inverse;
    pars->color_map_params.contrast_recovery     = opts->tone_map.contrast_recovery;
    pars->color_map_params.visualize_lut         = opts->tone_map.visualize;
    pars->color_map_params.contrast_smoothness   = opts->tone_map.contrast_smoothness;
    pars->color_map_params.gamut_mapping         = opts->tone_map.gamut_mode == GAMUT_AUTO
        ? pl_color_map_default_params.gamut_mapping
        : gamut_modes[opts->tone_map.gamut_mode];

    // Dithering
    pars->params.dither_params   = NULL;
    pars->params.error_diffusion = NULL;
    if (opts->dither_depth >= 0) {
        switch (opts->dither_algo) {
        case DITHER_ERROR_DIFFUSION:
            pars->params.error_diffusion =
                pl_find_error_diffusion_kernel(opts->error_diffusion);
            if (!pars->params.error_diffusion)
                MP_WARN(p->ctx, "Unknown error-diffusion kernel '%s', "
                        "falling back to fruit.\n", opts->error_diffusion);
            MP_FALLTHROUGH;
        case DITHER_ORDERED:
        case DITHER_FRUIT:
            pars->params.dither_params = &pars->dither_params;
            pars->dither_params.method = opts->dither_algo == DITHER_ORDERED
                ? PL_DITHER_ORDERED_FIXED : PL_DITHER_BLUE_NOISE;
            pars->dither_params.lut_size  = opts->dither_size;
            pars->dither_params.temporal  = opts->temporal_dither;
            break;
        default:
            break;
        }
    }

    // raw_opts are not available here; --libplacebo-* options flow through gl_video_opts.
}

static void apply_target_contrast_libmpv(struct priv *p,
                                          struct pl_color_space *color)
{
    const struct gl_video_opts *opts = p->opts_cache->opts;

    if (!opts->target_contrast) // 0 = auto, let libplacebo infer from display
        return;
    if (opts->target_contrast == -1) {
        color->hdr.min_luma = 1e-7f; // infinite contrast
        return;
    }

    pl_color_space_nominal_luma_ex(pl_nominal_luma_params(
        .color    = color,
        .metadata = PL_HDR_METADATA_HDR10,
        .scaling  = PL_HDR_NITS,
        .out_max  = &color->hdr.max_luma,
    ));
    color->hdr.min_luma = color->hdr.max_luma / opts->target_contrast;
}

static int get_target_size(struct render_backend *ctx, mpv_render_param *params,
                           int *out_w, int *out_h)
{
    struct priv *p = ctx->priv;
    return p->ctx->fns->get_target_size(p->ctx, params, out_w, out_h);
}

static int render(struct render_backend *ctx, mpv_render_param *params,
                  struct vo_frame *frame)
{
    struct priv *p = ctx->priv;

    m_config_cache_update(p->opts_cache);
    update_render_options_libmpv(p);
    const struct gl_video_opts *vopts = p->opts_cache->opts;
    bool hdr_target = vopts->target_trc &&
                      pl_color_transfer_is_hdr(vopts->target_trc);
    if (hdr_target) {
        p->ctx->target_color_space = (struct pl_color_space) {
            .transfer  = vopts->target_trc,
            .primaries = vopts->target_prim,
            .hdr.max_luma = vopts->target_peak,
        };
    } else {
        p->ctx->target_color_space = pl_color_space_srgb;
    }
    if (p->ctx->fns->colorspace_hint)
        p->ctx->fns->colorspace_hint(p->ctx,
                                     hdr_target ? &p->ctx->target_color_space
                                                : NULL);

    pl_tex target_tex = NULL;
    int err = p->ctx->fns->start_frame(p->ctx, params, &target_tex);
    if (err < 0)
        return err;

    // get_target_size() and start_frame() race against layer.drawableSize changes.
    // If the drawable size changed, scale eff_dst/eff_osd to match this frame;
    // leave p->dst/osd_res unchanged so the next resize() corrects them.
    int actual_w = (int) target_tex->params.w;
    int actual_h = (int) target_tex->params.h;
    struct mp_rect    eff_dst = p->dst;
    struct mp_osd_res eff_osd = p->osd_res;
    if (p->osd_res.w > 0 && p->osd_res.h > 0 &&
        (actual_w != p->osd_res.w || actual_h != p->osd_res.h))
    {
        float sx = (float) actual_w / p->osd_res.w;
        float sy = (float) actual_h / p->osd_res.h;
        eff_dst.x0 = (int)(eff_dst.x0 * sx);
        eff_dst.x1 = (int)(eff_dst.x1 * sx);
        eff_dst.y0 = (int)(eff_dst.y0 * sy);
        eff_dst.y1 = (int)(eff_dst.y1 * sy);
        eff_osd.w  = actual_w;
        eff_osd.h  = actual_h;
    }

    bool flip = GET_MPV_RENDER_PARAM(params, MPV_RENDER_PARAM_FLIP_Y, int, 0);

    struct pl_rect2df dst_crop = {
        .x0 = eff_dst.x0, .x1 = eff_dst.x1,
        .y0 = flip ? eff_dst.y1 : eff_dst.y0,
        .y1 = flip ? eff_dst.y0 : eff_dst.y1,
    };

    // start_frame() may have updated target_color_space from the swapchain.
    struct pl_color_space target_csp = p->ctx->target_color_space;
    if (target_csp.transfer == PL_COLOR_TRC_UNKNOWN)
        target_csp = pl_color_space_srgb;

    struct pl_frame target_frame = {
        .num_planes = 1,
        .planes[0]  = { .texture = target_tex, .components = 4,
                        .component_mapping = {0, 1, 2, 3} },
        .crop       = dst_crop,
        .repr       = pl_color_repr_rgb,
        .color      = target_csp,
    };

    apply_target_contrast_libmpv(p, &target_frame.color);

    // Override sample depth from the dither_depth option.
    {
        int dither_depth = vopts->dither_depth;
        if (dither_depth == 0)
            dither_depth = target_frame.repr.bits.sample_depth; // auto: match surface
        if (dither_depth > 0) {
            struct pl_bit_encoding *tbits = &target_frame.repr.bits;
            tbits->color_depth  += dither_depth - tbits->sample_depth;
            tbits->sample_depth  = dither_depth;
        }
    }

    // SDR content on an HDR display: set the reference white point.
    if (vopts->hdr_reference_white && !pl_color_transfer_is_hdr(target_csp.transfer) &&
        !target_frame.color.hdr.max_luma)
    {
        target_frame.color.hdr.max_luma = vopts->hdr_reference_white;
    }

    if (vopts->target_gamut)
        mp_parse_raw_primaries(mp_null_log, vopts->target_gamut, &target_frame.color.hdr.prim);

    if (frame && frame->current && frame->frame_id != p->last_id) {
        struct mp_image *mpi = mp_image_new_ref(frame->current);
        struct frame_priv *fp = talloc_zero(mpi, struct frame_priv);
        fp->p = p;
        mpi->priv = fp;

        pl_queue_push(p->queue, &(struct pl_source_frame) {
            .pts        = mpi->pts,
            .duration   = frame->ideal_frame_vsync_duration,
            .frame_data = mpi,
            .map        = map_frame,
            .unmap      = unmap_frame,
            .discard    = discard_frame,
        });
        p->last_id  = frame->frame_id;
        p->last_pts = mpi->pts;
    }

    double pts = p->last_pts;
    struct pl_frame_mix mix = {0};
    pl_queue_update(p->queue, &mix, pl_queue_params(.pts = pts));

    // signatures derived from user_data (mp_image *) pointers.
    uint64_t sigs[64] = {0};
    int num_frames = MPMIN(mix.num_frames, MP_ARRAY_SIZE(sigs));
    for (int i = 0; i < num_frames; i++)
        sigs[i] = (uintptr_t) mix.frames[i]->user_data;
    mix.signatures = sigs;
    mix.num_frames = num_frames;

    // map_frame() leaves crop zeroed; set it now to avoid degenerate GLSL.
    // Cast away const — we own the frames through user_data.
    for (int i = 0; i < mix.num_frames; i++) {
        struct pl_frame *image = (struct pl_frame *) mix.frames[i];
        image->crop = (struct pl_rect2df) {
            .x0 = p->src.x0, .y0 = p->src.y0,
            .x1 = p->src.x1, .y1 = p->src.y1,
        };
        // mpv gives us rotated rects; libplacebo expects unrotated
        pl_rect2df_rotate(&image->crop, -image->rotation);
        if (image->crop.x1 < image->crop.x0) {
            image->crop.x0 = p->img_params.w - image->crop.x0;
            image->crop.x1 = p->img_params.w - image->crop.x1;
        }
        if (image->crop.y1 < image->crop.y0) {
            image->crop.y0 = p->img_params.h - image->crop.y0;
            image->crop.y1 = p->img_params.h - image->crop.y1;
        }
    }

    double osd_pts = (frame && frame->current) ? frame->current->pts : pts;
    update_overlays(p, eff_osd, 0, PL_OVERLAY_COORDS_DST_FRAME,
                    &p->osd_state, &target_frame, osd_pts);

    {
        struct mp_csp_params csp = MP_CSP_PARAMS_DEFAULTS;
        mp_csp_equalizer_state_get(p->video_eq, &csp);
        p->pars->color_adjustment.brightness = csp.brightness;
        p->pars->color_adjustment.contrast   = csp.contrast;
        p->pars->color_adjustment.hue        = csp.hue;
        p->pars->color_adjustment.saturation = csp.saturation;
        p->pars->color_adjustment.gamma      = csp.gamma * vopts->gamma;
    }

    struct pl_render_params rparams = p->pars->params;
    rparams.color_adjustment = &p->pars->color_adjustment;
    if (!pl_render_image_mix(p->renderer, &mix, &target_frame, &rparams))
        MP_WARN(p->ctx, "pl_render_image_mix failed.\n");

    p->ctx->fns->end_frame(p->ctx, &target_tex);

    return 0;
}

static void destroy(struct render_backend *ctx)
{
    struct priv *p = ctx->priv;
    if (!p)
        return;

    hwdec_devices_destroy(ctx->hwdec_devs);
    ctx->hwdec_devs = NULL;

    if (p->ctx && p->ctx->gpu) {
        for (int i = 0; i < MAX_OSD_PARTS; i++)
            pl_tex_destroy(p->ctx->gpu, &p->osd_state.entries[i].tex);
        for (int i = 0; i < p->num_sub_tex; i++)
            pl_tex_destroy(p->ctx->gpu, &p->sub_tex[i]);
    }

    if (p->pars)
        pl_options_free(&p->pars);
    if (p->queue)
        pl_queue_destroy(&p->queue);
    if (p->renderer)
        pl_renderer_destroy(&p->renderer);

    if (p->ctx && p->ctx->fns)
        p->ctx->fns->destroy(p->ctx);

    talloc_free(p);
    ctx->priv = NULL;
}

const struct render_backend_fns render_backend_gpu_next = {
    .init           = init,
    .check_format   = check_format,
    .set_parameter  = set_parameter,
    .reconfig       = reconfig,
    .reset          = reset,
    .screenshot     = screenshot,
    .perfdata       = perfdata,
    .get_image      = get_image,
    .update_external = update_external,
    .resize         = resize,
    .get_target_size = get_target_size,
    .render         = render,
    .destroy        = destroy,
};

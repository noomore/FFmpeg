/*
 * Copyright (c) 2010 Stefano Sabatini
 * Copyright (c) 2008 Vitor Sessak
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * transposition filter
 * Based on MPlayer libmpcodecs/vf_rotate.c.
 */

#include <stdio.h>

#include "libavutil/intreadwrite.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/imgutils.h"
#include "libavutil/internal.h"
#include "avfilter.h"
#include "formats.h"
#include "internal.h"
#include "video.h"

typedef enum {
    TRANSPOSE_PT_TYPE_NONE,
    TRANSPOSE_PT_TYPE_LANDSCAPE,
    TRANSPOSE_PT_TYPE_PORTRAIT,
} PassthroughType;

typedef struct {
    const AVClass *class;
    int hsub, vsub;
    int pixsteps[4];

    /* 0    Rotate by 90 degrees counterclockwise and vflip. */
    /* 1    Rotate by 90 degrees clockwise.                  */
    /* 2    Rotate by 90 degrees counterclockwise.           */
    /* 3    Rotate by 90 degrees clockwise and vflip.        */
    int dir;
    PassthroughType passthrough; ///< landscape passthrough mode enabled
} TransContext;

#define OFFSET(x) offsetof(TransContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM

static const AVOption transpose_options[] = {
    { "dir", "set transpose direction", OFFSET(dir), AV_OPT_TYPE_INT, {.i64=0},  0, 7, FLAGS },

    { "passthrough", "do not apply transposition if the input matches the specified geometry",
      OFFSET(passthrough), AV_OPT_TYPE_INT, {.i64=TRANSPOSE_PT_TYPE_NONE},  0, INT_MAX, FLAGS, "passthrough" },
    { "none",      "always apply transposition",   0, AV_OPT_TYPE_CONST, {.i64=TRANSPOSE_PT_TYPE_NONE},      INT_MIN, INT_MAX, FLAGS, "passthrough" },
    { "portrait",  "preserve portrait geometry",   0, AV_OPT_TYPE_CONST, {.i64=TRANSPOSE_PT_TYPE_PORTRAIT},  INT_MIN, INT_MAX, FLAGS, "passthrough" },
    { "landscape", "preserve landscape geometry",  0, AV_OPT_TYPE_CONST, {.i64=TRANSPOSE_PT_TYPE_LANDSCAPE}, INT_MIN, INT_MAX, FLAGS, "passthrough" },

    { NULL },
};

AVFILTER_DEFINE_CLASS(transpose);

static av_cold int init(AVFilterContext *ctx, const char *args)
{
    TransContext *trans = ctx->priv;
    const char *shorthand[] = { "dir", "passthrough", NULL };

    trans->class = &transpose_class;
    av_opt_set_defaults(trans);

    return av_opt_set_from_string(trans, args, shorthand, "=", ":");
}

static int query_formats(AVFilterContext *ctx)
{
    enum AVPixelFormat pix_fmts[] = {
        AV_PIX_FMT_ARGB,         AV_PIX_FMT_RGBA,
        AV_PIX_FMT_ABGR,         AV_PIX_FMT_BGRA,
        AV_PIX_FMT_RGB24,        AV_PIX_FMT_BGR24,
        AV_PIX_FMT_RGB565BE,     AV_PIX_FMT_RGB565LE,
        AV_PIX_FMT_RGB555BE,     AV_PIX_FMT_RGB555LE,
        AV_PIX_FMT_BGR565BE,     AV_PIX_FMT_BGR565LE,
        AV_PIX_FMT_BGR555BE,     AV_PIX_FMT_BGR555LE,
        AV_PIX_FMT_GRAY16BE,     AV_PIX_FMT_GRAY16LE,
        AV_PIX_FMT_YUV420P16LE,  AV_PIX_FMT_YUV420P16BE,
        AV_PIX_FMT_YUV444P16LE,  AV_PIX_FMT_YUV444P16BE,
        AV_PIX_FMT_NV12,         AV_PIX_FMT_NV21,
        AV_PIX_FMT_RGB8,         AV_PIX_FMT_BGR8,
        AV_PIX_FMT_RGB4_BYTE,    AV_PIX_FMT_BGR4_BYTE,
        AV_PIX_FMT_YUV444P,      AV_PIX_FMT_YUVJ444P,
        AV_PIX_FMT_YUV420P,      AV_PIX_FMT_YUVJ420P,
        AV_PIX_FMT_YUV410P,
        AV_PIX_FMT_YUVA420P,     AV_PIX_FMT_GRAY8,
        AV_PIX_FMT_NONE
    };

    ff_set_common_formats(ctx, ff_make_format_list(pix_fmts));
    return 0;
}

static int config_props_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    TransContext *trans = ctx->priv;
    AVFilterLink *inlink = ctx->inputs[0];
    const AVPixFmtDescriptor *desc_out = av_pix_fmt_desc_get(outlink->format);
    const AVPixFmtDescriptor *desc_in  = av_pix_fmt_desc_get(inlink->format);

    if (trans->dir&4) {
        av_log(ctx, AV_LOG_WARNING,
               "dir values greater than 3 are deprecated, use the passthrough option instead\n");
        trans->dir &= 3;
        trans->passthrough = TRANSPOSE_PT_TYPE_LANDSCAPE;
    }

    if ((inlink->w >= inlink->h && trans->passthrough == TRANSPOSE_PT_TYPE_LANDSCAPE) ||
        (inlink->w <= inlink->h && trans->passthrough == TRANSPOSE_PT_TYPE_PORTRAIT)) {
        av_log(ctx, AV_LOG_VERBOSE,
               "w:%d h:%d -> w:%d h:%d (passthrough mode)\n",
               inlink->w, inlink->h, inlink->w, inlink->h);
        return 0;
    } else {
        trans->passthrough = TRANSPOSE_PT_TYPE_NONE;
    }

    trans->hsub = desc_in->log2_chroma_w;
    trans->vsub = desc_in->log2_chroma_h;

    av_image_fill_max_pixsteps(trans->pixsteps, NULL, desc_out);

    outlink->w = inlink->h;
    outlink->h = inlink->w;

    if (inlink->sample_aspect_ratio.num){
        outlink->sample_aspect_ratio = av_div_q((AVRational){1,1}, inlink->sample_aspect_ratio);
    } else
        outlink->sample_aspect_ratio = inlink->sample_aspect_ratio;

    av_log(ctx, AV_LOG_VERBOSE, "w:%d h:%d dir:%d -> w:%d h:%d rotation:%s vflip:%d\n",
           inlink->w, inlink->h, trans->dir, outlink->w, outlink->h,
           trans->dir == 1 || trans->dir == 3 ? "clockwise" : "counterclockwise",
           trans->dir == 0 || trans->dir == 3);
    return 0;
}

static AVFilterBufferRef *get_video_buffer(AVFilterLink *inlink, int perms, int w, int h)
{
    TransContext *trans = inlink->dst->priv;

    return trans->passthrough ?
        ff_null_get_video_buffer   (inlink, perms, w, h) :
        ff_default_get_video_buffer(inlink, perms, w, h);
}

static int start_frame(AVFilterLink *inlink, AVFilterBufferRef *picref)
{
    TransContext *trans = inlink->dst->priv;
    AVFilterLink *outlink = inlink->dst->outputs[0];
    AVFilterBufferRef *buf_out;

    if (trans->passthrough)
        return ff_null_start_frame(inlink, picref);

    outlink->out_buf = ff_get_video_buffer(outlink, AV_PERM_WRITE,
                                           outlink->w, outlink->h);
    if (!outlink->out_buf)
        return AVERROR(ENOMEM);

    outlink->out_buf->pts = picref->pts;

    if (picref->video->sample_aspect_ratio.num == 0) {
        outlink->out_buf->video->sample_aspect_ratio = picref->video->sample_aspect_ratio;
    } else {
        outlink->out_buf->video->sample_aspect_ratio.num = picref->video->sample_aspect_ratio.den;
        outlink->out_buf->video->sample_aspect_ratio.den = picref->video->sample_aspect_ratio.num;
    }

    buf_out = avfilter_ref_buffer(outlink->out_buf, ~0);
    if (!buf_out)
        return AVERROR(ENOMEM);
    return ff_start_frame(outlink, buf_out);
}

static int end_frame(AVFilterLink *inlink)
{
    TransContext *trans = inlink->dst->priv;
    AVFilterBufferRef *inpic  = inlink->cur_buf;
    AVFilterBufferRef *outpic = inlink->dst->outputs[0]->out_buf;
    AVFilterLink *outlink = inlink->dst->outputs[0];
    int plane, ret;

    if (trans->passthrough)
        return ff_null_end_frame(inlink);

    for (plane = 0; outpic->data[plane]; plane++) {
        int hsub = plane == 1 || plane == 2 ? trans->hsub : 0;
        int vsub = plane == 1 || plane == 2 ? trans->vsub : 0;
        int pixstep = trans->pixsteps[plane];
        int inh  = inpic->video->h>>vsub;
        int outw = outpic->video->w>>hsub;
        int outh = outpic->video->h>>vsub;
        uint8_t *out, *in;
        int outlinesize, inlinesize;
        int x, y;

        out = outpic->data[plane]; outlinesize = outpic->linesize[plane];
        in  = inpic ->data[plane]; inlinesize  = inpic ->linesize[plane];

        if (trans->dir&1) {
            in +=  inpic->linesize[plane] * (inh-1);
            inlinesize *= -1;
        }

        if (trans->dir&2) {
            out += outpic->linesize[plane] * (outh-1);
            outlinesize *= -1;
        }

        for (y = 0; y < outh; y++) {
            switch (pixstep) {
            case 1:
                for (x = 0; x < outw; x++)
                    out[x] = in[x*inlinesize + y];
                break;
            case 2:
                for (x = 0; x < outw; x++)
                    *((uint16_t *)(out + 2*x)) = *((uint16_t *)(in + x*inlinesize + y*2));
                break;
            case 3:
                for (x = 0; x < outw; x++) {
                    int32_t v = AV_RB24(in + x*inlinesize + y*3);
                    AV_WB24(out + 3*x, v);
                }
                break;
            case 4:
                for (x = 0; x < outw; x++)
                    *((uint32_t *)(out + 4*x)) = *((uint32_t *)(in + x*inlinesize + y*4));
                break;
            }
            out += outlinesize;
        }
    }

    if ((ret = ff_draw_slice(outlink, 0, outpic->video->h, 1)) < 0 ||
        (ret = ff_end_frame(outlink)) < 0)
        return ret;
    return 0;
}

static int draw_slice(AVFilterLink *inlink, int y, int h, int slice_dir)
{
    TransContext *trans = inlink->dst->priv;

    return trans->passthrough ? ff_null_draw_slice(inlink, y, h, slice_dir) : 0;
}

static const AVFilterPad avfilter_vf_transpose_inputs[] = {
    {
        .name        = "default",
        .type        = AVMEDIA_TYPE_VIDEO,
        .get_video_buffer= get_video_buffer,
        .start_frame = start_frame,
        .draw_slice  = draw_slice,
        .end_frame   = end_frame,
        .min_perms   = AV_PERM_READ,
    },
    { NULL }
};

static const AVFilterPad avfilter_vf_transpose_outputs[] = {
    {
        .name         = "default",
        .config_props = config_props_output,
        .type         = AVMEDIA_TYPE_VIDEO,
    },
    { NULL }
};

AVFilter avfilter_vf_transpose = {
    .name      = "transpose",
    .description = NULL_IF_CONFIG_SMALL("Transpose input video."),

    .init = init,
    .priv_size = sizeof(TransContext),

    .query_formats = query_formats,

    .inputs    = avfilter_vf_transpose_inputs,
    .outputs   = avfilter_vf_transpose_outputs,
    .priv_class = &transpose_class,
};

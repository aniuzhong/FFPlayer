#include <math.h>
#include <stdio.h>

#include <libavutil/bprint.h>
#include <libavutil/display.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavfilter/buffersrc.h>

#include "filter.h"

static const struct TextureFormatEntry {
    enum AVPixelFormat format;
    int texture_fmt;
} sdl_texture_format_map[] = {
    { AV_PIX_FMT_RGB8,           SDL_PIXELFORMAT_RGB332 },
    { AV_PIX_FMT_RGB444,         SDL_PIXELFORMAT_RGB444 },
    { AV_PIX_FMT_RGB555,         SDL_PIXELFORMAT_RGB555 },
    { AV_PIX_FMT_BGR555,         SDL_PIXELFORMAT_BGR555 },
    { AV_PIX_FMT_RGB565,         SDL_PIXELFORMAT_RGB565 },
    { AV_PIX_FMT_BGR565,         SDL_PIXELFORMAT_BGR565 },
    { AV_PIX_FMT_RGB24,          SDL_PIXELFORMAT_RGB24 },
    { AV_PIX_FMT_BGR24,          SDL_PIXELFORMAT_BGR24 },
    { AV_PIX_FMT_0RGB32,         SDL_PIXELFORMAT_RGB888 },
    { AV_PIX_FMT_0BGR32,         SDL_PIXELFORMAT_BGR888 },
    { AV_PIX_FMT_NE(RGB0, 0BGR), SDL_PIXELFORMAT_RGBX8888 },
    { AV_PIX_FMT_NE(BGR0, 0RGB), SDL_PIXELFORMAT_BGRX8888 },
    { AV_PIX_FMT_RGB32,          SDL_PIXELFORMAT_ARGB8888 },
    { AV_PIX_FMT_RGB32_1,        SDL_PIXELFORMAT_RGBA8888 },
    { AV_PIX_FMT_BGR32,          SDL_PIXELFORMAT_ABGR8888 },
    { AV_PIX_FMT_BGR32_1,        SDL_PIXELFORMAT_BGRA8888 },
    { AV_PIX_FMT_YUV420P,        SDL_PIXELFORMAT_IYUV },
    { AV_PIX_FMT_YUYV422,        SDL_PIXELFORMAT_YUY2 },
    { AV_PIX_FMT_UYVY422,        SDL_PIXELFORMAT_UYVY },
};

static enum AVColorSpace sdl_supported_color_spaces[] = {
    AVCOL_SPC_BT709,
    AVCOL_SPC_BT470BG,
    AVCOL_SPC_SMPTE170M,
};

static enum AVAlphaMode sdl_supported_alpha_modes[] = {
    AVALPHA_MODE_UNSPECIFIED,
    AVALPHA_MODE_STRAIGHT,
};

static double get_rotation(const int32_t *displaymatrix)
{
    double theta = 0;
    if (displaymatrix)
        theta = -round(av_display_rotation_get(displaymatrix));
    theta -= 360 * floor(theta / 360 + 0.9 / 360);
    return theta;
}

static int configure_filtergraph(AVFilterGraph *graph, const char *filtergraph,
                                 AVFilterContext *source_ctx, AVFilterContext *sink_ctx)
{
    int ret, i;
    int nb_filters = graph->nb_filters;
    AVFilterInOut *outputs = NULL, *inputs = NULL;

    if (filtergraph) {
        outputs = avfilter_inout_alloc();
        inputs  = avfilter_inout_alloc();
        if (!outputs || !inputs) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }

        outputs->name       = av_strdup("in");
        outputs->filter_ctx = source_ctx;
        outputs->pad_idx    = 0;
        outputs->next       = NULL;

        inputs->name        = av_strdup("out");
        inputs->filter_ctx  = sink_ctx;
        inputs->pad_idx     = 0;
        inputs->next        = NULL;

        if ((ret = avfilter_graph_parse_ptr(graph, filtergraph, &inputs, &outputs, NULL)) < 0)
            goto fail;
    } else {
        if ((ret = avfilter_link(source_ctx, 0, sink_ctx, 0)) < 0)
            goto fail;
    }

    for (i = 0; i < graph->nb_filters - nb_filters; i++)
        FFSWAP(AVFilterContext*, graph->filters[i], graph->filters[i + nb_filters]);

    ret = avfilter_graph_config(graph, NULL);
fail:
    avfilter_inout_free(&outputs);
    avfilter_inout_free(&inputs);
    return ret;
}

int configure_video_filters(AVFilterGraph *graph, VideoState *is, const char *vfilters, AVFrame *frame, const SDL_RendererInfo *renderer_info)
{
    enum AVPixelFormat pix_fmts[FF_ARRAY_ELEMS(sdl_texture_format_map)];
    int ret;
    AVFilterContext *filt_src = NULL, *filt_out = NULL, *last_filter = NULL;
    AVCodecParameters *codecpar = is->video_st->codecpar;
    AVRational fr = av_guess_frame_rate(demuxer_get_ic(is->demuxer), is->video_st, NULL);
    int nb_pix_fmts = 0;
    int i, j;
    AVBufferSrcParameters *par = av_buffersrc_parameters_alloc();

    if (!par)
        return AVERROR(ENOMEM);

    for (i = 0; i < renderer_info->num_texture_formats; i++) {
        for (j = 0; j < FF_ARRAY_ELEMS(sdl_texture_format_map); j++) {
            if (renderer_info->texture_formats[i] == sdl_texture_format_map[j].texture_fmt) {
                pix_fmts[nb_pix_fmts++] = sdl_texture_format_map[j].format;
                break;
            }
        }
    }

    graph->scale_sws_opts = av_strdup("");
    filt_src = avfilter_graph_alloc_filter(graph, avfilter_get_by_name("buffer"), "ffplay_buffer");
    if (!filt_src) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    par->format = frame->format;
    par->time_base = is->video_st->time_base;
    par->width = frame->width;
    par->height = frame->height;
    par->sample_aspect_ratio = codecpar->sample_aspect_ratio;
    par->color_space = frame->colorspace;
    par->color_range = frame->color_range;
    par->alpha_mode = frame->alpha_mode;
    par->frame_rate = fr;
    par->hw_frames_ctx = frame->hw_frames_ctx;
    ret = av_buffersrc_parameters_set(filt_src, par);
    if (ret < 0)
        goto fail;
    ret = avfilter_init_dict(filt_src, NULL);
    if (ret < 0)
        goto fail;

    filt_out = avfilter_graph_alloc_filter(graph, avfilter_get_by_name("buffersink"), "ffplay_buffersink");
    if (!filt_out) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    if ((ret = av_opt_set_array(filt_out, "pixel_formats", AV_OPT_SEARCH_CHILDREN, 0, nb_pix_fmts, AV_OPT_TYPE_PIXEL_FMT, pix_fmts)) < 0)
        goto fail;
    if ((ret = av_opt_set_array(filt_out, "colorspaces", AV_OPT_SEARCH_CHILDREN, 0, FF_ARRAY_ELEMS(sdl_supported_color_spaces), AV_OPT_TYPE_INT, sdl_supported_color_spaces)) < 0)
        goto fail;
    if ((ret = av_opt_set_array(filt_out, "alphamodes", AV_OPT_SEARCH_CHILDREN, 0, FF_ARRAY_ELEMS(sdl_supported_alpha_modes), AV_OPT_TYPE_INT, sdl_supported_alpha_modes)) < 0)
        goto fail;
    ret = avfilter_init_dict(filt_out, NULL);
    if (ret < 0)
        goto fail;

    last_filter = filt_out;
#define INSERT_FILT(name, arg) do { \
    AVFilterContext *filt_ctx; \
    ret = avfilter_graph_create_filter(&filt_ctx, avfilter_get_by_name(name), "ffplay_" name, arg, NULL, graph); \
    if (ret < 0) goto fail; \
    ret = avfilter_link(filt_ctx, 0, last_filter, 0); \
    if (ret < 0) goto fail; \
    last_filter = filt_ctx; \
} while (0)

    {
        double theta = 0.0;
        int32_t *displaymatrix = NULL;
        AVFrameSideData *sd = av_frame_get_side_data(frame, AV_FRAME_DATA_DISPLAYMATRIX);
        if (sd)
            displaymatrix = (int32_t *)sd->data;
        if (!displaymatrix) {
            const AVPacketSideData *psd = av_packet_side_data_get(is->video_st->codecpar->coded_side_data,
                                                                  is->video_st->codecpar->nb_coded_side_data,
                                                                  AV_PKT_DATA_DISPLAYMATRIX);
            if (psd)
                displaymatrix = (int32_t *)psd->data;
        }
        theta = get_rotation(displaymatrix);
        if (fabs(theta - 90) < 1.0) {
            INSERT_FILT("transpose", displaymatrix[3] > 0 ? "cclock_flip" : "clock");
        } else if (fabs(theta - 180) < 1.0) {
            if (displaymatrix[0] < 0) INSERT_FILT("hflip", NULL);
            if (displaymatrix[4] < 0) INSERT_FILT("vflip", NULL);
        } else if (fabs(theta - 270) < 1.0) {
            INSERT_FILT("transpose", displaymatrix[3] < 0 ? "clock_flip" : "cclock");
        } else if (fabs(theta) > 1.0) {
            char rotate_buf[64];
            snprintf(rotate_buf, sizeof(rotate_buf), "%f*PI/180", theta);
            INSERT_FILT("rotate", rotate_buf);
        } else if (displaymatrix && displaymatrix[4] < 0) {
            INSERT_FILT("vflip", NULL);
        }
    }

    if ((ret = configure_filtergraph(graph, vfilters, filt_src, last_filter)) < 0)
        goto fail;
    is->in_video_filter = filt_src;
    is->out_video_filter = filt_out;
fail:
    av_freep(&par);
    return ret;
}

int configure_audio_filters(VideoState *is, const char *afilters, int force_output_format)
{
    AVFilterContext *filt_asrc = NULL, *filt_asink = NULL;
    AVBPrint bp;
    char asrc_args[256];
    int ret;

    avfilter_graph_free(&is->agraph);
    if (!(is->agraph = avfilter_graph_alloc()))
        return AVERROR(ENOMEM);
    av_bprint_init(&bp, 0, AV_BPRINT_SIZE_AUTOMATIC);
    av_channel_layout_describe_bprint(&is->audio_filter_src.ch_layout, &bp);
    ret = snprintf(asrc_args, sizeof(asrc_args),
                   "sample_rate=%d:sample_fmt=%s:time_base=%d/%d:channel_layout=%s",
                   is->audio_filter_src.freq, av_get_sample_fmt_name(is->audio_filter_src.fmt),
                   1, is->audio_filter_src.freq, bp.str);

    ret = avfilter_graph_create_filter(&filt_asrc, avfilter_get_by_name("abuffer"), "ffplay_abuffer", asrc_args, NULL, is->agraph);
    if (ret < 0) goto end;
    filt_asink = avfilter_graph_alloc_filter(is->agraph, avfilter_get_by_name("abuffersink"), "ffplay_abuffersink");
    if (!filt_asink) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    if ((ret = av_opt_set(filt_asink, "sample_formats", "s16", AV_OPT_SEARCH_CHILDREN)) < 0) goto end;
    if (force_output_format) {
        if ((ret = av_opt_set_array(filt_asink, "channel_layouts", AV_OPT_SEARCH_CHILDREN, 0, 1, AV_OPT_TYPE_CHLAYOUT, &is->audio_tgt.ch_layout)) < 0) goto end;
        if ((ret = av_opt_set_array(filt_asink, "samplerates", AV_OPT_SEARCH_CHILDREN, 0, 1, AV_OPT_TYPE_INT, &is->audio_tgt.freq)) < 0) goto end;
    }
    ret = avfilter_init_dict(filt_asink, NULL);
    if (ret < 0) goto end;
    if ((ret = configure_filtergraph(is->agraph, afilters, filt_asrc, filt_asink)) < 0) goto end;
    is->in_audio_filter = filt_asrc;
    is->out_audio_filter = filt_asink;
end:
    if (ret < 0)
        avfilter_graph_free(&is->agraph);
    av_bprint_finalize(&bp, NULL);
    return ret;
}

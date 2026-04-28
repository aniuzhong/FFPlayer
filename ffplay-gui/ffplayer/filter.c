#include <math.h>
#include <stdio.h>

#include <libavutil/bprint.h>
#include <libavutil/display.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavfilter/buffersrc.h>

#include "filter.h"
#include "demuxer.h"

static enum AVColorSpace supported_color_spaces[] = {
    AVCOL_SPC_BT709,
    AVCOL_SPC_BT470BG,
    AVCOL_SPC_SMPTE170M,
};

static enum AVAlphaMode supported_alpha_modes[] = {
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

int configure_video_filters(AVFilterGraph *graph,
                            Demuxer *demuxer,
                            AVStream *video_st,
                            const char *vfilters,
                            AVFrame *frame,
                            const enum AVPixelFormat *pix_fmts,
                            int nb_pix_fmts,
                            AVFilterContext **in_filter,
                            AVFilterContext **out_filter)
{
    int ret;
    AVFilterContext *filt_src = NULL, *filt_out = NULL, *last_filter = NULL;
    AVCodecParameters *codecpar = video_st->codecpar;
    AVRational fr = demuxer_guess_frame_rate(demuxer, demuxer_get_stream_index(demuxer, AVMEDIA_TYPE_VIDEO), NULL);
    AVBufferSrcParameters *par = av_buffersrc_parameters_alloc();

    if (!par)
        return AVERROR(ENOMEM);

    graph->scale_sws_opts = av_strdup("");
    filt_src = avfilter_graph_alloc_filter(graph, avfilter_get_by_name("buffer"), "ffplay_buffer");
    if (!filt_src) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    par->format = frame->format;
    par->time_base = video_st->time_base;
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
    if ((ret = av_opt_set_array(filt_out, "colorspaces", AV_OPT_SEARCH_CHILDREN, 0, FF_ARRAY_ELEMS(supported_color_spaces), AV_OPT_TYPE_INT, supported_color_spaces)) < 0)
        goto fail;
    if ((ret = av_opt_set_array(filt_out, "alphamodes", AV_OPT_SEARCH_CHILDREN, 0, FF_ARRAY_ELEMS(supported_alpha_modes), AV_OPT_TYPE_INT, supported_alpha_modes)) < 0)
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
            const AVPacketSideData *psd = av_packet_side_data_get(video_st->codecpar->coded_side_data,
                                                                  video_st->codecpar->nb_coded_side_data,
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
    *in_filter = filt_src;
    *out_filter = filt_out;
fail:
    av_freep(&par);
    return ret;
}

int configure_audio_filters(AVFilterGraph **agraph,
                            const struct AudioParams *src,
                            const struct AudioParams *tgt,
                            const char *afilters,
                            int force_output_format,
                            AVFilterContext **in_filter,
                            AVFilterContext **out_filter)
{
    AVFilterContext *filt_asrc = NULL, *filt_asink = NULL;
    AVBPrint bp;
    char asrc_args[256];
    int ret;

    avfilter_graph_free(agraph);
    if (!(*agraph = avfilter_graph_alloc()))
        return AVERROR(ENOMEM);
    av_bprint_init(&bp, 0, AV_BPRINT_SIZE_AUTOMATIC);
    av_channel_layout_describe_bprint(&src->ch_layout, &bp);
    ret = snprintf(asrc_args, sizeof(asrc_args),
                   "sample_rate=%d:sample_fmt=%s:time_base=%d/%d:channel_layout=%s",
                   src->freq, av_get_sample_fmt_name(src->fmt),
                   1, src->freq, bp.str);

    ret = avfilter_graph_create_filter(&filt_asrc, avfilter_get_by_name("abuffer"), "ffplay_abuffer", asrc_args, NULL, *agraph);
    if (ret < 0) goto end;
    filt_asink = avfilter_graph_alloc_filter(*agraph, avfilter_get_by_name("abuffersink"), "ffplay_abuffersink");
    if (!filt_asink) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    if ((ret = av_opt_set(filt_asink, "sample_formats", "s16", AV_OPT_SEARCH_CHILDREN)) < 0) goto end;
    if (force_output_format) {
        if ((ret = av_opt_set_array(filt_asink, "channel_layouts", AV_OPT_SEARCH_CHILDREN, 0, 1, AV_OPT_TYPE_CHLAYOUT, &tgt->ch_layout)) < 0) goto end;
        if ((ret = av_opt_set_array(filt_asink, "samplerates", AV_OPT_SEARCH_CHILDREN, 0, 1, AV_OPT_TYPE_INT, &tgt->freq)) < 0) goto end;
    }
    ret = avfilter_init_dict(filt_asink, NULL);
    if (ret < 0) goto end;
    if ((ret = configure_filtergraph(*agraph, afilters, filt_asrc, filt_asink)) < 0) goto end;
    *in_filter = filt_asrc;
    *out_filter = filt_asink;
end:
    if (ret < 0)
        avfilter_graph_free(agraph);
    av_bprint_finalize(&bp, NULL);
    return ret;
}

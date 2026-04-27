#ifndef FFPLAY_GUI_FILTER_H
#define FFPLAY_GUI_FILTER_H

#include <libavfilter/avfilter.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>

#include "audio_device.h"
#include "demuxer.h"

#ifdef __cplusplus
extern "C" {
#endif

int configure_video_filters(AVFilterGraph *graph,
                            Demuxer *demuxer,
                            AVStream *video_st,
                            const char *vfilters,
                            AVFrame *frame,
                            const enum AVPixelFormat *pix_fmts,
                            int nb_pix_fmts,
                            AVFilterContext **in_filter,
                            AVFilterContext **out_filter);
int configure_audio_filters(AVFilterGraph **agraph,
                            const struct AudioParams *src,
                            const struct AudioParams *tgt,
                            const char *afilters,
                            int force_output_format,
                            AVFilterContext **in_filter,
                            AVFilterContext **out_filter);

#ifdef __cplusplus
}
#endif

#endif

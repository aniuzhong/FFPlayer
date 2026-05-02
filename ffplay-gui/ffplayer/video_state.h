#ifndef FFPLAY_GUI_VIDEO_STATE_H
#define FFPLAY_GUI_VIDEO_STATE_H

#include <libavutil/buffer.h>
#include <libswscale/swscale.h>
#include <libavformat/avformat.h>
#include <libavfilter/avfilter.h>

#include "audio_pipeline.h"
#include "decoder.h"
#include "clock.h"
#include "av_sync.h"
#include "demuxer.h"

#define MAX_QUEUE_SIZE (15 * 1024 * 1024)
#define MIN_FRAMES 25

typedef struct AudioDevice AudioDevice;
typedef struct AudioVisualizer AudioVisualizer;

typedef struct VideoState {
    int force_refresh;
    int paused;
    int last_paused;
    int seek_req;
    int seek_flags;
    int64_t seek_pos;
    int64_t seek_rel;

    Clock *audclk;
    Clock *vidclk;
    Clock *extclk;
    AVSync av_sync;

    FrameQueue *pictq;
    FrameQueue *subpq;
    FrameQueue *sampq;

    Decoder auddec;
    Decoder viddec;
    Decoder subdec;

    int audio_stream;
    AVStream *audio_st;
    PacketQueue *audioq;
    struct AudioParams audio_filter_src;
    int frame_drops_early;
    int frame_drops_late;

    enum ShowMode show_mode;
    AudioPipeline *audio_pipeline;
    AudioVisualizer *audio_visualizer;

    int subtitle_stream;
    AVStream *subtitle_st;
    PacketQueue *subtitleq;

    double frame_timer;
    double frame_last_returned_time;
    double frame_last_filter_delay;
    int video_stream;
    AVStream *video_st;
    PacketQueue *videoq;

    int width, height, xleft, ytop;
    int step;

    AVFilterContext *in_video_filter;
    AVFilterContext *out_video_filter;
    AVFilterContext *in_audio_filter;
    AVFilterContext *out_audio_filter;
    AVFilterGraph *agraph;
    Demuxer demuxer;
    AudioDevice *audio_device;
    enum AVPixelFormat supported_pix_fmts[32];
    int nb_supported_pix_fmts;
    /* Owning ref. NULL when hardware acceleration is disabled or
     * unavailable; non-NULL frames will then take the SW filter path. */
    AVBufferRef *hw_device_ctx;
    int video_decoder_uses_hw;
    int hw_fallback_triggered;
    void (*on_frame_size_changed)(void *opaque, int width, int height, AVRational sar);
    void *frame_size_opaque;
    void (*on_step_frame)(struct VideoState *is);

    int last_video_stream, last_audio_stream, last_subtitle_stream;

    int quit_request;
} VideoState;

#endif

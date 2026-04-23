#ifndef FFPLAY_GUI_VIDEO_STATE_H
#define FFPLAY_GUI_VIDEO_STATE_H

#include <SDL.h>
#include <SDL_thread.h>

#include <libavutil/tx.h>
#include <libswscale/swscale.h>
#include <libavformat/avformat.h>
#include <libavfilter/avfilter.h>

#include "audio_pipeline.h"
#include "decoder.h"
#include "clock.h"
#include "av_sync.h"
#include "demuxer.h"

#define VIDEO_BACKGROUND_TILE_SIZE 64

#define MAX_QUEUE_SIZE (15 * 1024 * 1024)
#define MIN_FRAMES 25

#define SDL_VOLUME_STEP (0.75)

#define REFRESH_RATE 0.01

#define CURSOR_HIDE_DELAY 1000000

#define USE_ONEPASS_SUBTITLE_RENDER 1

#define FF_QUIT_EVENT    (SDL_USEREVENT + 2)

typedef struct AudioDevice AudioDevice;
typedef struct VideoRenderer VideoRenderer;

enum VideoBackgroundType {
    VIDEO_BACKGROUND_TILES,
    VIDEO_BACKGROUND_COLOR,
    VIDEO_BACKGROUND_NONE,
};

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
    AvSync av_sync;

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
    int last_i_start;
    AVTXContext *rdft;
    av_tx_fn rdft_fn;
    int rdft_bits;
    float *real_data;
    AVComplexFloat *rdft_data;
    int xpos;
    double last_vis_time;

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
    Demuxer *demuxer;
    AudioDevice *audio_device;
    VideoRenderer *video_renderer;
    void (*on_frame_size_changed)(struct VideoState *is, int width, int height, AVRational sar);
    void (*on_step_frame)(struct VideoState *is);

    int last_video_stream, last_audio_stream, last_subtitle_stream;
} VideoState;

#endif

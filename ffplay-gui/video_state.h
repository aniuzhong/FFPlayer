#ifndef FFPLAY_GUI_VIDEO_STATE_H
#define FFPLAY_GUI_VIDEO_STATE_H

#include <SDL.h>
#include <SDL_thread.h>

#include "libavutil/channel_layout.h"
#include "libavutil/samplefmt.h"
#include "libavutil/tx.h"
#include "libswresample/swresample.h"
#include "libswscale/swscale.h"
#include "libavformat/avformat.h"
#include "libavfilter/avfilter.h"

#include "decoder.h"
#include "clock.h"

#define VIDEO_BACKGROUND_TILE_SIZE 64

enum VideoBackgroundType {
    VIDEO_BACKGROUND_TILES,
    VIDEO_BACKGROUND_COLOR,
    VIDEO_BACKGROUND_NONE,
};

typedef struct RenderParams {
    SDL_Rect target_rect;
    uint8_t video_background_color[4];
    enum VideoBackgroundType video_background_type;
} RenderParams;

#define MAX_QUEUE_SIZE (15 * 1024 * 1024)
#define MIN_FRAMES 25

#define SDL_AUDIO_MIN_BUFFER_SIZE 512
#define SDL_AUDIO_MAX_CALLBACKS_PER_SEC 30

#define SDL_VOLUME_STEP (0.75)

#define SAMPLE_CORRECTION_PERCENT_MAX 10

#define AUDIO_DIFF_AVG_NB   20

#define REFRESH_RATE 0.01

#define SAMPLE_ARRAY_SIZE (8 * 65536)

#define CURSOR_HIDE_DELAY 1000000

#define USE_ONEPASS_SUBTITLE_RENDER 1

#define FF_QUIT_EVENT    (SDL_USEREVENT + 2)

typedef struct AudioParams {
    int freq;
    AVChannelLayout ch_layout;
    enum AVSampleFormat fmt;
    int frame_size;
    int bytes_per_sec;
} AudioParams;

enum ShowMode {
    SHOW_MODE_NONE = -1, SHOW_MODE_VIDEO = 0, SHOW_MODE_WAVES, SHOW_MODE_RDFT, SHOW_MODE_NB
};

typedef struct Demuxer Demuxer;
typedef struct AudioDevice AudioDevice;
typedef struct VideoRenderer VideoRenderer;

typedef struct VideoState {
    SDL_Thread *read_tid;
    int abort_request;
    int force_refresh;
    int paused;
    int last_paused;
    int queue_attachments_req;
    int seek_req;
    int seek_flags;
    int64_t seek_pos;
    int64_t seek_rel;
    int read_pause_return;
    AVFormatContext *ic;
    int realtime;

    Clock audclk;
    Clock vidclk;
    Clock extclk;

    FrameQueue *pictq;
    FrameQueue *subpq;
    FrameQueue *sampq;

    Decoder auddec;
    Decoder viddec;
    Decoder subdec;

    int audio_stream;
    int64_t audio_callback_time;

    double audio_clock;
    int audio_clock_serial;
    double audio_diff_cum;
    double audio_diff_avg_coef;
    double audio_diff_threshold;
    int audio_diff_avg_count;
    AVStream *audio_st;
    PacketQueue *audioq;
    int audio_hw_buf_size;
    uint8_t *audio_buf;
    uint8_t *audio_buf1;
    unsigned int audio_buf_size;
    unsigned int audio_buf1_size;
    int audio_buf_index;
    int audio_write_buf_size;
    int audio_volume;
    int muted;
    struct AudioParams audio_src;
    struct AudioParams audio_filter_src;
    struct AudioParams audio_tgt;
    struct SwrContext *swr_ctx;
    int frame_drops_early;
    int frame_drops_late;

    enum ShowMode show_mode;
    int16_t sample_array[SAMPLE_ARRAY_SIZE];
    int sample_array_index;
    int last_i_start;
    AVTXContext *rdft;
    av_tx_fn rdft_fn;
    int rdft_bits;
    float *real_data;
    AVComplexFloat *rdft_data;
    int xpos;
    double last_vis_time;
    RenderParams render_params;
    SDL_Texture *vis_texture;
    SDL_Texture *sub_texture;
    SDL_Texture *vid_texture;

    int subtitle_stream;
    AVStream *subtitle_st;
    PacketQueue *subtitleq;

    double frame_timer;
    double frame_last_returned_time;
    double frame_last_filter_delay;
    int video_stream;
    AVStream *video_st;
    PacketQueue *videoq;
    double max_frame_duration;
    struct SwsContext *sub_convert_ctx;
    int eof;

    char *filename;
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

    int last_video_stream, last_audio_stream, last_subtitle_stream;

    SDL_cond *continue_read_thread;
} VideoState;

#endif

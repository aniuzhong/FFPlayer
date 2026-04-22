#include "video_renderer.h"

#include <limits.h>
#include <math.h>
#include <string.h>

#include "av_sync.h"
#include "clock.h"
#include "stream.h"
#include "libavutil/common.h"
#include "libavutil/mathematics.h"
#include "libavutil/pixdesc.h"
#include "libavutil/time.h"

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

static inline void fill_rectangle(SDL_Renderer *renderer, int x, int y, int w, int h)
{
    SDL_Rect rect;
    rect.x = x;
    rect.y = y;
    rect.w = w;
    rect.h = h;
    if (w && h)
        SDL_RenderFillRect(renderer, &rect);
}

static int realloc_texture(SDL_Renderer *renderer, SDL_Texture **texture, Uint32 new_format, int new_width, int new_height, SDL_BlendMode blendmode, int init_texture)
{
    Uint32 format;
    int access, w, h;
    if (!*texture || SDL_QueryTexture(*texture, &format, &access, &w, &h) < 0 || new_width != w || new_height != h || new_format != format) {
        void *pixels;
        int pitch;
        if (*texture)
            SDL_DestroyTexture(*texture);
        if (!(*texture = SDL_CreateTexture(renderer, new_format, SDL_TEXTUREACCESS_STREAMING, new_width, new_height)))
            return -1;
        if (SDL_SetTextureBlendMode(*texture, blendmode) < 0)
            return -1;
        if (init_texture) {
            if (SDL_LockTexture(*texture, NULL, &pixels, &pitch) < 0)
                return -1;
            memset(pixels, 0, pitch * new_height);
            SDL_UnlockTexture(*texture);
        }
        av_log(NULL, AV_LOG_VERBOSE, "Created %dx%d texture with %s.\n", new_width, new_height, SDL_GetPixelFormatName(new_format));
    }
    return 0;
}

static void calculate_display_rect(SDL_Rect *rect,
                                   int scr_xleft, int scr_ytop, int scr_width, int scr_height,
                                   int pic_width, int pic_height, AVRational pic_sar)
{
    AVRational aspect_ratio = pic_sar;
    int64_t width, height, x, y;

    if (av_cmp_q(aspect_ratio, av_make_q(0, 1)) <= 0)
        aspect_ratio = av_make_q(1, 1);

    aspect_ratio = av_mul_q(aspect_ratio, av_make_q(pic_width, pic_height));

    height = scr_height;
    width = av_rescale(height, aspect_ratio.num, aspect_ratio.den) & ~1;
    if (width > scr_width) {
        width = scr_width;
        height = av_rescale(width, aspect_ratio.den, aspect_ratio.num) & ~1;
    }
    x = (scr_width - width) / 2;
    y = (scr_height - height) / 2;
    rect->x = scr_xleft + (int)x;
    rect->y = scr_ytop  + (int)y;
    rect->w = FFMAX((int)width,  1);
    rect->h = FFMAX((int)height, 1);
}

static void get_sdl_pix_fmt_and_blendmode(int format, Uint32 *sdl_pix_fmt, SDL_BlendMode *sdl_blendmode)
{
    int i;
    *sdl_blendmode = SDL_BLENDMODE_NONE;
    *sdl_pix_fmt = SDL_PIXELFORMAT_UNKNOWN;
    if (format == AV_PIX_FMT_RGB32   ||
        format == AV_PIX_FMT_RGB32_1 ||
        format == AV_PIX_FMT_BGR32   ||
        format == AV_PIX_FMT_BGR32_1)
        *sdl_blendmode = SDL_BLENDMODE_BLEND;
    for (i = 0; i < FF_ARRAY_ELEMS(sdl_texture_format_map); i++) {
        if (format == sdl_texture_format_map[i].format) {
            *sdl_pix_fmt = sdl_texture_format_map[i].texture_fmt;
            return;
        }
    }
}

static int upload_texture(SDL_Renderer *renderer, SDL_Texture **tex, AVFrame *frame)
{
    int ret = 0;
    Uint32 sdl_pix_fmt;
    SDL_BlendMode sdl_blendmode;
    get_sdl_pix_fmt_and_blendmode(frame->format, &sdl_pix_fmt, &sdl_blendmode);
    if (realloc_texture(renderer, tex, sdl_pix_fmt == SDL_PIXELFORMAT_UNKNOWN ? SDL_PIXELFORMAT_ARGB8888 : sdl_pix_fmt, frame->width, frame->height, sdl_blendmode, 0) < 0)
        return -1;
    switch (sdl_pix_fmt) {
        case SDL_PIXELFORMAT_IYUV:
            if (frame->linesize[0] > 0 && frame->linesize[1] > 0 && frame->linesize[2] > 0) {
                ret = SDL_UpdateYUVTexture(*tex, NULL, frame->data[0], frame->linesize[0],
                                                       frame->data[1], frame->linesize[1],
                                                       frame->data[2], frame->linesize[2]);
            } else if (frame->linesize[0] < 0 && frame->linesize[1] < 0 && frame->linesize[2] < 0) {
                ret = SDL_UpdateYUVTexture(*tex, NULL, frame->data[0] + frame->linesize[0] * (frame->height                    - 1), -frame->linesize[0],
                                                       frame->data[1] + frame->linesize[1] * (AV_CEIL_RSHIFT(frame->height, 1) - 1), -frame->linesize[1],
                                                       frame->data[2] + frame->linesize[2] * (AV_CEIL_RSHIFT(frame->height, 1) - 1), -frame->linesize[2]);
            } else {
                av_log(NULL, AV_LOG_ERROR, "Mixed negative and positive linesizes are not supported.\n");
                return -1;
            }
            break;
        default:
            if (frame->linesize[0] < 0) {
                ret = SDL_UpdateTexture(*tex, NULL, frame->data[0] + frame->linesize[0] * (frame->height - 1), -frame->linesize[0]);
            } else {
                ret = SDL_UpdateTexture(*tex, NULL, frame->data[0], frame->linesize[0]);
            }
            break;
    }
    return ret;
}

static void set_sdl_yuv_conversion_mode(AVFrame *frame)
{
#if SDL_VERSION_ATLEAST(2,0,8)
    SDL_YUV_CONVERSION_MODE mode = SDL_YUV_CONVERSION_AUTOMATIC;
    if (frame && (frame->format == AV_PIX_FMT_YUV420P || frame->format == AV_PIX_FMT_YUYV422 || frame->format == AV_PIX_FMT_UYVY422)) {
        if (frame->color_range == AVCOL_RANGE_JPEG)
            mode = SDL_YUV_CONVERSION_JPEG;
        else if (frame->colorspace == AVCOL_SPC_BT709)
            mode = SDL_YUV_CONVERSION_BT709;
        else if (frame->colorspace == AVCOL_SPC_BT470BG || frame->colorspace == AVCOL_SPC_SMPTE170M)
            mode = SDL_YUV_CONVERSION_BT601;
    }
    SDL_SetYUVConversionMode(mode);
#endif
}

static void draw_video_background(VideoRenderer *vr, VideoState *is)
{
    const int tile_size = VIDEO_BACKGROUND_TILE_SIZE;
    SDL_Rect *rect = &is->render_params.target_rect;
    SDL_BlendMode blendMode;

    if (!SDL_GetTextureBlendMode(is->vid_texture, &blendMode) && blendMode == SDL_BLENDMODE_BLEND) {
        switch (is->render_params.video_background_type) {
        case VIDEO_BACKGROUND_TILES:
            SDL_SetRenderDrawColor(vr->renderer, 237, 237, 237, 255);
            fill_rectangle(vr->renderer, rect->x, rect->y, rect->w, rect->h);
            SDL_SetRenderDrawColor(vr->renderer, 222, 222, 222, 255);
            for (int x = 0; x < rect->w; x += tile_size * 2)
                fill_rectangle(vr->renderer, rect->x + x, rect->y, FFMIN(tile_size, rect->w - x), rect->h);
            for (int y = 0; y < rect->h; y += tile_size * 2)
                fill_rectangle(vr->renderer, rect->x, rect->y + y, rect->w, FFMIN(tile_size, rect->h - y));
            SDL_SetRenderDrawColor(vr->renderer, 237, 237, 237, 255);
            for (int y = 0; y < rect->h; y += tile_size * 2) {
                int h = FFMIN(tile_size, rect->h - y);
                for (int x = 0; x < rect->w; x += tile_size * 2)
                    fill_rectangle(vr->renderer, x + rect->x, y + rect->y, FFMIN(tile_size, rect->w - x), h);
            }
            break;
        case VIDEO_BACKGROUND_COLOR: {
            const uint8_t *c = is->render_params.video_background_color;
            SDL_SetRenderDrawColor(vr->renderer, c[0], c[1], c[2], c[3]);
            fill_rectangle(vr->renderer, rect->x, rect->y, rect->w, rect->h);
            break;
        }
        case VIDEO_BACKGROUND_NONE:
            SDL_SetTextureBlendMode(is->vid_texture, SDL_BLENDMODE_NONE);
            break;
        }
    }
}

static inline int compute_mod(int a, int b)
{
    return a < 0 ? a%b + b : a%b;
}

static void video_image_display(VideoRenderer *vr, VideoState *is)
{
    Frame *vp;
    Frame *sp = NULL;
    SDL_Rect *rect = &is->render_params.target_rect;

    vp = frame_queue_peek_last(is->pictq);
    calculate_display_rect(rect, is->xleft, is->ytop, is->width, is->height, vp->width, vp->height, vp->sar);

    if (is->subtitle_st && frame_queue_nb_remaining(is->subpq) > 0) {
        sp = frame_queue_peek(is->subpq);
        if (vp->pts >= sp->pts + ((float) sp->sub.start_display_time / 1000)) {
            if (!sp->uploaded) {
                uint8_t* pixels[4];
                int pitch[4];
                int i;
                if (!sp->width || !sp->height) {
                    sp->width = vp->width;
                    sp->height = vp->height;
                }
                if (realloc_texture(vr->renderer, &is->sub_texture, SDL_PIXELFORMAT_ARGB8888, sp->width, sp->height, SDL_BLENDMODE_BLEND, 1) < 0)
                    return;

                for (i = 0; i < sp->sub.num_rects; i++) {
                    AVSubtitleRect *sub_rect = sp->sub.rects[i];

                    sub_rect->x = av_clip(sub_rect->x, 0, sp->width );
                    sub_rect->y = av_clip(sub_rect->y, 0, sp->height);
                    sub_rect->w = av_clip(sub_rect->w, 0, sp->width  - sub_rect->x);
                    sub_rect->h = av_clip(sub_rect->h, 0, sp->height - sub_rect->y);

                    is->sub_convert_ctx = sws_getCachedContext(is->sub_convert_ctx,
                        sub_rect->w, sub_rect->h, AV_PIX_FMT_PAL8,
                        sub_rect->w, sub_rect->h, AV_PIX_FMT_BGRA,
                        0, NULL, NULL, NULL);
                    if (!is->sub_convert_ctx) {
                        av_log(NULL, AV_LOG_FATAL, "Cannot initialize the conversion context\n");
                        return;
                    }
                    if (!SDL_LockTexture(is->sub_texture, (SDL_Rect *)sub_rect, (void **)pixels, pitch)) {
                        sws_scale(is->sub_convert_ctx, (const uint8_t * const *)sub_rect->data, sub_rect->linesize,
                                  0, sub_rect->h, pixels, pitch);
                        SDL_UnlockTexture(is->sub_texture);
                    }
                }
                sp->uploaded = 1;
            }
        } else {
            sp = NULL;
        }
    }

    set_sdl_yuv_conversion_mode(vp->frame);

    if (!vp->uploaded) {
        if (upload_texture(vr->renderer, &is->vid_texture, vp->frame) < 0) {
            set_sdl_yuv_conversion_mode(NULL);
            return;
        }
        vp->uploaded = 1;
        vp->flip_v = vp->frame->linesize[0] < 0;
    }

    draw_video_background(vr, is);
    SDL_RenderCopyEx(vr->renderer, is->vid_texture, NULL, rect, 0, NULL,
                     vp->flip_v ? SDL_FLIP_VERTICAL : SDL_FLIP_NONE);
    set_sdl_yuv_conversion_mode(NULL);
    if (sp) {
#if USE_ONEPASS_SUBTITLE_RENDER
        SDL_RenderCopy(vr->renderer, is->sub_texture, NULL, rect);
#else
        int i;
        double xratio = (double)rect->w / (double)sp->width;
        double yratio = (double)rect->h / (double)sp->height;
        for (i = 0; i < sp->sub.num_rects; i++) {
            SDL_Rect *sub_rect = (SDL_Rect*)sp->sub.rects[i];
            SDL_Rect target;
            target.x = (int)(rect->x + sub_rect->x * xratio);
            target.y = (int)(rect->y + sub_rect->y * yratio);
            target.w = (int)(sub_rect->w * xratio);
            target.h = (int)(sub_rect->h * yratio);
            SDL_RenderCopy(vr->renderer, is->sub_texture, sub_rect, &target);
        }
#endif
    }
}

static void video_audio_display(VideoRenderer *vr, VideoState *s)
{
    int i, i_start, x, y1, y, ys, delay, n, nb_display_channels;
    int ch, channels, h, h2;
    int64_t time_diff;
    int rdft_bits, nb_freq;

    for (rdft_bits = 1; (1 << rdft_bits) < 2 * s->height; rdft_bits++)
        ;
    nb_freq = 1 << (rdft_bits - 1);
    channels = s->audio_tgt.ch_layout.nb_channels;
    nb_display_channels = channels;
    if (!s->paused) {
        int data_used= s->show_mode == SHOW_MODE_WAVES ? s->width : (2*nb_freq);
        n = 2 * channels;
        delay = s->audio_write_buf_size;
        delay /= n;
        if (s->audio_callback_time) {
            time_diff = av_gettime_relative() - s->audio_callback_time;
            delay -= (int)((time_diff * s->audio_tgt.freq) / 1000000);
        }
        delay += 2 * data_used;
        if (delay < data_used)
            delay = data_used;

        i_start= x = compute_mod(s->sample_array_index - delay * channels, SAMPLE_ARRAY_SIZE);
        if (s->show_mode == SHOW_MODE_WAVES) {
            h = INT_MIN;
            for (i = 0; i < 1000; i += channels) {
                int idx = (SAMPLE_ARRAY_SIZE + x - i) % SAMPLE_ARRAY_SIZE;
                int a = s->sample_array[idx];
                int b = s->sample_array[(idx + 4 * channels) % SAMPLE_ARRAY_SIZE];
                int c = s->sample_array[(idx + 5 * channels) % SAMPLE_ARRAY_SIZE];
                int d = s->sample_array[(idx + 9 * channels) % SAMPLE_ARRAY_SIZE];
                int score = a - d;
                if (h < score && (b ^ c) < 0) {
                    h = score;
                    i_start = idx;
                }
            }
        }
        s->last_i_start = i_start;
    } else {
        i_start = s->last_i_start;
    }

    if (s->show_mode == SHOW_MODE_WAVES) {
        SDL_SetRenderDrawColor(vr->renderer, 255, 255, 255, 255);
        h = s->height / nb_display_channels;
        h2 = (h * 9) / 20;
        for (ch = 0; ch < nb_display_channels; ch++) {
            i = i_start + ch;
            y1 = s->ytop + ch * h + (h / 2);
            for (x = 0; x < s->width; x++) {
                y = (s->sample_array[i] * h2) >> 15;
                if (y < 0) {
                    y = -y;
                    ys = y1 - y;
                } else {
                    ys = y1;
                }
                fill_rectangle(vr->renderer, s->xleft + x, ys, 1, y);
                i += channels;
                if (i >= SAMPLE_ARRAY_SIZE)
                    i -= SAMPLE_ARRAY_SIZE;
            }
        }

        SDL_SetRenderDrawColor(vr->renderer, 0, 0, 255, 255);
        for (ch = 1; ch < nb_display_channels; ch++) {
            y = s->ytop + ch * h;
            fill_rectangle(vr->renderer, s->xleft, y, s->width, 1);
        }
    } else {
        int err = 0;
        if (realloc_texture(vr->renderer, &s->vis_texture, SDL_PIXELFORMAT_ARGB8888, s->width, s->height, SDL_BLENDMODE_NONE, 1) < 0)
            return;

        if (s->xpos >= s->width)
            s->xpos = 0;
        nb_display_channels= FFMIN(nb_display_channels, 2);
        if (rdft_bits != s->rdft_bits) {
            const float rdft_scale = 1.0;
            av_tx_uninit(&s->rdft);
            av_freep(&s->real_data);
            av_freep(&s->rdft_data);
            s->rdft_bits = rdft_bits;
            s->real_data = av_malloc_array(nb_freq, 4 * sizeof(*s->real_data));
            s->rdft_data = av_malloc_array(nb_freq + 1, 2 * sizeof(*s->rdft_data));
            err = av_tx_init(&s->rdft, &s->rdft_fn, AV_TX_FLOAT_RDFT,
                             0, 1 << rdft_bits, &rdft_scale, 0);
        }
        if (err < 0 || !s->rdft_data) {
            av_log(NULL, AV_LOG_ERROR, "Failed to allocate buffers for RDFT, switching to waves display\n");
            s->show_mode = SHOW_MODE_WAVES;
        } else {
            float *data_in[2];
            AVComplexFloat *data[2];
            SDL_Rect rect;
            uint32_t *pixels;
            int pitch;
            rect.x = s->xpos;
            rect.y = 0;
            rect.w = 1;
            rect.h = s->height;
            for (ch = 0; ch < nb_display_channels; ch++) {
                data_in[ch] = s->real_data + 2 * nb_freq * ch;
                data[ch] = s->rdft_data + nb_freq * ch;
                i = i_start + ch;
                for (x = 0; x < 2 * nb_freq; x++) {
                    double w = (x-nb_freq) * (1.0 / nb_freq);
                    data_in[ch][x] = s->sample_array[i] * (float)(1.0 - w * w);
                    i += channels;
                    if (i >= SAMPLE_ARRAY_SIZE)
                        i -= SAMPLE_ARRAY_SIZE;
                }
                s->rdft_fn(s->rdft, data[ch], data_in[ch], sizeof(float));
                data[ch][0].im = data[ch][nb_freq].re;
                data[ch][nb_freq].re = 0;
            }
            if (!SDL_LockTexture(s->vis_texture, &rect, (void **)&pixels, &pitch)) {
                pitch >>= 2;
                pixels += pitch * s->height;
                for (y = 0; y < s->height; y++) {
                    double w = 1 / sqrt(nb_freq);
                    int a = (int)sqrt(w * sqrt(data[0][y].re * data[0][y].re + data[0][y].im * data[0][y].im));
                    int b = (nb_display_channels == 2 ) ? (int)sqrt(w * hypot(data[1][y].re, data[1][y].im))
                                                        : a;
                    a = FFMIN(a, 255);
                    b = FFMIN(b, 255);
                    pixels -= pitch;
                    *pixels = (a << 16) + (b << 8) + ((a+b) >> 1);
                }
                SDL_UnlockTexture(s->vis_texture);
            }
            SDL_RenderCopy(vr->renderer, s->vis_texture, NULL, NULL);
        }
        if (!s->paused)
            s->xpos++;
    }
}

void video_renderer_set_default_window_size(VideoRenderer *vr, VideoState *is, int width, int height, AVRational sar)
{
    SDL_Rect rect;
    int max_width  = (is && is->width > 0)  ? is->width  : INT_MAX;
    int max_height = (is && is->height > 0) ? is->height : INT_MAX;
    if (max_width == INT_MAX && max_height == INT_MAX)
        max_height = height;
    calculate_display_rect(&rect, 0, 0, max_width, max_height, width, height, sar);
    vr->default_width  = rect.w;
    vr->default_height = rect.h;
}

const SDL_RendererInfo *video_renderer_get_info(const VideoRenderer *vr)
{
    if (!vr)
        return NULL;
    return &vr->renderer_info;
}

int video_renderer_open(VideoRenderer *vr, VideoState *is)
{
    int w,h;
    const char *title;

    w = is->width > 0 ? is->width : vr->default_width;
    h = is->height > 0 ? is->height : vr->default_height;

    title = vr->title_provider ? vr->title_provider(is, vr->program_name) : vr->program_name;
    SDL_SetWindowTitle(vr->window, title && title[0] ? title : vr->program_name);
    SDL_SetWindowSize(vr->window, w, h);
    SDL_SetWindowPosition(vr->window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    if (*vr->is_full_screen)
        SDL_SetWindowFullscreen(vr->window, SDL_WINDOW_FULLSCREEN_DESKTOP);
    SDL_ShowWindow(vr->window);

    is->width  = w;
    is->height = h;
    return 0;
}

void video_renderer_display(VideoRenderer *vr, VideoState *is)
{
    if (!is->width)
        video_renderer_open(vr, is);

    SDL_SetRenderDrawColor(vr->renderer, 0, 0, 0, 255);
    SDL_RenderClear(vr->renderer);
    if (is->audio_st && is->show_mode != SHOW_MODE_VIDEO)
        video_audio_display(vr, is);
    else if (is->video_st)
        video_image_display(vr, is);
}

void video_renderer_present(VideoRenderer *vr)
{
    if (!vr || !vr->renderer)
        return;
    SDL_RenderPresent(vr->renderer);
}

void video_renderer_refresh(VideoRenderer *vr, VideoState *is, double *remaining_time)
{
    double time;
    Frame *sp, *sp2;

    if (!is->paused && av_sync_is_external_clock_master(&is->av_sync) &&
        demuxer_is_realtime(&is->demuxer))
        check_external_clock_speed(&is->av_sync);

    if (is->show_mode != SHOW_MODE_VIDEO && is->audio_st) {
        time = av_gettime_relative() / 1000000.0;
        if (is->force_refresh || is->last_vis_time + vr->rdftspeed < time) {
            video_renderer_display(vr, is);
            is->last_vis_time = time;
        }
        *remaining_time = FFMIN(*remaining_time, is->last_vis_time + vr->rdftspeed - time);
    }

    if (is->video_st) {
retry:
        if (frame_queue_nb_remaining(is->pictq) == 0) {
        } else {
            double duration, delay;
            Frame *vp, *lastvp;

            lastvp = frame_queue_peek_last(is->pictq);
            vp = frame_queue_peek(is->pictq);

            if (vp->serial != packet_queue_get_serial(is->videoq)) {
                frame_queue_next(is->pictq);
                goto retry;
            }

            if (lastvp->serial != vp->serial)
                is->frame_timer = av_gettime_relative() / 1000000.0;

            if (is->paused)
                goto display;

            delay = av_sync_compute_frame_delay(&is->av_sync, lastvp, vp);

            time= av_gettime_relative()/1000000.0;
            if (time < is->frame_timer + delay) {
                *remaining_time = FFMIN(is->frame_timer + delay - time, *remaining_time);
                goto display;
            }

            is->frame_timer += delay;
            if (av_sync_should_reset_frame_timer(delay, time, is->frame_timer))
                is->frame_timer = time;

            frame_queue_lock(is->pictq);
            av_sync_update_video_pts_if_valid(&is->av_sync, vp->pts, vp->serial);
            frame_queue_unlock(is->pictq);

            if (frame_queue_nb_remaining(is->pictq) > 1) {
                Frame *nextvp = frame_queue_peek_next(is->pictq);
                duration = vp_duration(&is->av_sync, vp, nextvp);
                if (av_sync_should_late_drop(&is->av_sync, is->step, time, is->frame_timer, duration)) {
                    is->frame_drops_late++;
                    frame_queue_next(is->pictq);
                    goto retry;
                }
            }

            if (is->subtitle_st) {
                while (frame_queue_nb_remaining(is->subpq) > 0) {
                    sp = frame_queue_peek(is->subpq);
                    if (frame_queue_nb_remaining(is->subpq) > 1)
                        sp2 = frame_queue_peek_next(is->subpq);
                    else
                        sp2 = NULL;

                    if (sp->serial != packet_queue_get_serial(is->subtitleq)
                            || (clock_get_pts(is->vidclk) > (sp->pts + ((float) sp->sub.end_display_time / 1000)))
                            || (sp2 && clock_get_pts(is->vidclk) > (sp2->pts + ((float) sp2->sub.start_display_time / 1000)))) {
                        if (sp->uploaded) {
                            int i;
                            for (i = 0; i < sp->sub.num_rects; i++) {
                                AVSubtitleRect *sub_rect = sp->sub.rects[i];
                                uint8_t *pixels;
                                int pitch, j;
                                if (!SDL_LockTexture(is->sub_texture, (SDL_Rect *)sub_rect, (void **)&pixels, &pitch)) {
                                    for (j = 0; j < sub_rect->h; j++, pixels += pitch)
                                        memset(pixels, 0, sub_rect->w << 2);
                                    SDL_UnlockTexture(is->sub_texture);
                                }
                            }
                        }
                        frame_queue_next(is->subpq);
                    } else {
                        break;
                    }
                }
            }

            frame_queue_next(is->pictq);
            is->force_refresh = 1;

            if (is->step && !is->paused)
                stream_toggle_pause(is);
        }
display:
        if (is->force_refresh && is->show_mode == SHOW_MODE_VIDEO && frame_queue_is_last_shown(is->pictq))
            video_renderer_display(vr, is);
    }
    is->force_refresh = 0;
}

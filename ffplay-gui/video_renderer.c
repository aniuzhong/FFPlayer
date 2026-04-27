#include <limits.h>
#include <math.h>
#include <string.h>

#include <libavutil/common.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixdesc.h>

#include "video_renderer.h"

#define USE_ONEPASS_SUBTITLE_RENDER 1

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

static void draw_video_background(VideoRenderer *vr)
{
    const int tile_size = VIDEO_BACKGROUND_TILE_SIZE;
    SDL_Rect *rect = &vr->render_params.target_rect;
    SDL_BlendMode blendMode;

    if (!SDL_GetTextureBlendMode(vr->vid_texture, &blendMode) && blendMode == SDL_BLENDMODE_BLEND) {
        switch (vr->render_params.video_background_type) {
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
            const uint8_t *c = vr->render_params.video_background_color;
            SDL_SetRenderDrawColor(vr->renderer, c[0], c[1], c[2], c[3]);
            fill_rectangle(vr->renderer, rect->x, rect->y, rect->w, rect->h);
            break;
        }
        case VIDEO_BACKGROUND_NONE:
            SDL_SetTextureBlendMode(vr->vid_texture, SDL_BLENDMODE_NONE);
            break;
        }
    }
}

static void video_image_display(VideoRenderer *vr, AVFrame *frame, AVSubtitle *subtitle,
                                int xleft, int ytop, int width, int height)
{
    SDL_Rect *rect = &vr->render_params.target_rect;

    calculate_display_rect(rect, xleft, ytop, width, height,
                           frame->width, frame->height, frame->sample_aspect_ratio);

    if (subtitle && subtitle->num_rects > 0) {
        int sub_w = frame->width;
        int sub_h = frame->height;
        int i;
        if (realloc_texture(vr->renderer, &vr->sub_texture, SDL_PIXELFORMAT_ARGB8888,
                            sub_w, sub_h, SDL_BLENDMODE_BLEND, 1) < 0)
            return;

        for (i = 0; i < subtitle->num_rects; i++) {
            AVSubtitleRect *sub_rect = subtitle->rects[i];
            uint8_t *pixels[4];
            int pitch[4];

            sub_rect->x = av_clip(sub_rect->x, 0, sub_w);
            sub_rect->y = av_clip(sub_rect->y, 0, sub_h);
            sub_rect->w = av_clip(sub_rect->w, 0, sub_w - sub_rect->x);
            sub_rect->h = av_clip(sub_rect->h, 0, sub_h - sub_rect->y);

            vr->sub_convert_ctx = sws_getCachedContext(vr->sub_convert_ctx,
                sub_rect->w, sub_rect->h, AV_PIX_FMT_PAL8,
                sub_rect->w, sub_rect->h, AV_PIX_FMT_BGRA,
                0, NULL, NULL, NULL);
            if (!vr->sub_convert_ctx) {
                av_log(NULL, AV_LOG_FATAL, "Cannot initialize the conversion context\n");
                return;
            }
            if (!SDL_LockTexture(vr->sub_texture, (SDL_Rect *)sub_rect, (void **)pixels, pitch)) {
                sws_scale(vr->sub_convert_ctx, (const uint8_t * const *)sub_rect->data, sub_rect->linesize,
                          0, sub_rect->h, pixels, pitch);
                SDL_UnlockTexture(vr->sub_texture);
            }
        }
    }

    set_sdl_yuv_conversion_mode(frame);

    if (vr->last_vid_data != frame->data[0]) {
        if (upload_texture(vr->renderer, &vr->vid_texture, frame) < 0) {
            set_sdl_yuv_conversion_mode(NULL);
            return;
        }
        vr->last_vid_data = frame->data[0];
        vr->last_flip_v = frame->linesize[0] < 0;
    }

    draw_video_background(vr);
    SDL_RenderCopyEx(vr->renderer, vr->vid_texture, NULL, rect, 0, NULL,
                     vr->last_flip_v ? SDL_FLIP_VERTICAL : SDL_FLIP_NONE);
    set_sdl_yuv_conversion_mode(NULL);
    if (subtitle && subtitle->num_rects > 0) {
#if USE_ONEPASS_SUBTITLE_RENDER
        SDL_RenderCopy(vr->renderer, vr->sub_texture, NULL, rect);
#else
        int i;
        double xratio = (double)rect->w / (double)frame->width;
        double yratio = (double)rect->h / (double)frame->height;
        for (i = 0; i < subtitle->num_rects; i++) {
            SDL_Rect *sub_rect = (SDL_Rect*)subtitle->rects[i];
            SDL_Rect target;
            target.x = (int)(rect->x + sub_rect->x * xratio);
            target.y = (int)(rect->y + sub_rect->y * yratio);
            target.w = (int)(sub_rect->w * xratio);
            target.h = (int)(sub_rect->h * yratio);
            SDL_RenderCopy(vr->renderer, vr->sub_texture, sub_rect, &target);
        }
#endif
    }
}

void video_renderer_set_default_window_size(VideoRenderer *vr, int screen_width, int screen_height, int width, int height, AVRational sar)
{
    SDL_Rect rect;
    int max_width  = screen_width > 0  ? screen_width  : INT_MAX;
    int max_height = screen_height > 0 ? screen_height : INT_MAX;
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

int video_renderer_get_supported_pixel_formats(const VideoRenderer *vr,
                                               enum AVPixelFormat *out_fmts,
                                               int max_fmts)
{
    int nb = 0;
    int i, j;
    if (!vr || !out_fmts || max_fmts <= 0)
        return 0;
    for (i = 0; i < (int)vr->renderer_info.num_texture_formats && nb < max_fmts; i++) {
        for (j = 0; j < FF_ARRAY_ELEMS(sdl_texture_format_map); j++) {
            if (vr->renderer_info.texture_formats[i] == sdl_texture_format_map[j].texture_fmt) {
                out_fmts[nb++] = sdl_texture_format_map[j].format;
                break;
            }
        }
    }
    return nb;
}

int video_renderer_open(VideoRenderer *vr, int *width, int *height)
{
    int w, h;

    w = (*width > 0) ? *width : vr->default_width;
    h = (*height > 0) ? *height : vr->default_height;

    SDL_SetWindowSize(vr->window, w, h);

    *width  = w;
    *height = h;
    return 0;
}

void video_renderer_draw_video(VideoRenderer *vr, AVFrame *frame, AVSubtitle *subtitle, int xleft, int ytop, int width, int height)
{
    if (!frame)
        return;
    video_image_display(vr, frame, subtitle, xleft, ytop, width, height);
}

void video_renderer_clear(VideoRenderer *vr)
{
    SDL_SetRenderDrawColor(vr->renderer, 0, 0, 0, 255);
    SDL_RenderClear(vr->renderer);
}

void video_renderer_present(VideoRenderer *vr)
{
    if (!vr || !vr->renderer)
        return;
    SDL_RenderPresent(vr->renderer);
}

void video_renderer_cleanup_textures(VideoRenderer *vr)
{
    if (!vr)
        return;
    sws_freeContext(vr->sub_convert_ctx);
    vr->sub_convert_ctx = NULL;
    if (vr->vid_texture)
        SDL_DestroyTexture(vr->vid_texture);
    vr->vid_texture = NULL;
    if (vr->sub_texture)
        SDL_DestroyTexture(vr->sub_texture);
    vr->sub_texture = NULL;
    vr->last_vid_data = NULL;
    vr->last_flip_v = 0;
}

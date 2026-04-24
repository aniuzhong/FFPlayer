#ifndef FFPLAY_GUI_VIDEO_RENDERER_H
#define FFPLAY_GUI_VIDEO_RENDERER_H

#include <SDL.h>
#include <libavutil/frame.h>
#include <libavutil/rational.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VIDEO_BACKGROUND_TILE_SIZE 64

enum VideoBackgroundType {
    VIDEO_BACKGROUND_TILES,
    VIDEO_BACKGROUND_COLOR,
    VIDEO_BACKGROUND_NONE,
};

typedef struct RenderParams {
    SDL_Rect target_rect;
    uint8_t video_background_color[4];
    int video_background_type;
} RenderParams;

typedef struct VideoRenderer {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_RendererInfo renderer_info;
    int default_width;
    int default_height;
    SDL_Texture *vid_texture;
    SDL_Texture *sub_texture;
    struct SwsContext *sub_convert_ctx;
    RenderParams render_params;
    const uint8_t *last_vid_data;
    int last_flip_v;
} VideoRenderer;

void video_renderer_set_default_window_size(VideoRenderer *vr, int screen_width, int screen_height, int width, int height, AVRational sar);
const SDL_RendererInfo *video_renderer_get_info(const VideoRenderer *vr);
int video_renderer_open(VideoRenderer *vr, int *width, int *height);
void video_renderer_draw_video(VideoRenderer *vr, AVFrame *frame, AVSubtitle *subtitle, int xleft, int ytop, int width, int height);
void video_renderer_clear(VideoRenderer *vr);
void video_renderer_present(VideoRenderer *vr);
void video_renderer_cleanup_textures(VideoRenderer *vr);

#ifdef __cplusplus
}
#endif

#endif

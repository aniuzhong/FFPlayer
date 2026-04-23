#ifndef FFPLAY_GUI_VIDEO_RENDERER_H
#define FFPLAY_GUI_VIDEO_RENDERER_H

#include <SDL.h>
#include <libavutil/rational.h>
#include <libswscale/swscale.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VideoState VideoState;

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
    
    // Rendering textures (moved from VideoState)
    SDL_Texture *vid_texture;
    SDL_Texture *sub_texture;
    struct SwsContext *sub_convert_ctx;
    RenderParams render_params;
} VideoRenderer;

void video_renderer_set_default_window_size(VideoRenderer *vr, VideoState *is, int width, int height, AVRational sar);
const SDL_RendererInfo *video_renderer_get_info(const VideoRenderer *vr);
int video_renderer_open(VideoRenderer *vr, VideoState *is);
void video_renderer_display(VideoRenderer *vr, VideoState *is);
void video_renderer_flush_sub_rect(VideoRenderer *vr, const SDL_Rect *rect);
void video_renderer_present(VideoRenderer *vr);

#ifdef __cplusplus
}
#endif

#endif

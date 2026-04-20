#ifndef FFPLAY_GUI_VIDEO_RENDERER_H
#define FFPLAY_GUI_VIDEO_RENDERER_H

#include "video_state.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VideoRenderer {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_RendererInfo renderer_info;
    int default_width;
    int default_height;
    int *is_full_screen;
    double rdftspeed;
    const char *program_name;
    const char *(*title_provider)(VideoState *is, const char *fallback);
} VideoRenderer;

void video_renderer_set_default_window_size(VideoRenderer *vr, VideoState *is, int width, int height, AVRational sar);
const SDL_RendererInfo *video_renderer_get_info(const VideoRenderer *vr);
int video_renderer_open(VideoRenderer *vr, VideoState *is);
void video_renderer_display(VideoRenderer *vr, VideoState *is);
void video_renderer_refresh(VideoRenderer *vr, VideoState *is, double *remaining_time);
void video_renderer_present(VideoRenderer *vr);

#ifdef __cplusplus
}
#endif

#endif

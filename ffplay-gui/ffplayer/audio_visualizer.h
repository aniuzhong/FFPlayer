#ifndef FFPLAY_GUI_AUDIO_VISUALIZER_H
#define FFPLAY_GUI_AUDIO_VISUALIZER_H

#include <SDL.h>

#include <libavutil/tx.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations for borrowed references */
typedef struct AudioPipeline AudioPipeline;

typedef struct AudioVisualizer {
    /* RDFT state (moved from VideoState) */
    int last_i_start;
    AVTXContext *rdft;
    av_tx_fn rdft_fn;
    int rdft_bits;
    float *real_data;
    AVComplexFloat *rdft_data;
    int xpos;
    double last_vis_time;

    /* Rendering resources (moved from VideoRenderer) */
    SDL_Texture *vis_texture;
    double rdftspeed;

    /* Borrowed references (not owned, set via audio_visualizer_bind) */
    AudioPipeline *audio_pipeline;
    int *paused;
    int *show_mode;
} AudioVisualizer;

AudioVisualizer *audio_visualizer_create(void);
void audio_visualizer_free(AudioVisualizer **av);

void audio_visualizer_bind(AudioVisualizer *av, AudioPipeline *ap,
                           int *paused, int *show_mode);

/* Render audio visualization (waves or RDFT spectrum) */
void audio_visualizer_render(AudioVisualizer *av, SDL_Renderer *renderer,
                             int xleft, int ytop, int width, int height);

/* Release RDFT context and buffers (called on audio stream close) */
void audio_visualizer_reset(AudioVisualizer *av);

/* Destroy and recreate vis_texture on next render (called on window resize) */
void audio_visualizer_invalidate_texture(AudioVisualizer *av);

#ifdef __cplusplus
}
#endif

#endif

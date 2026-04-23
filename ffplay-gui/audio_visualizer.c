#include <limits.h>
#include <math.h>
#include <string.h>

#include <libavutil/common.h>
#include <libavutil/log.h>
#include <libavutil/mem.h>
#include <libavutil/time.h>

#include "audio_visualizer.h"
#include "audio_pipeline.h"

#define DEFAULT_RDFTSPEED 0.02

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

static inline int compute_mod(int a, int b)
{
    return a < 0 ? a%b + b : a%b;
}

AudioVisualizer *audio_visualizer_create(void)
{
    AudioVisualizer *av = av_mallocz(sizeof(AudioVisualizer));
    if (av)
        av->rdftspeed = DEFAULT_RDFTSPEED;
    return av;
}

void audio_visualizer_free(AudioVisualizer **pav)
{
    AudioVisualizer *av;
    if (!pav || !*pav)
        return;
    av = *pav;
    if (av->vis_texture)
        SDL_DestroyTexture(av->vis_texture);
    av_tx_uninit(&av->rdft);
    av_freep(&av->real_data);
    av_freep(&av->rdft_data);
    av_free(av);
    *pav = NULL;
}

void audio_visualizer_bind(AudioVisualizer *av, AudioPipeline *ap,
                           int *paused, int *show_mode)
{
    if (!av)
        return;
    av->audio_pipeline = ap;
    av->paused = paused;
    av->show_mode = show_mode;
}

void audio_visualizer_render(AudioVisualizer *av, SDL_Renderer *renderer,
                             int xleft, int ytop, int width, int height)
{
    AudioPipeline *ap;
    int i, i_start, x, y1, y, ys, delay, n, nb_display_channels;
    int ch, channels, h, h2;
    int64_t time_diff;
    int rdft_bits, nb_freq;

    if (!av || !av->audio_pipeline || !renderer)
        return;
    ap = av->audio_pipeline;

    for (rdft_bits = 1; (1 << rdft_bits) < 2 * height; rdft_bits++)
        ;
    nb_freq = 1 << (rdft_bits - 1);
    channels = ap->audio_tgt.ch_layout.nb_channels;
    nb_display_channels = channels;
    if (!*av->paused) {
        int data_used= *av->show_mode == SHOW_MODE_WAVES ? width : (2*nb_freq);
        n = 2 * channels;
        delay = ap->audio_write_buf_size;
        delay /= n;
        if (ap->audio_callback_time) {
            time_diff = av_gettime_relative() - ap->audio_callback_time;
            delay -= (int)((time_diff * ap->audio_tgt.freq) / 1000000);
        }
        delay += 2 * data_used;
        if (delay < data_used)
            delay = data_used;

        i_start= x = compute_mod(ap->sample_array_index - delay * channels, SAMPLE_ARRAY_SIZE);
        if (*av->show_mode == SHOW_MODE_WAVES) {
            h = INT_MIN;
            for (i = 0; i < 1000; i += channels) {
                int idx = (SAMPLE_ARRAY_SIZE + x - i) % SAMPLE_ARRAY_SIZE;
                int a = ap->sample_array[idx];
                int b = ap->sample_array[(idx + 4 * channels) % SAMPLE_ARRAY_SIZE];
                int c = ap->sample_array[(idx + 5 * channels) % SAMPLE_ARRAY_SIZE];
                int d = ap->sample_array[(idx + 9 * channels) % SAMPLE_ARRAY_SIZE];
                int score = a - d;
                if (h < score && (b ^ c) < 0) {
                    h = score;
                    i_start = idx;
                }
            }
        }
        av->last_i_start = i_start;
    } else {
        i_start = av->last_i_start;
    }

    if (*av->show_mode == SHOW_MODE_WAVES) {
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        h = height / nb_display_channels;
        h2 = (h * 9) / 20;
        for (ch = 0; ch < nb_display_channels; ch++) {
            i = i_start + ch;
            y1 = ytop + ch * h + (h / 2);
            for (x = 0; x < width; x++) {
                y = (ap->sample_array[i] * h2) >> 15;
                if (y < 0) {
                    y = -y;
                    ys = y1 - y;
                } else {
                    ys = y1;
                }
                fill_rectangle(renderer, xleft + x, ys, 1, y);
                i += channels;
                if (i >= SAMPLE_ARRAY_SIZE)
                    i -= SAMPLE_ARRAY_SIZE;
            }
        }

        SDL_SetRenderDrawColor(renderer, 0, 0, 255, 255);
        for (ch = 1; ch < nb_display_channels; ch++) {
            y = ytop + ch * h;
            fill_rectangle(renderer, xleft, y, width, 1);
        }
    } else {
        int err = 0;
        if (realloc_texture(renderer, &av->vis_texture, SDL_PIXELFORMAT_ARGB8888, width, height, SDL_BLENDMODE_NONE, 1) < 0)
            return;

        if (av->xpos >= width)
            av->xpos = 0;
        nb_display_channels= FFMIN(nb_display_channels, 2);
        if (rdft_bits != av->rdft_bits) {
            const float rdft_scale = 1.0;
            av_tx_uninit(&av->rdft);
            av_freep(&av->real_data);
            av_freep(&av->rdft_data);
            av->rdft_bits = rdft_bits;
            av->real_data = av_malloc_array(nb_freq, 4 * sizeof(*av->real_data));
            av->rdft_data = av_malloc_array(nb_freq + 1, 2 * sizeof(*av->rdft_data));
            err = av_tx_init(&av->rdft, &av->rdft_fn, AV_TX_FLOAT_RDFT,
                             0, 1 << rdft_bits, &rdft_scale, 0);
        }
        if (err < 0 || !av->rdft_data) {
            av_log(NULL, AV_LOG_ERROR, "Failed to allocate buffers for RDFT, switching to waves display\n");
            *av->show_mode = SHOW_MODE_WAVES;
        } else {
            float *data_in[2];
            AVComplexFloat *data[2];
            SDL_Rect rect;
            uint32_t *pixels;
            int pitch;
            rect.x = av->xpos;
            rect.y = 0;
            rect.w = 1;
            rect.h = height;
            for (ch = 0; ch < nb_display_channels; ch++) {
                data_in[ch] = av->real_data + 2 * nb_freq * ch;
                data[ch] = av->rdft_data + nb_freq * ch;
                i = i_start + ch;
                for (x = 0; x < 2 * nb_freq; x++) {
                    double w = (x-nb_freq) * (1.0 / nb_freq);
                    data_in[ch][x] = ap->sample_array[i] * (float)(1.0 - w * w);
                    i += channels;
                    if (i >= SAMPLE_ARRAY_SIZE)
                        i -= SAMPLE_ARRAY_SIZE;
                }
                av->rdft_fn(av->rdft, data[ch], data_in[ch], sizeof(float));
                data[ch][0].im = data[ch][nb_freq].re;
                data[ch][nb_freq].re = 0;
            }
            if (!SDL_LockTexture(av->vis_texture, &rect, (void **)&pixels, &pitch)) {
                pitch >>= 2;
                pixels += pitch * height;
                for (y = 0; y < height; y++) {
                    double w = 1 / sqrt(nb_freq);
                    int a = (int)sqrt(w * sqrt(data[0][y].re * data[0][y].re + data[0][y].im * data[0][y].im));
                    int b = (nb_display_channels == 2 ) ? (int)sqrt(w * hypot(data[1][y].re, data[1][y].im))
                                                        : a;
                    a = FFMIN(a, 255);
                    b = FFMIN(b, 255);
                    pixels -= pitch;
                    *pixels = (a << 16) + (b << 8) + ((a+b) >> 1);
                }
                SDL_UnlockTexture(av->vis_texture);
            }
            SDL_RenderCopy(renderer, av->vis_texture, NULL, NULL);
        }
        if (!*av->paused)
            av->xpos++;
    }
}

void audio_visualizer_reset(AudioVisualizer *av)
{
    if (!av)
        return;
    if (av->rdft) {
        av_tx_uninit(&av->rdft);
        av_freep(&av->real_data);
        av_freep(&av->rdft_data);
        av->rdft = NULL;
        av->rdft_bits = 0;
    }
}

void audio_visualizer_invalidate_texture(AudioVisualizer *av)
{
    if (!av)
        return;
    if (av->vis_texture) {
        SDL_DestroyTexture(av->vis_texture);
        av->vis_texture = NULL;
    }
}

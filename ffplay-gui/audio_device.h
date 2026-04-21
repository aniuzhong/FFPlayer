#ifndef FFPLAY_GUI_AUDIO_DEVICE_H
#define FFPLAY_GUI_AUDIO_DEVICE_H

#include <SDL.h>

#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>

#define SDL_AUDIO_MIN_BUFFER_SIZE 512
#define SDL_AUDIO_MAX_CALLBACKS_PER_SEC 30

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AudioParams {
    int freq;
    AVChannelLayout ch_layout;
    enum AVSampleFormat fmt;
    int frame_size;
    int bytes_per_sec;
} AudioParams;

typedef struct AudioDevice {
    SDL_AudioDeviceID id;
    int (*open_cb)(void *opaque, AVChannelLayout *wanted_channel_layout, int wanted_sample_rate, struct AudioParams *audio_hw_params);
} AudioDevice;

void audio_device_reset(AudioDevice *device);
void audio_device_set_open_cb(AudioDevice *device,
                              int (*open_cb)(void *opaque, AVChannelLayout *wanted_channel_layout, int wanted_sample_rate, struct AudioParams *audio_hw_params));
int audio_device_open(AudioDevice *device,
                      void *opaque,
                      AVChannelLayout *wanted_channel_layout,
                      int wanted_sample_rate,
                      struct AudioParams *audio_hw_params);
int audio_device_open_sdl(AudioDevice *device,
                          void *opaque,
                          AVChannelLayout *wanted_channel_layout,
                          int wanted_sample_rate,
                          SDL_AudioCallback callback,
                          struct AudioParams *audio_hw_params);
void audio_device_pause(AudioDevice *device, int pause_on);
void audio_device_close(AudioDevice *device);

#ifdef __cplusplus
}
#endif

#endif

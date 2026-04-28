/**
 * @file ffplayer.h
 * @brief FFPlayer public API.
 *
 * Public interface for media lifecycle, playback control, seeking, audio,
 * track selection, rendering integration, and frame access.
 */

#ifndef FFPLAY_GUI_FFPLAYER_H
#define FFPLAY_GUI_FFPLAYER_H

#include <stdint.h>
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
#include <libavcodec/avcodec.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque audio device type.
 */
typedef struct AudioDevice AudioDevice;

/**
 * @brief Opaque audio visualizer type.
 */
typedef struct AudioVisualizer AudioVisualizer;

/**
 * @brief Opaque FFPlayer type.
 */
typedef struct FFPlayer FFPlayer;

/**
 * @brief FFPlayer display mode.
 */
enum FFPlayerShowMode {
    FFPLAYER_SHOW_MODE_NONE = -1, /**< No display mode selected. */
    FFPLAYER_SHOW_MODE_VIDEO = 0,  /**< Video display mode. */
    FFPLAYER_SHOW_MODE_WAVES,      /**< Audio waveform display mode. */
    FFPLAYER_SHOW_MODE_RDFT,       /**< Audio RDFT display mode. */
    FFPLAYER_SHOW_MODE_NB          /**< Number of display modes. */
};

/**
 * @brief Default volume step factor.
 */
#define FFPLAYER_VOLUME_STEP (0.75)

/**
 * @brief Default refresh interval in seconds.
 */
#define FFPLAYER_REFRESH_RATE 0.01

/**
 * @brief Delay before hiding the cursor, in microseconds.
 */
#define FFPLAYER_CURSOR_HIDE_DELAY 1000000

/**
 * @brief Create and initialize an FFPlayer instance.
 *
 * @param[in] audio_device Associated audio device.
 * @return A newly created FFPlayer instance, or NULL on failure.
 */
FFPlayer *ffplayer_create(AudioDevice *audio_device);

/**
 * @brief Destroy an FFPlayer instance.
 *
 * @param[in,out] p Pointer to the FFPlayer pointer.
 *
 * @note If @p p is non-NULL, then *@p p is set to NULL on return.
 */
void ffplayer_free(FFPlayer **p);

/**
 * @brief Open media from the specified URL.
 *
 * @param[in] p FFPlayer instance.
 * @param[in] url Media URL or path.
 * @return 0 on success, negative value on failure.
 */
int ffplayer_open(FFPlayer *p, const char *url);

/**
 * @brief Close the currently opened media.
 *
 * @param[in] p FFPlayer instance.
 */
void ffplayer_close(FFPlayer *p);

/**
 * @brief Check whether media is currently open.
 *
 * @param[in] p FFPlayer instance.
 * @return Non-zero if media is open, otherwise 0.
 */
int ffplayer_is_open(const FFPlayer *p);

/**
 * @brief Toggle pause state.
 *
 * @param[in] p FFPlayer instance.
 */
void ffplayer_toggle_pause(FFPlayer *p);

/**
 * @brief Check whether playback is paused.
 *
 * @param[in] p FFPlayer instance.
 * @return Non-zero if paused, otherwise 0.
 */
int ffplayer_is_paused(const FFPlayer *p);

/**
 * @brief Advance playback by one frame.
 *
 * @param[in] p FFPlayer instance.
 */
void ffplayer_step_frame(FFPlayer *p);

/**
 * @brief Seek relative to the current position.
 *
 * @param[in] p FFPlayer instance.
 * @param[in] incr_seconds Relative seek offset in seconds.
 */
void ffplayer_seek_relative(FFPlayer *p, double incr_seconds);

/**
 * @brief Seek to a position expressed as a ratio of total duration.
 *
 * @param[in] p FFPlayer instance.
 * @param[in] ratio Target ratio in the range [0, 1].
 */
void ffplayer_seek_to_ratio(FFPlayer *p, float ratio);

/**
 * @brief Seek by chapter offset.
 *
 * @param[in] p FFPlayer instance.
 * @param[in] incr Chapter offset.
 */
void ffplayer_seek_chapter(FFPlayer *p, int incr);

/**
 * @brief Set the volume level.
 *
 * @param[in] p FFPlayer instance.
 * @param[in] volume Target volume.
 */
void ffplayer_set_volume(FFPlayer *p, int volume);

/**
 * @brief Get the current volume level.
 *
 * @param[in] p FFPlayer instance.
 * @return Current volume level.
 */
int ffplayer_get_volume(const FFPlayer *p);

/**
 * @brief Adjust volume by a step value.
 *
 * @param[in] p FFPlayer instance.
 * @param[in] sign Step direction.
 * @param[in] step Step size.
 */
void ffplayer_adjust_volume_step(FFPlayer *p, int sign, double step);

/**
 * @brief Toggle mute state.
 *
 * @param[in] p FFPlayer instance.
 */
void ffplayer_toggle_mute(FFPlayer *p);

/**
 * @brief Cycle to the next audio track.
 *
 * @param[in] p FFPlayer instance.
 */
void ffplayer_cycle_audio_track(FFPlayer *p);

/**
 * @brief Cycle to the next video track.
 *
 * @param[in] p FFPlayer instance.
 */
void ffplayer_cycle_video_track(FFPlayer *p);

/**
 * @brief Cycle to the next subtitle track.
 *
 * @param[in] p FFPlayer instance.
 */
void ffplayer_cycle_subtitle_track(FFPlayer *p);

/**
 * @brief Cycle all track types.
 *
 * @param[in] p FFPlayer instance.
 */
void ffplayer_cycle_all_tracks(FFPlayer *p);

/**
 * @brief Toggle audio display mode.
 *
 * @param[in] p FFPlayer instance.
 */
void ffplayer_toggle_audio_display(FFPlayer *p);

/**
 * @brief Get the current playback position in seconds.
 *
 * @param[in] p FFPlayer instance.
 * @return Current playback position in seconds.
 */
double ffplayer_get_position(const FFPlayer *p);

/**
 * @brief Get the total duration in seconds.
 *
 * @param[in] p FFPlayer instance.
 * @return Total duration in seconds.
 */
double ffplayer_get_duration(const FFPlayer *p);

/**
 * @brief Check whether playback has reached end of file.
 *
 * @param[in] p FFPlayer instance.
 * @return Non-zero if EOF has been reached, otherwise 0.
 */
int ffplayer_is_eof(const FFPlayer *p);

/**
 * @brief Check whether a quit request has been issued.
 *
 * @param[in] p FFPlayer instance.
 * @return Non-zero if a quit request exists, otherwise 0.
 */
int ffplayer_has_quit_request(const FFPlayer *p);

/**
 * @brief Check whether the media contains chapters.
 *
 * @param[in] p FFPlayer instance.
 * @return Non-zero if chapters are available, otherwise 0.
 */
int ffplayer_has_chapters(const FFPlayer *p);

/**
 * @brief Get the media title.
 *
 * @param[in] p FFPlayer instance.
 * @return Media title string, or NULL if unavailable.
 */
const char *ffplayer_get_media_title(const FFPlayer *p);

/**
 * @brief Check whether seeking is supported.
 *
 * @param[in] p FFPlayer instance.
 * @return Non-zero if seeking is supported, otherwise 0.
 */
int ffplayer_can_seek(const FFPlayer *p);

/**
 * @brief Get byte-based progress.
 *
 * @param[in] p FFPlayer instance.
 * @return Byte progress in the range [0, 1].
 */
float ffplayer_get_byte_progress(const FFPlayer *p);

/**
 * @brief Check whether a refresh is needed.
 *
 * @param[in] p FFPlayer instance.
 * @return Non-zero if refresh is needed, otherwise 0.
 */
int ffplayer_needs_refresh(const FFPlayer *p);

/**
 * @brief Refresh internal state.
 *
 * @param[in] p FFPlayer instance.
 * @param[out] remaining_time Remaining time until the next refresh, in seconds.
 */
void ffplayer_refresh(FFPlayer *p, double *remaining_time);

/**
 * @brief Get the current video frame selected by the sync algorithm.
 *
 * @param[in] p FFPlayer instance.
 * @return Borrowed AVFrame pointer, or NULL if no frame is available.
 *
 * @warning The returned frame must not be freed by the caller.
 * @note The returned frame remains valid until the next call to
 * ffplayer_refresh().
 */
AVFrame *ffplayer_get_video_frame(const FFPlayer *p);

/**
 * @brief Get the current subtitle that should be overlaid.
 *
 * @param[in] p FFPlayer instance.
 * @return Borrowed AVSubtitle pointer, or NULL if no subtitle is available.
 *
 * @warning The returned subtitle must not be freed by the caller.
 * @note The returned subtitle remains valid until the next call to
 * ffplayer_refresh().
 */
AVSubtitle *ffplayer_get_subtitle(const FFPlayer *p);

/**
 * @brief Get the video display dimensions and sample aspect ratio.
 *
 * @param[in] p FFPlayer instance.
 * @param[out] width Video width in pixels.
 * @param[out] height Video height in pixels.
 * @param[out] sar Sample aspect ratio.
 * @return 0 on success, or -1 if no video is available.
 */
int ffplayer_get_video_size(const FFPlayer *p, int *width, int *height, AVRational *sar);

/**
 * @brief Get the current display mode.
 *
 * @param[in] p FFPlayer instance.
 * @return Current display mode.
 */
enum FFPlayerShowMode ffplayer_get_show_mode(const FFPlayer *p);

/**
 * @brief Get the audio visualizer handle.
 *
 * @param[in] p FFPlayer instance.
 * @return Audio visualizer handle, or NULL if unavailable.
 *
 * @note The caller provides its own renderer.
 */
AudioVisualizer *ffplayer_get_audio_visualizer(const FFPlayer *p);

/**
 * @brief Set the window size.
 *
 * @param[in] p FFPlayer instance.
 * @param[in] width Window width in pixels.
 * @param[in] height Window height in pixels.
 */
void ffplayer_set_window_size(FFPlayer *p, int width, int height);

/**
 * @brief Notify FFPlayer that the window size has changed.
 *
 * @param[in] p FFPlayer instance.
 * @param[in] width Window width in pixels.
 * @param[in] height Window height in pixels.
 */
void ffplayer_handle_window_size_changed(FFPlayer *p, int width, int height);

/**
 * @brief Request a refresh.
 *
 * @param[in] p FFPlayer instance.
 */
void ffplayer_request_refresh(FFPlayer *p);

/**
 * @brief Check whether video is open.
 *
 * @param[in] p FFPlayer instance.
 * @return Non-zero if video is open, otherwise 0.
 */
int ffplayer_is_video_open(const FFPlayer *p);

/**
 * @brief Set the list of pixel formats supported by the renderer.
 *
 * The filter chain will restrict output to these formats.
 * Must be called before ffplayer_open().
 *
 * @param[in] p FFPlayer instance.
 * @param[in] pix_fmts Array of supported pixel formats.
 * @param[in] nb_pix_fmts Number of entries in @p pix_fmts.
 */
void ffplayer_set_supported_pixel_formats(FFPlayer *p,
                                          const enum AVPixelFormat *pix_fmts,
                                          int nb_pix_fmts);

/**
 * @brief Provide a hardware device context for zero-copy decoding.
 *
 * When set, the player installs a get_format callback on the video
 * decoder that prefers AV_PIX_FMT_D3D11 (or any other format owned by
 * @p hw_device_ctx). Decoded frames then live on the GPU and are
 * forwarded directly to the renderer, bypassing the CPU-side filter
 * graph for the hardware path.
 *
 * The reference is duplicated (av_buffer_ref); the caller retains
 * ownership of @p hw_device_ctx and is free to release it after this
 * call returns.
 *
 * Pass NULL to clear a previously-installed device context. Must be
 * called before ffplayer_open().
 *
 * @param[in] p FFPlayer instance.
 * @param[in] hw_device_ctx Borrowed AVBufferRef to an AVHWDeviceContext,
 * or NULL to disable hardware acceleration.
 */
void ffplayer_set_hw_device_ctx(FFPlayer *p, AVBufferRef *hw_device_ctx);

/**
 * @brief Callback invoked from the video thread when the coded frame size changes.
 *
 * The application can use this to adjust the window and default size.
 *
 * @param[in] opaque User-supplied opaque pointer.
 * @param[in] width New coded frame width.
 * @param[in] height New coded frame height.
 * @param[in] sar Sample aspect ratio.
 */
typedef void (*ffplayer_frame_size_cb)(void *opaque, int width, int height, AVRational sar);

/**
 * @brief Set the frame size callback.
 *
 * @param[in] p FFPlayer instance.
 * @param[in] cb Callback function.
 * @param[in] opaque User-supplied opaque pointer passed to the callback.
 */
void ffplayer_set_frame_size_callback(FFPlayer *p, ffplayer_frame_size_cb cb, void *opaque);

#ifdef __cplusplus
}
#endif

#endif
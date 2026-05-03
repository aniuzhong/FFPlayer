#ifndef FFPLAY_GUI_APP_APPLICATION_H
#define FFPLAY_GUI_APP_APPLICATION_H

#include <cstdint>
#include <cstddef>

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif
#include "ffplayer/audio_device.h"
#include "ffplayer/ffplayer.h"
#ifdef __cplusplus
}
#endif

#include "render/video_renderer.h"

class Application {
public:
    int Execute();

    FFPlayer *player() noexcept { return player_; }
    const FFPlayer *player() const noexcept { return player_; }

    int &ui_startup_infinite_buffer() noexcept { return startup_infinite_buffer_; }
    int &ui_startup_av_sync_type() noexcept { return startup_av_sync_type_; }

    bool &ui_show_statistics() noexcept { return show_statistics_window_; }

    float ui_render_fps() const noexcept { return render_fps_; }
    float ui_render_frame_time_ms() const noexcept { return render_frame_time_ms_; }
    bool ui_stats_has_video_frame() const noexcept { return stats_has_video_frame_; }
    bool ui_stats_pipeline_zero_copy() const noexcept { return stats_pipeline_zero_copy_; }
    AVPixelFormat ui_stats_video_pix_fmt() const noexcept { return stats_video_pix_fmt_; }

    float &ui_pending_seek_ratio() noexcept { return pending_seek_ratio_; }
    int64_t &ui_last_drag_seek_us() noexcept { return last_drag_seek_us_; }
    float &ui_stable_progress_ratio() noexcept { return stable_progress_ratio_; }
    bool &ui_stable_progress_ready() noexcept { return stable_progress_ready_; }
    char *ui_open_url_buffer() noexcept { return open_url_input_; }
    static constexpr size_t ui_open_url_buffer_bytes() noexcept { return sizeof(open_url_input_); }

    void ui_seek_to_ratio(float ratio) { SeekToRatio(ratio); }
    bool ui_open_file_dialog_and_play() { return OpenFileDialogAndPlay(); }
    bool ui_open_url_and_play() { return OpenUrlAndPlay(); }

    /** Used by ApplicationWin32WndProc only. */
    void win32_on_client_resize(WPARAM size_type, int client_w, int client_h);
    void win32_request_quit();
    void ui_toggle_fullscreen();
    void ui_player_request_refresh();
    void ui_note_mouse_activity_show_cursor();
    [[noreturn]] void ui_quit_application();

private:
    void InitImGui();
    void ShutdownImGui();
    void RefreshWindowTitle();
    void SeekToRatio(float ratio);
    void StopPlaybackAndReset();
    void UpdateRenderFps();
    void UpdatePipelineStatsFromFrame(const AVFrame *frame);
    void RenderImGui();
    bool OpenMediaAndPlay(const char *source, const char *error_message);
    bool OpenFileDialogAndPlay();
    bool OpenUrlAndPlay();
    [[noreturn]] void DoExit();
    void ToggleFullScreen();
    void HandlePlaybackFatalError();
    bool InitWindow();
    void InitRenderer();
    void DisplayVideo();
    void MainLoop();

    static void OnFrameSizeChanged(void *opaque, int width, int height, AVRational sar);

    HWND hwnd_ = nullptr;
    AudioDevice audio_device_ = {};
    int64_t cursor_last_shown_ = 0;
    int cursor_hidden_ = 0;
    int is_full_screen_ = 0;
    WINDOWPLACEMENT wp_before_fullscreen_ = {};
    FFPlayer *player_ = nullptr;
    VideoRenderer video_renderer_ctx_ = {};
    int video_open_done_ = 0;
    bool open_dialog_active_ = false;
    bool imgui_ready_ = false;
    float pending_seek_ratio_ = -1.0f;
    int64_t last_drag_seek_us_ = 0;
    float stable_progress_ratio_ = 0.0f;
    bool stable_progress_ready_ = false;
    char open_url_input_[1024] = {};
    bool show_statistics_window_ = false;
    float render_fps_ = 0.0f;
    float render_frame_time_ms_ = 0.0f;
    int render_fps_frame_count_ = 0;
    int64_t render_fps_last_sample_us_ = 0;
    bool stats_has_video_frame_ = false;
    bool stats_pipeline_zero_copy_ = false;
    AVPixelFormat stats_video_pix_fmt_ = AV_PIX_FMT_NONE;
    bool quit_requested_ = false;
    int startup_infinite_buffer_ = -1;
    int startup_av_sync_type_ = FFPLAYER_AV_SYNC_AUDIO_MASTER;
};

void UiDrawStartupDefaults(Application &app, float main_menu_bar_bottom);
void UiDrawStatisticsWindow(Application &app);
bool UiDrawPlayerControls(Application &app);

#endif

#ifndef FFPLAY_GUI_APPLICATION_H
#define FFPLAY_GUI_APPLICATION_H

#include <cstdint>
#include <string>

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif
#include "ffplayer/audio_device.h"
#include "ffplayer/ffplayer.h"
#ifdef __cplusplus
}
#endif

#include "video_renderer.h"

class Application {
public:
    int Execute();

private:
    static std::string FormatDuration(double seconds);
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
    void HandleKeyDown(WPARAM wParam);

    static void OnFrameSizeChanged(void *opaque, int width, int height, AVRational sar);
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

private:
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
};

#endif

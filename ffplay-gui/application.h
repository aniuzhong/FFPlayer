#ifndef FFPLAY_GUI_APPLICATION_H
#define FFPLAY_GUI_APPLICATION_H

#include <stdint.h>
#include <string>

#ifdef __cplusplus
extern "C" {
#endif
#include "audio_device.h"
#include "video_renderer.h"
#ifdef __cplusplus
}
#endif

class Application {
public:
    int Run();

private:
    SDL_Window *window_ = nullptr;
    SDL_Renderer *renderer_ = nullptr;
    AudioDevice audio_device_ = {};
    int64_t cursor_last_shown_ = 0;
    int cursor_hidden_ = 0;
    int is_full_screen_ = 0;
    VideoState *stream_ = nullptr;
    VideoRenderer video_renderer_ctx_ = {};
    bool open_dialog_active_ = false;
    bool imgui_ready_ = false;
    float pending_seek_ratio_ = -1.0f;
    int64_t last_drag_seek_us_ = 0;
    float stable_progress_ratio_ = 0.0f;
    bool stable_progress_ready_ = false;
    bool show_log_panel_ = false;
    float log_panel_width_ = 420.0f;
    bool log_auto_scroll_ = true;
    bool log_wrap_lines_ = true;
    int log_level_filter_ = 0;

    static std::string FormatDuration(double seconds);

    void InitImGui();
    void ShutdownImGui();
    void RefreshWindowTitle();
    void SeekToRatio(float ratio);
    void RenderImGui();
    void RenderLogPanel(float bar_height);
    bool OpenFileDialogAndPlay();
    [[noreturn]] void DoExit(VideoState *is);
    void ToggleFullScreen();
    void RefreshLoopWaitEvent(SDL_Event *event);
    void InitWindowAndRenderer();
    void ConfigureVideoRenderer();
    void EventLoop();
};

#endif

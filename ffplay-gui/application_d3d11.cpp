#include <math.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <deque>
#include <mutex>
#include <memory>
#include <vector>

extern "C" {
#include <libavutil/common.h>
#include <libavutil/log.h>
#include <libavutil/mathematics.h>
#include <libavutil/time.h>
#include <libavformat/avformat.h>
#include <libavdevice/avdevice.h>
}

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"
#include "spdlog/logger.h"
#include "spdlog/sinks/base_sink.h"
#include "spdlog/spdlog.h"

#include <windows.h>
#include <commdlg.h>
#include <SDL.h>

#include "application_d3d11.h"

extern "C" {
#include "audio_visualizer.h"
}

/* Forward declare the ImGui Win32 message handler */
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

const char program_name[] = "ffplay";
const int program_birth_year = 2003;

static constexpr int kInitialDefaultWidth = 640;
static constexpr int kInitialDefaultHeight = 480;
static constexpr float kSeekIntervalSeconds = 10.0f;
static constexpr size_t kMaxLogLines = 2000;

/* ------------------------------------------------------------------ */
/* Logging (identical to SDL2 application)                             */
/* ------------------------------------------------------------------ */
namespace {
struct UiLogLine {
    spdlog::level::level_enum level;
    std::string text;
};

class ImGuiLogSink : public spdlog::sinks::base_sink<std::mutex> {
public:
    std::vector<UiLogLine> Snapshot()
    {
        std::lock_guard<std::mutex> lock(base_sink<std::mutex>::mutex_);
        return std::vector<UiLogLine>(lines_.begin(), lines_.end());
    }

    void Clear()
    {
        std::lock_guard<std::mutex> lock(base_sink<std::mutex>::mutex_);
        lines_.clear();
    }

private:
    void sink_it_(const spdlog::details::log_msg &msg) override
    {
        spdlog::memory_buf_t buffer;
        base_sink<std::mutex>::formatter_->format(msg, buffer);
        lines_.push_back({msg.level, std::string(buffer.data(), buffer.size())});
        if (lines_.size() > kMaxLogLines)
            lines_.pop_front();
    }

    void flush_() override {}

    std::deque<UiLogLine> lines_;
};

static std::shared_ptr<ImGuiLogSink> g_log_sink;
static std::shared_ptr<spdlog::logger> g_ui_logger;

static void InitUiLogger()
{
    if (g_ui_logger)
        return;
    g_log_sink = std::make_shared<ImGuiLogSink>();
    g_ui_logger = std::make_shared<spdlog::logger>("ffplay_gui_ui", g_log_sink);
    g_ui_logger->set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");
    g_ui_logger->set_level(spdlog::level::trace);
    spdlog::register_logger(g_ui_logger);
}

static void ShutdownUiLogger()
{
    if (!g_ui_logger)
        return;
    av_log_set_callback(av_log_default_callback);
    spdlog::drop("ffplay_gui_ui");
    g_ui_logger.reset();
    g_log_sink.reset();
}

static spdlog::level::level_enum AvLevelToSpdLevel(int level)
{
    if (level <= AV_LOG_PANIC)   return spdlog::level::critical;
    if (level <= AV_LOG_ERROR)   return spdlog::level::err;
    if (level <= AV_LOG_WARNING) return spdlog::level::warn;
    if (level <= AV_LOG_INFO)    return spdlog::level::info;
    if (level <= AV_LOG_VERBOSE) return spdlog::level::debug;
    return spdlog::level::trace;
}

static void UiAvLogCallback(void *avcl, int level, const char *fmt, va_list vl)
{
    if (!g_ui_logger)
        return;
    char line[2048];
    int print_prefix = 1;
    av_log_format_line2(avcl, level, fmt, vl, line, sizeof(line), &print_prefix);
    std::string text(line);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
        text.pop_back();
    if (text.empty())
        return;
    g_ui_logger->log(AvLevelToSpdLevel(level), "{}", text);
}
}  // namespace

static void sigterm_handler(int sig)
{
    exit(123);
}

/* ------------------------------------------------------------------ */
/* Win32 Window Procedure                                              */
/* ------------------------------------------------------------------ */

LRESULT CALLBACK ApplicationD3D11::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
        return true;

    ApplicationD3D11 *app = (ApplicationD3D11 *)GetWindowLongPtr(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_SIZE:
        if (app && app->video_renderer_ctx_.device && wParam != SIZE_MINIMIZED) {
            int w = LOWORD(lParam);
            int h = HIWORD(lParam);
            video_renderer_d3d11_resize(&app->video_renderer_ctx_, w, h);
            if (app->player_)
                ffplayer_handle_window_size_changed(app->player_, w, h);
            ffplayer_request_refresh(app->player_);
        }
        return 0;
    case WM_KEYDOWN:
        if (app)
            app->HandleKeyDown(wParam);
        return 0;
    case WM_LBUTTONDOWN:
        if (app) {
            if (ImGui::GetIO().WantCaptureMouse)
                break;
            static int64_t last_click = 0;
            if (av_gettime_relative() - last_click <= 500000) {
                app->ToggleFullScreen();
                ffplayer_request_refresh(app->player_);
                last_click = 0;
            } else {
                last_click = av_gettime_relative();
            }
            if (app->cursor_hidden_) {
                ShowCursor(TRUE);
                app->cursor_hidden_ = 0;
            }
            app->cursor_last_shown_ = av_gettime_relative();
        }
        return 0;
    case WM_MOUSEMOVE:
        if (app) {
            if (app->cursor_hidden_) {
                ShowCursor(TRUE);
                app->cursor_hidden_ = 0;
            }
            app->cursor_last_shown_ = av_gettime_relative();
        }
        return 0;
    case WM_CLOSE:
    case WM_DESTROY:
        if (app)
            app->quit_requested_ = true;
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

/* ------------------------------------------------------------------ */
/* Application lifecycle                                               */
/* ------------------------------------------------------------------ */

int ApplicationD3D11::Execute()
{
    InitUiLogger();
    av_log_set_callback(UiAvLogCallback);
    av_log_set_flags(AV_LOG_SKIP_REPEATED);

    avdevice_register_all();
    avformat_network_init();

    signal(SIGINT,  sigterm_handler);
    signal(SIGTERM, sigterm_handler);

    /* Init SDL for audio + timer only (no video) */
    if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
        av_log(nullptr, AV_LOG_FATAL, "Could not initialize SDL audio - %s\n", SDL_GetError());
        exit(1);
    }

    if (!InitWindow()) {
        av_log(nullptr, AV_LOG_FATAL, "Failed to create Win32 window\n");
        exit(1);
    }
    InitD3D11();
    ShowWindow(hwnd_, SW_SHOWDEFAULT);
    UpdateWindow(hwnd_);
    SetWindowTextA(hwnd_, "ffplay-gui");

    player_ = ffplayer_create(&audio_device_);
    {
        enum AVPixelFormat pix_fmts[32];
        int nb = video_renderer_d3d11_get_supported_pixel_formats(&video_renderer_ctx_, pix_fmts, 32);
        ffplayer_set_supported_pixel_formats(player_, pix_fmts, nb);
    }
    /* Forward the renderer-owned D3D11VA hwdevice context so the video
     * decoder can produce GPU-resident NV12 surfaces that the renderer
     * samples directly (zero-copy). The setter takes its own ref. */
    if (AVBufferRef *hw = video_renderer_d3d11_get_hw_device_ctx(&video_renderer_ctx_))
        ffplayer_set_hw_device_ctx(player_, hw);
    ffplayer_set_frame_size_callback(player_, ApplicationD3D11::OnFrameSizeChanged, this);
    InitImGui();

    MainLoop();
    return 0;
}

bool ApplicationD3D11::InitWindow()
{
    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = ApplicationD3D11::WndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = "FFPlayGuiD3D11";
    RegisterClassExA(&wc);

    RECT rc = { 0, 0, kInitialDefaultWidth, kInitialDefaultHeight };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    hwnd_ = CreateWindowA("FFPlayGuiD3D11", "ffplay-gui",
                          WS_OVERLAPPEDWINDOW,
                          CW_USEDEFAULT, CW_USEDEFAULT,
                          rc.right - rc.left, rc.bottom - rc.top,
                          nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
    if (!hwnd_)
        return false;

    SetWindowLongPtr(hwnd_, GWLP_USERDATA, (LONG_PTR)this);
    return true;
}

void ApplicationD3D11::InitD3D11()
{
    video_renderer_ctx_.default_width = kInitialDefaultWidth;
    video_renderer_ctx_.default_height = kInitialDefaultHeight;
    if (video_renderer_d3d11_init(&video_renderer_ctx_, hwnd_) < 0) {
        av_log(nullptr, AV_LOG_FATAL, "Failed to initialize D3D11 renderer\n");
        DoExit();
    }
    av_log(nullptr, AV_LOG_VERBOSE, "Initialized D3D11 renderer.\n");
}

void ApplicationD3D11::InitImGui()
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui_ImplWin32_Init(hwnd_);
    ImGui_ImplDX11_Init(video_renderer_ctx_.device, video_renderer_ctx_.context);
    imgui_ready_ = true;
}

void ApplicationD3D11::ShutdownImGui()
{
    if (!imgui_ready_)
        return;
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    imgui_ready_ = false;
}

/* ------------------------------------------------------------------ */
/* Main Loop — replaces SDL event loop with Win32 message pump         */
/* ------------------------------------------------------------------ */

void ApplicationD3D11::MainLoop()
{
    MSG msg;
    for (;;) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                quit_requested_ = true;
        }

        if (quit_requested_ || ffplayer_has_quit_request(player_))
            DoExit();

        /* Cursor auto-hide */
        if (!cursor_hidden_ && av_gettime_relative() - cursor_last_shown_ > FFPLAYER_CURSOR_HIDE_DELAY) {
            ShowCursor(FALSE);
            cursor_hidden_ = 1;
        }

        /* Playback refresh */
        double remaining_time = FFPLAYER_REFRESH_RATE;
        if (ffplayer_needs_refresh(player_))
            ffplayer_refresh(player_, &remaining_time);

        /* Render */
        video_renderer_d3d11_clear(&video_renderer_ctx_);
        if (ffplayer_is_open(player_))
            DisplayVideo();
        RenderImGui();
        video_renderer_d3d11_present(&video_renderer_ctx_);

        /* Sleep a bit if nothing to do */
        if (remaining_time > 0.0)
            av_usleep((int64_t)(remaining_time * 1000000.0));
    }
}

/* ------------------------------------------------------------------ */
/* Keyboard handling                                                   */
/* ------------------------------------------------------------------ */

void ApplicationD3D11::HandleKeyDown(WPARAM wParam)
{
    if (wParam == VK_ESCAPE || wParam == 'Q') {
        DoExit();
        return;
    }
    if (ImGui::GetIO().WantCaptureKeyboard)
        return;
    if (wParam == 'O') {
        OpenFileDialogAndPlay();
        return;
    }
    if (wParam == 'F') {
        ToggleFullScreen();
        ffplayer_request_refresh(player_);
        return;
    }
    if (!ffplayer_is_open(player_) || !ffplayer_is_video_open(player_))
        return;

    double incr;
    switch (wParam) {
    case 'P':
    case VK_SPACE:
        ffplayer_toggle_pause(player_);
        break;
    case 'M':
        ffplayer_toggle_mute(player_);
        break;
    case '0':
    case VK_MULTIPLY:
        ffplayer_adjust_volume_step(player_, 1, FFPLAYER_VOLUME_STEP);
        break;
    case '9':
    case VK_DIVIDE:
        ffplayer_adjust_volume_step(player_, -1, FFPLAYER_VOLUME_STEP);
        break;
    case 'S':
        ffplayer_step_frame(player_);
        break;
    case 'A':
        ffplayer_cycle_audio_track(player_);
        break;
    case 'V':
        ffplayer_cycle_video_track(player_);
        break;
    case 'C':
        ffplayer_cycle_all_tracks(player_);
        break;
    case 'T':
        ffplayer_cycle_subtitle_track(player_);
        break;
    case 'W':
        ffplayer_toggle_audio_display(player_);
        break;
    case VK_PRIOR:
        if (!ffplayer_has_chapters(player_)) { incr = 600.0; goto do_seek; }
        ffplayer_seek_chapter(player_, 1);
        break;
    case VK_NEXT:
        if (!ffplayer_has_chapters(player_)) { incr = -600.0; goto do_seek; }
        ffplayer_seek_chapter(player_, -1);
        break;
    case VK_LEFT:
        incr = -kSeekIntervalSeconds;
        goto do_seek;
    case VK_RIGHT:
        incr = kSeekIntervalSeconds;
        goto do_seek;
    case VK_UP:
        incr = 60.0;
        goto do_seek;
    case VK_DOWN:
        incr = -60.0;
do_seek:
        ffplayer_seek_relative(player_, incr);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Video display                                                       */
/* ------------------------------------------------------------------ */

void ApplicationD3D11::DisplayVideo()
{
    RECT rc;
    GetClientRect(hwnd_, &rc);
    int win_w = rc.right - rc.left;
    int win_h = rc.bottom - rc.top;

    if (!video_open_done_) {
        video_renderer_d3d11_open(&video_renderer_ctx_, &win_w, &win_h);
        ffplayer_set_window_size(player_, win_w, win_h);
        video_open_done_ = 1;
    }

    enum FFPlayerShowMode mode = ffplayer_get_show_mode(player_);
    if (mode == FFPLAYER_SHOW_MODE_VIDEO) {
        AVFrame *frame = ffplayer_get_video_frame(player_);
        AVSubtitle *subtitle = ffplayer_get_subtitle(player_);
        if (frame)
            video_renderer_d3d11_draw_video(&video_renderer_ctx_, frame, subtitle, 0, 0, win_w, win_h);
    }
    /* Audio visualization is not supported in D3D11 backend (video mode only) */
}

/* ------------------------------------------------------------------ */
/* Window title / fullscreen                                           */
/* ------------------------------------------------------------------ */

void ApplicationD3D11::RefreshWindowTitle()
{
    if (ffplayer_is_open(player_)) {
        const char *title = ffplayer_get_media_title(player_);
        SetWindowTextA(hwnd_, (title && title[0]) ? title : "ffplay-gui");
        return;
    }
    SetWindowTextA(hwnd_, "ffplay-gui - No media loaded");
}

void ApplicationD3D11::ToggleFullScreen()
{
    DWORD style = GetWindowLong(hwnd_, GWL_STYLE);
    if (!is_full_screen_) {
        MONITORINFO mi = { sizeof(mi) };
        if (GetWindowPlacement(hwnd_, &wp_before_fullscreen_) &&
            GetMonitorInfo(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi)) {
            SetWindowLong(hwnd_, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
            SetWindowPos(hwnd_, HWND_TOP,
                         mi.rcMonitor.left, mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        }
    } else {
        SetWindowLong(hwnd_, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(hwnd_, &wp_before_fullscreen_);
        SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    is_full_screen_ = !is_full_screen_;
}

void ApplicationD3D11::SeekToRatio(float ratio)
{
    ffplayer_seek_to_ratio(player_, ratio);
}

/* ------------------------------------------------------------------ */
/* ImGui rendering (same UI as SDL2 backend)                           */
/* ------------------------------------------------------------------ */

std::string ApplicationD3D11::FormatDuration(double seconds)
{
    int total = (int)(seconds >= 0.0 ? seconds : 0.0);
    int h = total / 3600;
    int m = (total % 3600) / 60;
    int s = total % 60;
    char buf[32];
    if (h > 0)
        snprintf(buf, sizeof(buf), "%d:%02d:%02d", h, m, s);
    else
        snprintf(buf, sizeof(buf), "%02d:%02d", m, s);
    return std::string(buf);
}

void ApplicationD3D11::RenderImGui()
{
    ImGuiIO &io = ImGui::GetIO();
    float bar_height = 52.0f;
    float margin = 12.0f;
    float progress = 0.0f;
    float volume_percent = 0.0f;
    double duration_sec = 0.0;
    double current_sec = 0.0;
    std::string play_text = "Play";
    std::string time_text = "00:00 / 00:00";
    bool has_known_duration = false;
    bool can_approx_seek = false;
    bool can_seek = false;
    bool using_stable_progress = false;

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    if (ffplayer_is_open(player_)) {
        duration_sec = ffplayer_get_duration(player_);
        has_known_duration = duration_sec > 0;
        can_seek = ffplayer_can_seek(player_);
        can_approx_seek = can_seek && !has_known_duration;

        if (has_known_duration) {
            current_sec = ffplayer_get_position(player_);
            current_sec = FFMAX(0.0, FFMIN(current_sec, duration_sec));
            progress = duration_sec > 0.0 ? (float)(current_sec / duration_sec) : 0.0f;

            if (ffplayer_is_eof(player_) && progress > 0.90f)
                progress = 1.0f;

            if (!isfinite(progress))
                progress = 0.0f;
            using_stable_progress = true;
            time_text = FormatDuration(current_sec) + " / " + FormatDuration(duration_sec);
        } else if (can_approx_seek) {
            float bp = ffplayer_get_byte_progress(player_);
            if (bp >= 0.0f)
                progress = bp;
            time_text = "Unknown duration | Approx seek";
        } else {
            time_text = "Unknown duration";
        }

        play_text = ffplayer_is_paused(player_) ? "Play" : "Pause";
    }
    if (!ffplayer_is_open(player_))
        stable_progress_ready_ = false;

    if (using_stable_progress && pending_seek_ratio_ < 0.0f) {
        if (!stable_progress_ready_) {
            stable_progress_ratio_ = progress;
            stable_progress_ready_ = true;
        } else {
            float delta = progress - stable_progress_ratio_;
            if (delta >= -0.003f) {
                stable_progress_ratio_ = FFMAX(stable_progress_ratio_, progress);
            } else if (delta <= -0.08f) {
                stable_progress_ratio_ = progress;
            }
        }
        progress = stable_progress_ratio_;
    }
    if (!isfinite(progress))
        progress = 0.0f;
    if (ffplayer_is_open(player_))
        volume_percent = 100.0f * (float)ffplayer_get_volume(player_) / (float)SDL_MIX_MAXVOLUME;

    ImGui::SetNextWindowPos(ImVec2(0.0f, io.DisplaySize.y - bar_height));
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, bar_height));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.03f, 0.04f, 0.08f, 0.85f));
    ImGui::Begin("PlayerControls", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);

    if (!ffplayer_is_open(player_)) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("No media loaded");
        ImGui::SameLine();
        if (ImGui::Button("Open file..."))
            OpenFileDialogAndPlay();
        ImGui::SameLine();
        if (ImGui::Button(show_log_panel_ ? "Hide Log" : "Log"))
            show_log_panel_ = !show_log_panel_;
    } else {
        float avail_w = ImGui::GetContentRegionAvail().x;
        float play_w = ImGui::CalcTextSize(play_text.c_str()).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        float time_w = ImGui::CalcTextSize(time_text.c_str()).x;
        float volume_w = FFMIN(110.0f, FFMAX(72.0f, avail_w * 0.22f));
        float log_button_w = ImGui::CalcTextSize(show_log_panel_ ? "Hide Log" : "Log").x +
            ImGui::GetStyle().FramePadding.x * 2.0f;
        float min_timeline_w = 80.0f;
        bool hide_time_text = false;
        float needed_w = play_w + margin + time_w + margin + min_timeline_w + margin + volume_w +
            margin + log_button_w;

        if (avail_w < needed_w) {
            hide_time_text = true;
            needed_w = play_w + margin + min_timeline_w + margin + volume_w + margin + log_button_w;
        }
        if (avail_w < needed_w) {
            volume_w = FFMAX(58.0f, avail_w * 0.18f);
        }

        float timeline_w = FFMAX(
            min_timeline_w,
            avail_w - play_w - margin - (hide_time_text ? 0.0f : (time_w + margin)) - margin - volume_w -
                margin - log_button_w);

        if (ImGui::Button(play_text.c_str()))
            ffplayer_toggle_pause(player_);
        if (!hide_time_text) {
            ImGui::SameLine(0.0f, margin);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(time_text.c_str());
        }
        ImGui::SameLine(0.0f, margin);
        ImGui::SetNextItemWidth(timeline_w);
        float slider_value = pending_seek_ratio_ >= 0.0f ? pending_seek_ratio_ : progress;
        if (!can_seek)
            ImGui::BeginDisabled();
        if (ImGui::SliderFloat("##timeline", &slider_value, 0.0f, 1.0f, "")) {
            pending_seek_ratio_ = slider_value;
            if (ImGui::IsItemActive()) {
                int64_t now_us = av_gettime_relative();
                if (last_drag_seek_us_ == 0 || now_us - last_drag_seek_us_ >= 40000) {
                    SeekToRatio(pending_seek_ratio_);
                    last_drag_seek_us_ = now_us;
                }
            }
        }
        if (pending_seek_ratio_ >= 0.0f && ImGui::IsItemDeactivatedAfterEdit()) {
            SeekToRatio(pending_seek_ratio_);
            stable_progress_ratio_ = pending_seek_ratio_;
            stable_progress_ready_ = true;
            pending_seek_ratio_ = -1.0f;
            last_drag_seek_us_ = 0;
        }
        if (!can_seek)
            ImGui::EndDisabled();
        ImGui::SameLine(0.0f, margin);
        ImGui::SetNextItemWidth(volume_w);
        const char *volume_fmt = volume_w >= 72.0f ? "%.0f%%" : "";
        if (ImGui::SliderFloat("##volume", &volume_percent, 0.0f, 100.0f, volume_fmt)) {
            ffplayer_set_volume(player_, (int)((volume_percent / 100.0f) * SDL_MIX_MAXVOLUME));
        }
        ImGui::SameLine(0.0f, margin);
        if (ImGui::Button(show_log_panel_ ? "Hide Log" : "Log"))
            show_log_panel_ = !show_log_panel_;
    }

    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    RenderLogPanel(bar_height);
    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

void ApplicationD3D11::RenderLogPanel(float bar_height)
{
    if (!show_log_panel_ || !g_log_sink)
        return;

    ImGuiIO &io = ImGui::GetIO();
    float panel_height = FFMAX(120.0f, io.DisplaySize.y - bar_height);
    float min_width = 280.0f;
    float max_width = FFMAX(min_width, io.DisplaySize.x * 0.85f);
    log_panel_width_ = av_clipf(log_panel_width_, min_width, max_width);

    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - log_panel_width_, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(log_panel_width_, panel_height), ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints(ImVec2(min_width, 120.0f), ImVec2(max_width, io.DisplaySize.y));
    if (!ImGui::Begin("Logs", &show_log_panel_, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    log_panel_width_ = ImGui::GetWindowWidth();
    const char *level_names[] = {"All", "Info+", "Warn+", "Error+"};
    ImGui::SetNextItemWidth(110.0f);
    ImGui::Combo("Level", &log_level_filter_, level_names, IM_ARRAYSIZE(level_names));
    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &log_auto_scroll_);
    ImGui::SameLine();
    ImGui::Checkbox("Wrap", &log_wrap_lines_);
    ImGui::SameLine();
    if (ImGui::Button("Clear"))
        g_log_sink->Clear();

    ImGui::Separator();
    ImGuiWindowFlags child_flags = ImGuiWindowFlags_HorizontalScrollbar;
    ImGui::BeginChild("LogScrollRegion", ImVec2(0.0f, 0.0f), false, child_flags);

    std::vector<UiLogLine> lines = g_log_sink->Snapshot();
    if (log_wrap_lines_)
        ImGui::PushTextWrapPos();
    for (const UiLogLine &line : lines) {
        if (log_level_filter_ == 1 && line.level < spdlog::level::info) continue;
        if (log_level_filter_ == 2 && line.level < spdlog::level::warn) continue;
        if (log_level_filter_ == 3 && line.level < spdlog::level::err)  continue;

        if (line.level >= spdlog::level::err)
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.00f, 0.45f, 0.45f, 1.0f));
        else if (line.level == spdlog::level::warn)
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.00f, 0.85f, 0.35f, 1.0f));

        ImGui::TextUnformatted(line.text.c_str());

        if (line.level >= spdlog::level::warn)
            ImGui::PopStyleColor();
    }
    if (log_wrap_lines_)
        ImGui::PopTextWrapPos();
    if (log_auto_scroll_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 10.0f)
        ImGui::SetScrollHereY(1.0f);

    ImGui::EndChild();
    ImGui::End();
}

/* ------------------------------------------------------------------ */
/* File dialog                                                         */
/* ------------------------------------------------------------------ */

bool ApplicationD3D11::OpenFileDialogAndPlay()
{
    wchar_t file_name[MAX_PATH] = {0};
    OPENFILENAMEW ofn = {0};

    if (open_dialog_active_)
        return false;
    open_dialog_active_ = true;

    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFile = file_name;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter =
        L"Media Files\0*.mp4;*.mkv;*.avi;*.mov;*.wmv;*.flv;*.webm;*.mp3;*.wav;*.flac;*.aac;*.ogg;*.m4a\0"
        L"All Files\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    ofn.lpstrTitle = L"Open media file";

    if (!GetOpenFileNameW(&ofn)) {
        open_dialog_active_ = false;
        return false;
    }

    int utf8_len = WideCharToMultiByte(CP_UTF8, 0, file_name, -1, nullptr, 0, nullptr, nullptr);
    if (utf8_len <= 0) {
        open_dialog_active_ = false;
        return false;
    }
    std::vector<char> utf8_path(static_cast<size_t>(utf8_len), 0);
    if (WideCharToMultiByte(CP_UTF8, 0, file_name, -1, utf8_path.data(), utf8_len, nullptr, nullptr) <= 0) {
        open_dialog_active_ = false;
        return false;
    }

    ffplayer_close(player_);
    video_renderer_d3d11_cleanup_textures(&video_renderer_ctx_);
    video_open_done_ = 0;
    stable_progress_ready_ = false;

    if (ffplayer_open(player_, utf8_path.data()) < 0) {
        MessageBoxA(hwnd_, "Failed to open selected media file.", "Open failed", MB_ICONERROR);
        RefreshWindowTitle();
        open_dialog_active_ = false;
        return false;
    }
    ffplayer_toggle_pause(player_);
    ffplayer_request_refresh(player_);
    stable_progress_ratio_ = 0.0f;
    stable_progress_ready_ = false;

    RefreshWindowTitle();
    open_dialog_active_ = false;
    return true;
}

/* ------------------------------------------------------------------ */
/* Exit / Cleanup                                                      */
/* ------------------------------------------------------------------ */

[[noreturn]] void ApplicationD3D11::DoExit()
{
    video_renderer_d3d11_cleanup_textures(&video_renderer_ctx_);
    ffplayer_free(&player_);
    ShutdownImGui();
    video_renderer_d3d11_shutdown(&video_renderer_ctx_);
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    avformat_network_deinit();
    ShutdownUiLogger();
    SDL_Quit();
    av_log(nullptr, AV_LOG_QUIET, "%s", "");
    exit(0);
}

void ApplicationD3D11::OnFrameSizeChanged(void *opaque, int width, int height, AVRational sar)
{
    auto *app = static_cast<ApplicationD3D11 *>(opaque);
    if (!app)
        return;
    video_renderer_d3d11_set_default_window_size(&app->video_renderer_ctx_,
                                                  app->video_renderer_ctx_.default_width,
                                                  app->video_renderer_ctx_.default_height,
                                                  width, height, sar);
}

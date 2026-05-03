#include <signal.h>
#include <stdio.h>
#include <string.h>

#include <string>

extern "C" {
#include <libavutil/pixdesc.h>
#include <libavutil/time.h>
#include <libavformat/avformat.h>
#include <libavdevice/avdevice.h>
}

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"

#include <windows.h>
#include <SDL.h>

#include "application.h"
#include "native/win32_file_dialog.h"
#include "native/win32_window_proc.h"
#include "ui/ui_main_menu.h"

namespace {

constexpr size_t kTitleUtf16MaxBytes = 4096;

static bool LooksUtf16LeBom(const unsigned char *p)
{
    return p[0] == 0xFF && p[1] == 0xFE;
}

/** UTF-16LE BMP text prefix (wchar buffers mis-tagged as UTF-8): xx 00 xx 00 … */
static bool LooksUtf16LeBmpPrefix(const unsigned char *r)
{
    return r[0] != 0 && r[1] == 0 && r[2] != 0 && r[3] == 0;
}

/** Walk UTF-16LE until wchar NUL or max_bytes; used when FFmpeg stores a wchar path in char *. */
static bool Utf16LeToWideCaption(const unsigned char *data, size_t max_bytes, std::wstring *out)
{
    out->clear();
    size_t i = 0;
    while (i + 1 < max_bytes) {
        uint16_t wc =
            static_cast<uint16_t>(data[i]) | (static_cast<uint16_t>(data[i + 1]) << 8);
        i += 2;
        if (wc == 0)
            break;
        out->push_back(static_cast<wchar_t>(wc));
    }
    return !out->empty();
}

/**
 * NUL-terminated caption: UTF-8 -> UTF-16 for SetWindowTextW, or UTF-16LE blob heuristics.
 * (Former TextToAsciiWindowTitle forced non-ASCII to '?', which breaks Chinese paths in the title bar.)
 */
static std::wstring CaptionUtf8ToWide(const char *text)
{
    std::wstring out;
    if (!text || !text[0])
        return out;

    const unsigned char *raw = reinterpret_cast<const unsigned char *>(text);
    if (LooksUtf16LeBom(raw)) {
        if (Utf16LeToWideCaption(raw + 2, kTitleUtf16MaxBytes - 2, &out))
            return out;
    } else if (LooksUtf16LeBmpPrefix(raw)) {
        if (Utf16LeToWideCaption(raw, kTitleUtf16MaxBytes, &out))
            return out;
    }

    int n = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (n <= 0)
        return out;
    out.assign(static_cast<size_t>(n), L'\0');
    if (MultiByteToWideChar(CP_UTF8, 0, text, -1, out.data(), n) <= 0)
        out.clear();
    else if (!out.empty() && out.back() == L'\0')
        out.pop_back();
    return out;
}

static void SetWindowTitleUtf8(HWND hwnd, const char *text)
{
    if (!hwnd)
        return;
    if (!text || !text[0]) {
        SetWindowTextW(hwnd, L"ffplay-gui");
        return;
    }
    std::wstring w = CaptionUtf8ToWide(text);
    SetWindowTextW(hwnd, !w.empty() ? w.c_str() : L"ffplay-gui");
}

} /* namespace */

static constexpr int kInitialDefaultWidth = 640;
static constexpr int kInitialDefaultHeight = 480;

static void sigterm_handler(int sig)
{
    exit(123);
}

int Application::Execute()
{
    avdevice_register_all();
    avformat_network_init();

    signal(SIGINT, sigterm_handler);
    signal(SIGTERM, sigterm_handler);

    if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
        fprintf(stderr, "Could not initialize SDL audio - %s\n", SDL_GetError());
        exit(1);
    }

    if (!InitWindow()) {
        fprintf(stderr, "Failed to create Win32 window\n");
        exit(1);
    }
    InitRenderer();
    ShowWindow(hwnd_, SW_SHOWDEFAULT);
    UpdateWindow(hwnd_);
    SetWindowTitleUtf8(hwnd_, "ffplay-gui");

    player_ = ffplayer_create(&audio_device_);
    {
        enum AVPixelFormat pix_fmts[32];
        int nb = video_renderer_get_supported_pixel_formats(&video_renderer_ctx_, pix_fmts, 32);
        ffplayer_set_supported_pixel_formats(player_, pix_fmts, nb);
    }
    if (AVBufferRef *hw = video_renderer_get_hw_device_ctx(&video_renderer_ctx_))
        ffplayer_set_hw_device_ctx(player_, hw);
    ffplayer_set_frame_size_callback(player_, Application::OnFrameSizeChanged, this);
    ffplayer_set_infinite_buffer(player_, startup_infinite_buffer_);
    ffplayer_set_av_sync_type(player_, startup_av_sync_type_);
    InitImGui();

    MainLoop();
    return 0;
}

bool Application::InitWindow()
{
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = ApplicationWin32WndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"FFPlayGuiD3D11";
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;

    RECT rc = { 0, 0, kInitialDefaultWidth, kInitialDefaultHeight };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    hwnd_ = CreateWindowExW(0, L"FFPlayGuiD3D11", L"ffplay-gui",
                            WS_OVERLAPPEDWINDOW,
                            CW_USEDEFAULT, CW_USEDEFAULT,
                            rc.right - rc.left, rc.bottom - rc.top,
                            nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
    if (!hwnd_)
        return false;

    SetWindowLongPtr(hwnd_, GWLP_USERDATA, (LONG_PTR)this);
    return true;
}

void Application::InitRenderer()
{
    video_renderer_ctx_.default_width = kInitialDefaultWidth;
    video_renderer_ctx_.default_height = kInitialDefaultHeight;
    if (video_renderer_init(&video_renderer_ctx_, hwnd_) < 0) {
        fprintf(stderr, "Failed to initialize renderer\n");
        DoExit();
    }
}

void Application::InitImGui()
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.Fonts->AddFontDefault();
    ImGui_ImplWin32_Init(hwnd_);
    ImGui_ImplDX11_Init(video_renderer_ctx_.device, video_renderer_ctx_.context);
    imgui_ready_ = true;
}

void Application::ShutdownImGui()
{
    if (!imgui_ready_)
        return;
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    imgui_ready_ = false;
}

void Application::MainLoop()
{
    MSG msg;
    for (;;) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                quit_requested_ = true;
        }

        if (quit_requested_)
            DoExit();

        if (ffplayer_has_quit_request(player_)) {
            HandlePlaybackFatalError();
            continue;
        }

        if (!cursor_hidden_ && av_gettime_relative() - cursor_last_shown_ > FFPLAYER_CURSOR_HIDE_DELAY) {
            ShowCursor(FALSE);
            cursor_hidden_ = 1;
        }

        double remaining_time = FFPLAYER_REFRESH_RATE;
        if (ffplayer_needs_refresh(player_))
            ffplayer_refresh(player_, &remaining_time);

        video_renderer_clear(&video_renderer_ctx_);
        if (ffplayer_is_open(player_))
            DisplayVideo();
        RenderImGui();
        video_renderer_present(&video_renderer_ctx_);

        if (remaining_time > 0.0)
            av_usleep((int64_t)(remaining_time * 1000000.0));
    }
}

void Application::win32_on_client_resize(WPARAM size_type, int client_w, int client_h)
{
    if (!video_renderer_ctx_.device || size_type == SIZE_MINIMIZED)
        return;
    video_renderer_resize(&video_renderer_ctx_, client_w, client_h);
    if (player_)
        ffplayer_handle_window_size_changed(player_, client_w, client_h);
    ffplayer_request_refresh(player_);
}

void Application::win32_request_quit()
{
    quit_requested_ = true;
}

void Application::ui_toggle_fullscreen()
{
    ToggleFullScreen();
}

void Application::ui_player_request_refresh()
{
    ffplayer_request_refresh(player_);
}

void Application::ui_note_mouse_activity_show_cursor()
{
    if (cursor_hidden_) {
        ShowCursor(TRUE);
        cursor_hidden_ = 0;
    }
    cursor_last_shown_ = av_gettime_relative();
}

[[noreturn]] void Application::ui_quit_application()
{
    DoExit();
}

void Application::DisplayVideo()
{
    RECT rc;
    GetClientRect(hwnd_, &rc);
    int win_w = rc.right - rc.left;
    int win_h = rc.bottom - rc.top;

    if (!video_open_done_) {
        video_renderer_open(&video_renderer_ctx_, &win_w, &win_h);
        ffplayer_set_window_size(player_, win_w, win_h);
        video_open_done_ = 1;
    }

    enum FFPlayerShowMode mode = ffplayer_get_show_mode(player_);
    if (mode == FFPLAYER_SHOW_MODE_VIDEO) {
        AVFrame *frame = ffplayer_get_video_frame(player_);
        AVSubtitle *subtitle = ffplayer_get_subtitle(player_);
        if (frame) {
            UpdatePipelineStatsFromFrame(frame);
            video_renderer_draw_video(&video_renderer_ctx_, frame, subtitle, 0, 0, win_w, win_h);
        } else {
            stats_has_video_frame_ = false;
        }
    } else {
        stats_has_video_frame_ = false;
    }
}

void Application::RefreshWindowTitle()
{
    if (ffplayer_is_open(player_)) {
        const char *u8 = ffplayer_get_media_title(player_);
        if (u8 && u8[0])
            SetWindowTitleUtf8(hwnd_, u8);
        else
            SetWindowTitleUtf8(hwnd_, "ffplay-gui");
        return;
    }
    SetWindowTitleUtf8(hwnd_, "ffplay-gui - No media loaded");
}

void Application::ToggleFullScreen()
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

void Application::SeekToRatio(float ratio)
{
    ffplayer_seek_to_ratio(player_, ratio);
}

void Application::HandlePlaybackFatalError()
{
    ffplayer_close(player_);
    video_renderer_cleanup_textures(&video_renderer_ctx_);
    video_open_done_ = 0;
    pending_seek_ratio_ = -1.0f;
    last_drag_seek_us_ = 0;
    stable_progress_ratio_ = 0.0f;
    stable_progress_ready_ = false;
    RefreshWindowTitle();
    MessageBoxA(hwnd_,
                "The media could not be opened or played.\nCheck the URL or network, or see the FFmpeg log.",
                "ffplay-gui",
                MB_OK | MB_ICONWARNING);
}

void Application::StopPlaybackAndReset()
{
    video_renderer_cleanup_textures(&video_renderer_ctx_);
    ffplayer_free(&player_);
    player_ = ffplayer_create(&audio_device_);
    if (!player_) {
        MessageBoxA(hwnd_, "Failed to recreate player.", "Stop failed", MB_ICONERROR);
        DoExit();
    }

    enum AVPixelFormat pix_fmts[32];
    int nb = video_renderer_get_supported_pixel_formats(&video_renderer_ctx_, pix_fmts, 32);
    ffplayer_set_supported_pixel_formats(player_, pix_fmts, nb);
    if (AVBufferRef *hw = video_renderer_get_hw_device_ctx(&video_renderer_ctx_))
        ffplayer_set_hw_device_ctx(player_, hw);
    ffplayer_set_frame_size_callback(player_, Application::OnFrameSizeChanged, this);
    ffplayer_set_infinite_buffer(player_, startup_infinite_buffer_);
    ffplayer_set_av_sync_type(player_, startup_av_sync_type_);

    video_open_done_ = 0;
    pending_seek_ratio_ = -1.0f;
    last_drag_seek_us_ = 0;
    stable_progress_ratio_ = 0.0f;
    stable_progress_ready_ = false;
    stats_has_video_frame_ = false;
    stats_pipeline_zero_copy_ = false;
    stats_video_pix_fmt_ = AV_PIX_FMT_NONE;
    RefreshWindowTitle();
}

void Application::UpdateRenderFps()
{
    const int64_t now_us = av_gettime_relative();
    if (render_fps_last_sample_us_ <= 0) {
        render_fps_last_sample_us_ = now_us;
        render_fps_frame_count_ = 0;
        render_fps_ = 0.0f;
        render_frame_time_ms_ = 0.0f;
        return;
    }

    ++render_fps_frame_count_;
    const int64_t elapsed_us = now_us - render_fps_last_sample_us_;
    if (elapsed_us >= 500000) {
        render_fps_ = (float)render_fps_frame_count_ * 1000000.0f / (float)elapsed_us;
        render_frame_time_ms_ = render_fps_ > 0.0f ? (1000.0f / render_fps_) : 0.0f;
        render_fps_frame_count_ = 0;
        render_fps_last_sample_us_ = now_us;
    }
}

void Application::UpdatePipelineStatsFromFrame(const AVFrame *frame)
{
    if (!frame) {
        stats_has_video_frame_ = false;
        stats_pipeline_zero_copy_ = false;
        stats_video_pix_fmt_ = AV_PIX_FMT_NONE;
        return;
    }
    stats_has_video_frame_ = true;
    stats_video_pix_fmt_ = (AVPixelFormat)frame->format;
    const char *pix_fmt_name = av_get_pix_fmt_name(stats_video_pix_fmt_);
    stats_pipeline_zero_copy_ = pix_fmt_name && strcmp(pix_fmt_name, "d3d11") == 0;
}

void Application::RenderImGui()
{
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    UpdateRenderFps();

    const float main_menu_bar_bottom = UiDrawMainMenuBar(&ui_show_statistics());
    UiDrawStartupDefaults(*this, main_menu_bar_bottom);
    UiDrawStatisticsWindow(*this);

    const bool stop_requested = UiDrawPlayerControls(*this);

    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    if (stop_requested)
        StopPlaybackAndReset();
}

bool Application::OpenFileDialogAndPlay()
{
    if (open_dialog_active_)
        return false;
    open_dialog_active_ = true;

    std::string path;
    if (!Win32PickMediaFileUtf8(hwnd_, path)) {
        open_dialog_active_ = false;
        return false;
    }

    bool ok = OpenMediaAndPlay(path.c_str(), "Failed to open selected media file.");
    open_dialog_active_ = false;
    return ok;
}

bool Application::OpenUrlAndPlay()
{
    std::string url(open_url_input_);
    size_t begin = url.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos)
        return false;
    size_t end = url.find_last_not_of(" \t\r\n");
    url = url.substr(begin, end - begin + 1);
    if (!OpenMediaAndPlay(url.c_str(), "Failed to open media URL."))
        return false;
    open_url_input_[0] = '\0';
    return true;
}

bool Application::OpenMediaAndPlay(const char *source, const char *error_message)
{
    if (!source || !source[0])
        return false;

    ffplayer_close(player_);
    video_renderer_cleanup_textures(&video_renderer_ctx_);
    video_open_done_ = 0;
    stable_progress_ready_ = false;

    if (ffplayer_open(player_, source) < 0) {
        MessageBoxA(hwnd_, error_message, "Open failed", MB_ICONERROR);
        RefreshWindowTitle();
        return false;
    }
    ffplayer_toggle_pause(player_);
    ffplayer_request_refresh(player_);
    stable_progress_ratio_ = 0.0f;
    stable_progress_ready_ = false;
    RefreshWindowTitle();
    return true;
}

void Application::DoExit()
{
    video_renderer_cleanup_textures(&video_renderer_ctx_);
    ffplayer_free(&player_);
    ShutdownImGui();
    video_renderer_shutdown(&video_renderer_ctx_);
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    avformat_network_deinit();
    SDL_Quit();
    exit(0);
}

void Application::OnFrameSizeChanged(void *opaque, int width, int height, AVRational sar)
{
    auto *app = static_cast<Application *>(opaque);
    if (!app)
        return;
    video_renderer_set_default_window_size(&app->video_renderer_ctx_,
                                         app->video_renderer_ctx_.default_width,
                                         app->video_renderer_ctx_.default_height,
                                         width, height, sar);
}

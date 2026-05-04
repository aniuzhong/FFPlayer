#include <signal.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <optional>
#include <string>
#include <thread>

using namespace std::chrono_literals;

extern "C" {
#include <libavutil/pixdesc.h>
#include <libavformat/avformat.h>
#include <libavdevice/avdevice.h>
}

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"

#include <windows.h>
#include <SDL.h>

extern "C" {
#include "utils/tinyfiledialogs.h"
}

#ifdef __cplusplus
extern "C" {
#endif
#include "ffplayer/audio_device.h"
#include "ffplayer/ffplayer.h"
#ifdef __cplusplus
}
#endif

#include "render/video_renderer.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {

constexpr size_t kTitleUtf16MaxBytes = 4096;
constexpr int kInitialDefaultWidth = 640;
constexpr int kInitialDefaultHeight = 480;
constexpr float kSeekIntervalSeconds = 10.0f;

static bool LooksUtf16LeBom(const unsigned char *p)
{
    return p[0] == 0xFF && p[1] == 0xFE;
}

static bool LooksUtf16LeBmpPrefix(const unsigned char *r)
{
    return r[0] != 0 && r[1] == 0 && r[2] != 0 && r[3] == 0;
}

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

static bool Utf8CaptionToWide(const char *utf8, std::wstring *out)
{
    out->clear();
    if (!utf8 || !utf8[0])
        return false;
    const int nwchars =
        MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (nwchars <= 0)
        return false;
    out->resize(static_cast<size_t>(nwchars - 1));
    return MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out->data(), nwchars) == nwchars;
}

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

    if (!Utf8CaptionToWide(text, &out))
        out.clear();
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

static void sigterm_handler(int sig)
{
    (void)sig;
    exit(123);
}

static std::string FormatDuration(double seconds)
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

static bool PickMediaFileUtf8(std::string &out_utf8_path)
{
    out_utf8_path.clear();

    static char const *const kFilterPatterns[] = {
        "*.mp4",
        "*.mkv",
        "*.avi",
        "*.mov",
        "*.wmv",
        "*.flv",
        "*.webm",
        "*.mp3",
        "*.wav",
        "*.flac",
        "*.aac",
        "*.ogg",
        "*.m4a",
        "*.*",
    };

    char *path = tinyfd_openFileDialog(
        "Open media file",
        "",
        static_cast<int>(sizeof(kFilterPatterns) / sizeof(kFilterPatterns[0])),
        kFilterPatterns,
        "Media files",
        0);
    if (!path || !path[0])
        return false;

    out_utf8_path.assign(path);
    return true;
}

void DispatchKeyDown(WPARAM wParam);

} /* namespace */

static HWND g_hwnd = nullptr;
static AudioDevice g_audio_device = {};
static std::optional<std::chrono::steady_clock::time_point> g_cursor_last_active;
static int g_cursor_hidden = 0;
static int g_is_full_screen = 0;
static WINDOWPLACEMENT g_wp_before_fullscreen = {};
static FFPlayer *g_player = nullptr;
static VideoRenderer g_video_renderer_ctx = {};
static int g_video_open_done = 0;
static bool g_open_dialog_active = false;
static bool g_imgui_ready = false;
static float g_pending_seek_ratio = -1.0f;
static std::optional<std::chrono::steady_clock::time_point> g_last_drag_seek;
static float g_stable_progress_ratio = 0.0f;
static bool g_stable_progress_ready = false;
static char g_open_url_input[1024] = {};
static bool g_show_statistics_window = false;
static float g_render_fps = 0.0f;
static float g_render_frame_time_ms = 0.0f;
static int g_render_fps_frame_count = 0;
static std::optional<std::chrono::steady_clock::time_point> g_render_fps_sample;
static bool g_stats_has_video_frame = false;
static bool g_stats_pipeline_zero_copy = false;
static AVPixelFormat g_stats_video_pix_fmt = AV_PIX_FMT_NONE;
static bool g_quit_requested = false;
static int g_startup_infinite_buffer = -1;
static int g_startup_av_sync_type = FFPLAYER_AV_SYNC_AUDIO_MASTER;

static void RefreshWindowTitle();
static void SeekToRatio(float ratio);
static void StopPlaybackAndReset();
static void UpdateRenderFps();
static void UpdatePipelineStatsFromFrame(const AVFrame *frame);
static void RenderImGui();
static bool OpenMediaAndPlay(const char *source, const char *error_message);
static bool OpenFileDialogAndPlay();
static bool OpenUrlAndPlay();
[[noreturn]] static void DoExit();
static void ToggleFullScreen();
static void HandlePlaybackFatalError();
static bool InitWindow();
static void InitRenderer();
static void DisplayVideo();
static void MainLoop();
static void InitImGui();
static void ShutdownImGui();

static void OnFrameSizeChanged(void *opaque, int width, int height, AVRational sar)
{
    (void)opaque;
    video_renderer_set_default_window_size(&g_video_renderer_ctx,
                                           g_video_renderer_ctx.default_width,
                                           g_video_renderer_ctx.default_height,
                                           width, height, sar);
}

static void win32_on_client_resize(WPARAM size_type, int client_w, int client_h)
{
    if (!g_video_renderer_ctx.device || size_type == SIZE_MINIMIZED)
        return;
    video_renderer_resize(&g_video_renderer_ctx, client_w, client_h);
    if (g_player)
        ffplayer_handle_window_size_changed(g_player, client_w, client_h);
    ffplayer_request_refresh(g_player);
}

static void win32_request_quit()
{
    g_quit_requested = true;
}

static void ui_player_request_refresh()
{
    ffplayer_request_refresh(g_player);
}

static void ui_note_mouse_activity_show_cursor()
{
    if (g_cursor_hidden) {
        ShowCursor(TRUE);
        g_cursor_hidden = 0;
    }
    g_cursor_last_active = std::chrono::steady_clock::now();
}

[[noreturn]] static void ui_quit_application()
{
    DoExit();
}

namespace {

void DispatchKeyDown(WPARAM wParam)
{
    if (wParam == VK_ESCAPE || wParam == 'Q') {
        ui_quit_application();
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
        ui_player_request_refresh();
        return;
    }

    FFPlayer *pl = g_player;
    if (!ffplayer_is_open(pl) || !ffplayer_is_video_open(pl))
        return;

    double incr;
    switch (wParam) {
    case 'P':
    case VK_SPACE:
        ffplayer_toggle_pause(pl);
        break;
    case 'M':
        ffplayer_toggle_mute(pl);
        break;
    case '0':
    case VK_MULTIPLY:
        ffplayer_adjust_volume_step(pl, 1, FFPLAYER_VOLUME_STEP);
        break;
    case '9':
    case VK_DIVIDE:
        ffplayer_adjust_volume_step(pl, -1, FFPLAYER_VOLUME_STEP);
        break;
    case 'S':
        ffplayer_step_frame(pl);
        break;
    case 'A':
        ffplayer_cycle_audio_track(pl);
        break;
    case 'V':
        ffplayer_cycle_video_track(pl);
        break;
    case 'C':
        ffplayer_cycle_all_tracks(pl);
        break;
    case 'T':
        ffplayer_cycle_subtitle_track(pl);
        break;
    case 'W':
        ffplayer_toggle_audio_display(pl);
        break;
    case VK_PRIOR:
        if (!ffplayer_has_chapters(pl)) {
            incr = 600.0;
            goto do_seek;
        }
        ffplayer_seek_chapter(pl, 1);
        break;
    case VK_NEXT:
        if (!ffplayer_has_chapters(pl)) {
            incr = -600.0;
            goto do_seek;
        }
        ffplayer_seek_chapter(pl, -1);
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
        ffplayer_seek_relative(pl, incr);
        break;
    }
}

} /* namespace */

LRESULT CALLBACK ApplicationWin32WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
        return true;

    (void)hwnd;

    switch (msg) {
    case WM_SIZE:
        win32_on_client_resize(wParam, LOWORD(lParam), HIWORD(lParam));
        return 0;
    case WM_KEYDOWN:
        DispatchKeyDown(wParam);
        return 0;
    case WM_LBUTTONDOWN: {
        if (ImGui::GetIO().WantCaptureMouse)
            break;
        static std::optional<std::chrono::steady_clock::time_point> last_click;
        const auto now = std::chrono::steady_clock::now();
        if (last_click && (now - *last_click <= 500ms)) {
            ToggleFullScreen();
            ui_player_request_refresh();
            last_click.reset();
        } else {
            last_click = now;
        }
        ui_note_mouse_activity_show_cursor();
        return 0;
    }
    case WM_MOUSEMOVE:
        ui_note_mouse_activity_show_cursor();
        return 0;
    case WM_CLOSE:
    case WM_DESTROY:
        win32_request_quit();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static bool InitWindow()
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
    g_hwnd = CreateWindowExW(0, L"FFPlayGuiD3D11", L"ffplay-gui",
                             WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT,
                             rc.right - rc.left, rc.bottom - rc.top,
                             nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
    if (!g_hwnd)
        return false;

    return true;
}

static void InitRenderer()
{
    g_video_renderer_ctx.default_width = kInitialDefaultWidth;
    g_video_renderer_ctx.default_height = kInitialDefaultHeight;
    if (video_renderer_init(&g_video_renderer_ctx, g_hwnd) < 0) {
        fprintf(stderr, "Failed to initialize renderer\n");
        DoExit();
    }
}

static void InitImGui()
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImFontAtlas *fonts = io.Fonts;
    constexpr float kUIFontPx = 13.0f;
    ImFontConfig base_font_cfg{};
    base_font_cfg.SizePixels = kUIFontPx;
    fonts->AddFontDefault(&base_font_cfg);

    auto merge_cjk_font = [&](const char *relative_font_file, float size_px) -> bool {
        char pattern[MAX_PATH];
        char expanded[MAX_PATH];
        snprintf(pattern, sizeof(pattern), "%%WINDIR%%\\Fonts\\%s", relative_font_file);
        DWORD n = ExpandEnvironmentStringsA(pattern, expanded, (DWORD)sizeof(expanded));
        if (n == 0 || n >= sizeof(expanded))
            return false;
        if (GetFileAttributesA(expanded) == INVALID_FILE_ATTRIBUTES)
            return false;
        ImFontConfig cfg{};
        cfg.MergeMode = true;
        cfg.PixelSnapH = true;
        ImFont *loaded =
            fonts->AddFontFromFileTTF(expanded, size_px, &cfg, fonts->GetGlyphRangesChineseSimplifiedCommon());
        return loaded != nullptr;
    };
    static const char *kWindowsCjkFonts[] = {"msyh.ttc", "msyhl.ttc", "simhei.ttf", "simsun.ttc"};
    for (const char *rel : kWindowsCjkFonts) {
        if (merge_cjk_font(rel, kUIFontPx))
            break;
    }

    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_video_renderer_ctx.device, g_video_renderer_ctx.context);
    g_imgui_ready = true;
}

static void ShutdownImGui()
{
    if (!g_imgui_ready)
        return;
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    g_imgui_ready = false;
}

static float UiDrawMainMenuBar(bool *show_statistics_window)
{
    if (!ImGui::BeginMainMenuBar())
        return 0.0f;

    const float bottom = ImGui::GetWindowPos().y + ImGui::GetWindowHeight();

    if (ImGui::BeginMenu("Tools")) {
        ImGui::MenuItem("Statistics", nullptr, show_statistics_window);
        ImGui::EndMenu();
    }

    ImGui::EndMainMenuBar();
    return bottom;
}

static void UiDrawStartupDefaults(float main_menu_bar_bottom)
{
    if (ffplayer_is_open(g_player))
        return;

    const float top_bar_h = 48.0f;
    ImGui::SetNextWindowPos(ImVec2(0.0f, main_menu_bar_bottom));
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetIO().DisplaySize.x, top_bar_h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.03f, 0.04f, 0.08f, 0.85f));
    ImGui::Begin("StartupDefaults", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("-infbuf");
    ImGui::SameLine();
    ImGui::TextUnformatted("=");
    ImGui::SameLine();
    int inf_idx;
    int &infbuf = g_startup_infinite_buffer;
    if (infbuf < 0)
        inf_idx = 0;
    else if (infbuf == 0)
        inf_idx = 1;
    else
        inf_idx = 2;
    static const char *inf_labels[] = {
        "auto (-1)",
        "off: (0)",
        "on: (1)",
    };
    const ImGuiStyle &st = ImGui::GetStyle();
    float inf_combo_w = 0.0f;
    for (int i = 0; i < IM_ARRAYSIZE(inf_labels); ++i)
        inf_combo_w = std::max(inf_combo_w, ImGui::CalcTextSize(inf_labels[i], nullptr, true).x);
    inf_combo_w += st.FramePadding.x * 2.0f + ImGui::GetFrameHeight();
    inf_combo_w = std::min(inf_combo_w, ImGui::GetContentRegionAvail().x);
    ImGui::SetNextItemWidth(inf_combo_w);
    if (ImGui::Combo("##infbuf", &inf_idx, inf_labels, IM_ARRAYSIZE(inf_labels))) {
        infbuf = (inf_idx == 0) ? -1 : (inf_idx == 1) ? 0 : 1;
        ffplayer_set_infinite_buffer(g_player, infbuf);
    }

    ImGui::SameLine(0.0f, 24.0f);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("-sync");
    ImGui::SameLine();
    ImGui::TextUnformatted("=");
    ImGui::SameLine();
    int sync_idx = g_startup_av_sync_type;
    if (sync_idx < FFPLAYER_AV_SYNC_AUDIO_MASTER || sync_idx > FFPLAYER_AV_SYNC_EXTERNAL_CLOCK)
        sync_idx = FFPLAYER_AV_SYNC_AUDIO_MASTER;
    static const char *sync_labels[] = {
        "audio",
        "video",
        "ext",
    };
    float sync_combo_w = 0.0f;
    for (int i = 0; i < IM_ARRAYSIZE(sync_labels); ++i)
        sync_combo_w = std::max(sync_combo_w, ImGui::CalcTextSize(sync_labels[i], nullptr, true).x);
    sync_combo_w += st.FramePadding.x * 2.0f + ImGui::GetFrameHeight();
    sync_combo_w = std::min(sync_combo_w, ImGui::GetContentRegionAvail().x);
    ImGui::SetNextItemWidth(sync_combo_w);
    if (ImGui::Combo("##sync", &sync_idx, sync_labels, IM_ARRAYSIZE(sync_labels))) {
        g_startup_av_sync_type = sync_idx;
        ffplayer_set_av_sync_type(g_player, g_startup_av_sync_type);
    }

    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

static void UiDrawStatisticsWindow()
{
    if (!g_show_statistics_window)
        return;

    FFPlayer *pl = g_player;
    const bool decoder_hw = ffplayer_is_video_decoder_hardware(pl) != 0;
    const bool hw_fallback = ffplayer_has_video_hw_fallback(pl) != 0;
    const char *pipeline_mode = "No Frame";
    if (g_stats_has_video_frame)
        pipeline_mode = g_stats_pipeline_zero_copy ? "Zero-Copy (D3D11)" : "Software Upload";
    const char *pix_fmt_name = g_stats_has_video_frame ? av_get_pix_fmt_name(g_stats_video_pix_fmt) : nullptr;
    if (!pix_fmt_name)
        pix_fmt_name = "N/A";

    ImGui::SetNextWindowSize(ImVec2(360.0f, 190.0f), ImGuiCond_FirstUseEver);
    bool &show = g_show_statistics_window;
    if (ImGui::Begin("Statistics", &show)) {
        ImGui::Text("Render FPS: %.2f", g_render_fps);
        ImGui::Text("Frame Time: %.2f ms", g_render_frame_time_ms);
        ImGui::Text("Pipeline Mode: %s", pipeline_mode);
        ImGui::Text("Video PixFmt: %s", pix_fmt_name);
        ImGui::Text("Decoder Mode: %s", decoder_hw ? "HW" : "SW");
        ImGui::Text("HW Fallback: %s", hw_fallback ? "Triggered" : "No");
    }
    ImGui::End();
}

static bool UiDrawPlayerControls()
{
    ImGuiIO &io = ImGui::GetIO();
    constexpr float bar_height = 52.0f;
    constexpr float margin = 12.0f;
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
    bool stop_requested = false;

    FFPlayer *pl = g_player;

    if (ffplayer_is_open(pl)) {
        duration_sec = ffplayer_get_duration(pl);
        has_known_duration = duration_sec > 0;
        can_seek = ffplayer_can_seek(pl);
        can_approx_seek = can_seek && !has_known_duration;

        if (has_known_duration) {
            current_sec = ffplayer_get_position(pl);
            current_sec = std::max(0.0, std::min(current_sec, duration_sec));
            progress = duration_sec > 0.0 ? (float)(current_sec / duration_sec) : 0.0f;

            if (ffplayer_is_eof(pl) && progress > 0.90f)
                progress = 1.0f;

            if (!std::isfinite(progress))
                progress = 0.0f;
            using_stable_progress = true;
            time_text = FormatDuration(current_sec) + " / " + FormatDuration(duration_sec);
        } else if (can_approx_seek) {
            float bp = ffplayer_get_byte_progress(pl);
            if (bp >= 0.0f)
                progress = bp;
            time_text = "Unknown duration | Approx seek";
        } else {
            time_text = "Unknown duration";
        }

        play_text = ffplayer_is_paused(pl) ? "Play " : "Pause";
    }
    if (!ffplayer_is_open(pl))
        g_stable_progress_ready = false;

    float &pending = g_pending_seek_ratio;
    float &stable_r = g_stable_progress_ratio;
    bool &stable_ok = g_stable_progress_ready;

    if (using_stable_progress && pending < 0.0f) {
        if (!stable_ok) {
            stable_r = progress;
            stable_ok = true;
        } else {
            float delta = progress - stable_r;
            if (delta >= -0.003f) {
                stable_r = std::max(stable_r, progress);
            } else if (delta <= -0.08f) {
                stable_r = progress;
            }
        }
        progress = stable_r;
    }
    if (!std::isfinite(progress))
        progress = 0.0f;
    if (ffplayer_is_open(pl))
        volume_percent = 100.0f * (float)ffplayer_get_volume(pl) / (float)SDL_MIX_MAXVOLUME;

    ImGui::SetNextWindowPos(ImVec2(0.0f, io.DisplaySize.y - bar_height));
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, bar_height));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.03f, 0.04f, 0.08f, 0.85f));
    ImGui::Begin("PlayerControls", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);

    const int url_cap = static_cast<int>(sizeof(g_open_url_input));

    if (!ffplayer_is_open(pl)) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("No media loaded");
        ImGui::SameLine();
        if (ImGui::Button("Open file..."))
            OpenFileDialogAndPlay();
        ImGui::SameLine();
        bool open_url_clicked = ImGui::Button("Open URL");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(std::max(220.0f, ImGui::GetContentRegionAvail().x));
        bool url_entered = ImGui::InputTextWithHint(
            "##open_url",
            "https://... / rtsp://... / file path",
            g_open_url_input,
            url_cap,
            ImGuiInputTextFlags_EnterReturnsTrue);
        if (open_url_clicked || url_entered)
            OpenUrlAndPlay();
    } else {
        float avail_w = ImGui::GetContentRegionAvail().x;
        float play_w = ImGui::CalcTextSize(play_text.c_str()).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        float stop_w = ImGui::CalcTextSize("Stop").x + ImGui::GetStyle().FramePadding.x * 2.0f;
        float time_w = ImGui::CalcTextSize(time_text.c_str()).x;
        float volume_w = std::min(110.0f, std::max(72.0f, avail_w * 0.22f));
        constexpr float min_timeline_w = 80.0f;
        bool hide_time_text = false;
        float needed_w = play_w + margin + stop_w + margin + time_w + margin + min_timeline_w + margin + volume_w;

        if (avail_w < needed_w) {
            hide_time_text = true;
            needed_w = play_w + margin + stop_w + margin + min_timeline_w + margin + volume_w;
        }
        if (avail_w < needed_w) {
            volume_w = std::max(58.0f, avail_w * 0.18f);
        }

        float timeline_w = std::max(
            min_timeline_w,
            avail_w - play_w - margin - stop_w - margin - (hide_time_text ? 0.0f : (time_w + margin)) - margin - volume_w);

        if (ImGui::Button(play_text.c_str()))
            ffplayer_toggle_pause(pl);
        ImGui::SameLine(0.0f, margin);
        if (ImGui::Button("Stop"))
            stop_requested = true;
        if (!stop_requested && !hide_time_text) {
            ImGui::SameLine(0.0f, margin);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(time_text.c_str());
        }
        if (!stop_requested) {
            ImGui::SameLine(0.0f, margin);
            ImGui::SetNextItemWidth(timeline_w);
            float slider_value = pending >= 0.0f ? pending : progress;
            if (!can_seek)
                ImGui::BeginDisabled();
            if (ImGui::SliderFloat("##timeline", &slider_value, 0.0f, 1.0f, "")) {
                pending = slider_value;
                if (ImGui::IsItemActive()) {
                    const auto now = std::chrono::steady_clock::now();
                    if (!g_last_drag_seek || now - *g_last_drag_seek >= 40ms) {
                        SeekToRatio(pending);
                        g_last_drag_seek = now;
                    }
                }
            }
            if (pending >= 0.0f && ImGui::IsItemDeactivatedAfterEdit()) {
                SeekToRatio(pending);
                stable_r = pending;
                stable_ok = true;
                pending = -1.0f;
                g_last_drag_seek.reset();
            }
            if (!can_seek)
                ImGui::EndDisabled();
            ImGui::SameLine(0.0f, margin);
            ImGui::SetNextItemWidth(volume_w);
            const char *volume_fmt = volume_w >= 72.0f ? "%.0f%%" : "";
            if (ImGui::SliderFloat("##volume", &volume_percent, 0.0f, 100.0f, volume_fmt)) {
                ffplayer_set_volume(pl, (int)((volume_percent / 100.0f) * SDL_MIX_MAXVOLUME));
            }
        }
    }

    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);

    return stop_requested;
}

static void RefreshWindowTitle()
{
    if (ffplayer_is_open(g_player)) {
        const char *u8 = ffplayer_get_media_title(g_player);
        if (u8 && u8[0])
            SetWindowTitleUtf8(g_hwnd, u8);
        else
            SetWindowTitleUtf8(g_hwnd, "ffplay-gui");
        return;
    }
    SetWindowTitleUtf8(g_hwnd, "ffplay-gui - No media loaded");
}

static void ToggleFullScreen()
{
    DWORD style = GetWindowLong(g_hwnd, GWL_STYLE);
    if (!g_is_full_screen) {
        MONITORINFO mi = { sizeof(mi) };
        if (GetWindowPlacement(g_hwnd, &g_wp_before_fullscreen) &&
            GetMonitorInfo(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
            SetWindowLong(g_hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
            SetWindowPos(g_hwnd, HWND_TOP,
                         mi.rcMonitor.left, mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        }
    } else {
        SetWindowLong(g_hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(g_hwnd, &g_wp_before_fullscreen);
        SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    g_is_full_screen = !g_is_full_screen;
}

static void SeekToRatio(float ratio)
{
    ffplayer_seek_to_ratio(g_player, ratio);
}

static void ResetSeekBarUiState()
{
    g_pending_seek_ratio = -1.0f;
    g_last_drag_seek.reset();
    g_stable_progress_ratio = 0.0f;
    g_stable_progress_ready = false;
}

static void ApplyVideoRendererSettingsToPlayer()
{
    enum AVPixelFormat pix_fmts[32];
    int nb = video_renderer_get_supported_pixel_formats(&g_video_renderer_ctx, pix_fmts, 32);
    ffplayer_set_supported_pixel_formats(g_player, pix_fmts, nb);
    if (AVBufferRef *hw = video_renderer_get_hw_device_ctx(&g_video_renderer_ctx))
        ffplayer_set_hw_device_ctx(g_player, hw);
    ffplayer_set_frame_size_callback(g_player, OnFrameSizeChanged, nullptr);
    ffplayer_set_infinite_buffer(g_player, g_startup_infinite_buffer);
    ffplayer_set_av_sync_type(g_player, g_startup_av_sync_type);
}

static void HandlePlaybackFatalError()
{
    ffplayer_close(g_player);
    video_renderer_cleanup_textures(&g_video_renderer_ctx);
    g_video_open_done = 0;
    ResetSeekBarUiState();
    RefreshWindowTitle();
    MessageBoxA(g_hwnd,
                "The media could not be opened or played.\nCheck the URL or network, or see the FFmpeg log.",
                "ffplay-gui",
                MB_OK | MB_ICONWARNING);
}

static void StopPlaybackAndReset()
{
    video_renderer_cleanup_textures(&g_video_renderer_ctx);
    ffplayer_free(&g_player);
    g_player = ffplayer_create(&g_audio_device);
    if (!g_player) {
        MessageBoxA(g_hwnd, "Failed to recreate player.", "Stop failed", MB_ICONERROR);
        DoExit();
    }

    ApplyVideoRendererSettingsToPlayer();

    g_video_open_done = 0;
    ResetSeekBarUiState();
    g_stats_has_video_frame = false;
    g_stats_pipeline_zero_copy = false;
    g_stats_video_pix_fmt = AV_PIX_FMT_NONE;
    RefreshWindowTitle();
}

static void UpdateRenderFps()
{
    const auto now = std::chrono::steady_clock::now();
    if (!g_render_fps_sample) {
        g_render_fps_sample = now;
        g_render_fps_frame_count = 0;
        g_render_fps = 0.0f;
        g_render_frame_time_ms = 0.0f;
        return;
    }

    ++g_render_fps_frame_count;
    const auto elapsed = now - *g_render_fps_sample;
    if (elapsed >= 500ms) {
        const float elapsed_sec = std::chrono::duration<float>(elapsed).count();
        g_render_fps = elapsed_sec > 0.0f ? (float)g_render_fps_frame_count / elapsed_sec : 0.0f;
        g_render_frame_time_ms = g_render_fps > 0.0f ? (1000.0f / g_render_fps) : 0.0f;
        g_render_fps_frame_count = 0;
        g_render_fps_sample = now;
    }
}

static void UpdatePipelineStatsFromFrame(const AVFrame *frame)
{
    if (!frame) {
        g_stats_has_video_frame = false;
        g_stats_pipeline_zero_copy = false;
        g_stats_video_pix_fmt = AV_PIX_FMT_NONE;
        return;
    }
    g_stats_has_video_frame = true;
    g_stats_video_pix_fmt = (AVPixelFormat)frame->format;
    const char *pix_fmt_name = av_get_pix_fmt_name(g_stats_video_pix_fmt);
    g_stats_pipeline_zero_copy = pix_fmt_name && strcmp(pix_fmt_name, "d3d11") == 0;
}

static void DisplayVideo()
{
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    int win_w = rc.right - rc.left;
    int win_h = rc.bottom - rc.top;

    if (!g_video_open_done) {
        video_renderer_open(&g_video_renderer_ctx, &win_w, &win_h);
        ffplayer_set_window_size(g_player, win_w, win_h);
        g_video_open_done = 1;
    }

    enum FFPlayerShowMode mode = ffplayer_get_show_mode(g_player);
    if (mode == FFPLAYER_SHOW_MODE_VIDEO) {
        AVFrame *frame = ffplayer_get_video_frame(g_player);
        AVSubtitle *subtitle = ffplayer_get_subtitle(g_player);
        if (frame) {
            UpdatePipelineStatsFromFrame(frame);
            video_renderer_draw_video(&g_video_renderer_ctx, frame, subtitle, 0, 0, win_w, win_h);
        } else {
            g_stats_has_video_frame = false;
        }
    } else {
        g_stats_has_video_frame = false;
    }
}

static void RenderImGui()
{
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    UpdateRenderFps();

    const float main_menu_bar_bottom = UiDrawMainMenuBar(&g_show_statistics_window);
    UiDrawStartupDefaults(main_menu_bar_bottom);
    UiDrawStatisticsWindow();

    const bool stop_requested = UiDrawPlayerControls();

    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    if (stop_requested)
        StopPlaybackAndReset();
}

static bool OpenFileDialogAndPlay()
{
    if (g_open_dialog_active)
        return false;
    g_open_dialog_active = true;

    std::string path;
    if (!PickMediaFileUtf8(path)) {
        g_open_dialog_active = false;
        return false;
    }

    bool ok = OpenMediaAndPlay(path.c_str(), "Failed to open selected media file.");
    g_open_dialog_active = false;
    return ok;
}

static bool OpenUrlAndPlay()
{
    std::string url(g_open_url_input);
    size_t begin = url.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos)
        return false;
    size_t end = url.find_last_not_of(" \t\r\n");
    url = url.substr(begin, end - begin + 1);
    if (!OpenMediaAndPlay(url.c_str(), "Failed to open media URL."))
        return false;
    g_open_url_input[0] = '\0';
    return true;
}

static bool OpenMediaAndPlay(const char *source, const char *error_message)
{
    if (!source || !source[0])
        return false;

    ffplayer_close(g_player);
    video_renderer_cleanup_textures(&g_video_renderer_ctx);
    g_video_open_done = 0;
    g_stable_progress_ready = false;

    if (ffplayer_open(g_player, source) < 0) {
        MessageBoxA(g_hwnd, error_message, "Open failed", MB_ICONERROR);
        RefreshWindowTitle();
        return false;
    }
    ffplayer_toggle_pause(g_player);
    ffplayer_request_refresh(g_player);
    g_stable_progress_ratio = 0.0f;
    RefreshWindowTitle();
    return true;
}

[[noreturn]] static void DoExit()
{
    video_renderer_cleanup_textures(&g_video_renderer_ctx);
    ffplayer_free(&g_player);
    ShutdownImGui();
    video_renderer_shutdown(&g_video_renderer_ctx);
    if (g_hwnd) {
        DestroyWindow(g_hwnd);
        g_hwnd = nullptr;
    }
    avformat_network_deinit();
    SDL_Quit();
    exit(0);
}

static void MainLoop()
{
    MSG msg;
    for (;;) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                g_quit_requested = true;
        }

        if (g_quit_requested)
            DoExit();

        if (ffplayer_has_quit_request(g_player)) {
            HandlePlaybackFatalError();
            continue;
        }

        if (!g_cursor_hidden && g_cursor_last_active &&
            std::chrono::steady_clock::now() - *g_cursor_last_active > std::chrono::microseconds(FFPLAYER_CURSOR_HIDE_DELAY)) {
            ShowCursor(FALSE);
            g_cursor_hidden = 1;
        }

        double remaining_time = FFPLAYER_REFRESH_RATE;
        if (ffplayer_needs_refresh(g_player))
            ffplayer_refresh(g_player, &remaining_time);

        video_renderer_clear(&g_video_renderer_ctx);
        if (ffplayer_is_open(g_player))
            DisplayVideo();
        RenderImGui();
        video_renderer_present(&g_video_renderer_ctx);

        if (remaining_time > 0.0)
            std::this_thread::sleep_for(std::chrono::duration<double>(remaining_time));
    }
}

#undef main

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
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
    ShowWindow(g_hwnd, SW_SHOWDEFAULT);
    UpdateWindow(g_hwnd);
    SetWindowTitleUtf8(g_hwnd, "ffplay-gui");

    g_player = ffplayer_create(&g_audio_device);
    ApplyVideoRendererSettingsToPlayer();
    InitImGui();

    MainLoop();
    return 0;
}

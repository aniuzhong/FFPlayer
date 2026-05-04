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
#include "utils/utf8.h"
#include "ffplayer/audio_device.h"
#include "ffplayer/ffplayer.h"
}

#include "render/video_renderer.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {

// -----------------------------------------------------------------------------
// Constants
// -----------------------------------------------------------------------------

constexpr int kInitialDefaultWidth = 640;
constexpr int kInitialDefaultHeight = 480;

// -----------------------------------------------------------------------------
// Small utilities (UTF-8 title, time text, file dialog)
// -----------------------------------------------------------------------------

void set_window_title_utf8(HWND hwnd, const char *text)
{
    if (!hwnd)
        return;
    if (!text || !text[0]) {
        SetWindowTextW(hwnd, L"ffplay-gui");
        return;
    }

    /* OBS util/utf8.c utf8_to_wchar (Windows path): UTF-8 BOM strip + CP_UTF8 */
    std::wstring w;
    const size_t nc = utf8_to_wchar(text, 0, nullptr, 0, 0);
    if (nc != 0) {
        w.resize(nc);
        if (utf8_to_wchar(text, 0, w.data(), nc, 0) != 0) {
            if (!w.empty() && w.back() == L'\0')
                w.pop_back();
        } else {
            w.clear();
        }
    }

    SetWindowTextW(hwnd, !w.empty() ? w.c_str() : L"ffplay-gui");
}

std::string format_duration_for_ui(double seconds)
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

bool pick_media_file_utf8(std::string &out_utf8_path)
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

// -----------------------------------------------------------------------------
// Application state (single translation unit)
// -----------------------------------------------------------------------------

/* Win32 window & cursor */
HWND g_hwnd = nullptr;
std::optional<std::chrono::steady_clock::time_point> g_cursor_last_active;
int g_cursor_hidden = 0;
int g_is_full_screen = 0;
WINDOWPLACEMENT g_wp_before_fullscreen = {};
bool g_quit_requested = false;

/* FFmpeg player & renderer */
AudioDevice g_audio_device = {};
FFPlayer *g_player = nullptr;
VideoRenderer g_video_renderer_ctx = {};
int g_video_open_done = 0;

/* ImGui lifecycle & overlays */
bool g_imgui_ready = false;
bool g_open_dialog_active = false;
bool g_show_statistics_window = false;

/* Seek bar / timeline UI */
float g_pending_seek_ratio = -1.0f;
std::optional<std::chrono::steady_clock::time_point> g_last_drag_seek;
float g_stable_progress_ratio = 0.0f;
bool g_stable_progress_ready = false;

/* Statistics panel snapshot */
float g_render_fps = 0.0f;
float g_render_frame_time_ms = 0.0f;
int g_render_fps_frame_count = 0;
std::optional<std::chrono::steady_clock::time_point> g_render_fps_sample;
bool g_stats_has_video_frame = false;
bool g_stats_pipeline_zero_copy = false;
AVPixelFormat g_stats_video_pix_fmt = AV_PIX_FMT_NONE;

/* Open URL field (idle UI) */
char g_open_url_input[1024] = {};

/* Defaults applied when opening media */
int g_startup_infinite_buffer = -1;
int g_startup_av_sync_type = FFPLAYER_AV_SYNC_AUDIO_MASTER;

// -----------------------------------------------------------------------------
// Forward declarations
// -----------------------------------------------------------------------------

void update_main_window_title();
void stop_playback_and_recreate_player();
void update_fps_overlay_stats();
void update_pipeline_stats_from_av_frame(const AVFrame *frame);
void render_imgui_frame();
bool open_media_source_and_play(const char *source, const char *error_message);
bool open_media_via_file_dialog();
bool open_media_from_url_field();
void shutdown_application_and_exit();
void toggle_full_screen();
void handle_fatal_playback_error();
bool create_main_window();
void init_gui_video_renderer();
void draw_current_video_frame();
void run_application_main_loop();
void init_imgui_dx11();
void shutdown_imgui_dx11();

// -----------------------------------------------------------------------------
// FFmpeg / player callbacks
// -----------------------------------------------------------------------------

void on_player_video_layout_changed(void *opaque, int width, int height, AVRational sar)
{
    (void)opaque;
    video_renderer_set_default_window_size(&g_video_renderer_ctx,
                                           g_video_renderer_ctx.default_width,
                                           g_video_renderer_ctx.default_height,
                                           width, height, sar);
}

// -----------------------------------------------------------------------------
// Win32: sizing, quit, raw input hooks, window procedure
// -----------------------------------------------------------------------------

void win32_handle_client_resize(WPARAM size_type, int client_w, int client_h)
{
    if (!g_video_renderer_ctx.device || size_type == SIZE_MINIMIZED)
        return;
    video_renderer_resize(&g_video_renderer_ctx, client_w, client_h);
    if (g_player)
        ffplayer_handle_window_size_changed(g_player, client_w, client_h);
    ffplayer_request_refresh(g_player);
}

void ui_show_cursor_on_input_activity()
{
    if (g_cursor_hidden) {
        ShowCursor(TRUE);
        g_cursor_hidden = 0;
    }
    g_cursor_last_active = std::chrono::steady_clock::now();
}

LRESULT CALLBACK win32_wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
        return true;

    (void)hwnd;

    switch (msg) {
    case WM_SIZE:
        win32_handle_client_resize(wParam, LOWORD(lParam), HIWORD(lParam));
        return 0;
    case WM_LBUTTONDOWN: {
        if (ImGui::GetIO().WantCaptureMouse)
            break;
        static std::optional<std::chrono::steady_clock::time_point> last_click;
        const auto now = std::chrono::steady_clock::now();
        if (last_click && (now - *last_click <= 500ms)) {
            toggle_full_screen();
            ffplayer_request_refresh(g_player);
            last_click.reset();
        } else {
            last_click = now;
        }
        ui_show_cursor_on_input_activity();
        return 0;
    }
    case WM_MOUSEMOVE:
        ui_show_cursor_on_input_activity();
        return 0;
    case WM_CLOSE:
    case WM_DESTROY:
        g_quit_requested = true;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// -----------------------------------------------------------------------------
// Window, D3D11 renderer, ImGui init / shutdown
// -----------------------------------------------------------------------------

bool create_main_window()
{
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = win32_wnd_proc;
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

void init_gui_video_renderer()
{
    g_video_renderer_ctx.default_width = kInitialDefaultWidth;
    g_video_renderer_ctx.default_height = kInitialDefaultHeight;
    if (video_renderer_init(&g_video_renderer_ctx, g_hwnd) < 0) {
        fprintf(stderr, "Failed to initialize renderer\n");
        shutdown_application_and_exit();
    }
}

void init_imgui_dx11()
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

void shutdown_imgui_dx11()
{
    if (!g_imgui_ready)
        return;
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    g_imgui_ready = false;
}

// -----------------------------------------------------------------------------
// ImGui: menu bar, startup options, statistics, player chrome
// -----------------------------------------------------------------------------

float ui_draw_main_menu_bar(bool *show_statistics_window)
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

void ui_draw_startup_defaults_panel(float main_menu_bar_bottom)
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

void ui_draw_statistics_window()
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

bool ui_draw_player_controls_bar()
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
            time_text = format_duration_for_ui(current_sec) + " / " + format_duration_for_ui(duration_sec);
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
            open_media_via_file_dialog();
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
            open_media_from_url_field();
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
                        ffplayer_seek_to_ratio(g_player, pending);
                        g_last_drag_seek = now;
                    }
                }
            }
            if (pending >= 0.0f && ImGui::IsItemDeactivatedAfterEdit()) {
                ffplayer_seek_to_ratio(g_player, pending);
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

// -----------------------------------------------------------------------------
// Window title & fullscreen
// -----------------------------------------------------------------------------

void update_main_window_title()
{
    if (ffplayer_is_open(g_player)) {
        const char *u8 = ffplayer_get_media_title(g_player);
        if (u8 && u8[0])
            set_window_title_utf8(g_hwnd, u8);
        else
            set_window_title_utf8(g_hwnd, "ffplay-gui");
        return;
    }
    set_window_title_utf8(g_hwnd, "ffplay-gui - No media loaded");
}

void toggle_full_screen()
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

// -----------------------------------------------------------------------------
// Player <-> renderer wiring & seek-bar reset
// -----------------------------------------------------------------------------

void reset_seek_bar_ui_state()
{
    g_pending_seek_ratio = -1.0f;
    g_last_drag_seek.reset();
    g_stable_progress_ratio = 0.0f;
    g_stable_progress_ready = false;
}

void apply_renderer_settings_to_player()
{
    enum AVPixelFormat pix_fmts[32];
    int nb = video_renderer_get_supported_pixel_formats(&g_video_renderer_ctx, pix_fmts, 32);
    ffplayer_set_supported_pixel_formats(g_player, pix_fmts, nb);
    if (AVBufferRef *hw = video_renderer_get_hw_device_ctx(&g_video_renderer_ctx))
        ffplayer_set_hw_device_ctx(g_player, hw);
    ffplayer_set_frame_size_callback(g_player, on_player_video_layout_changed, nullptr);
    ffplayer_set_infinite_buffer(g_player, g_startup_infinite_buffer);
    ffplayer_set_av_sync_type(g_player, g_startup_av_sync_type);
}

void handle_fatal_playback_error()
{
    ffplayer_close(g_player);
    video_renderer_cleanup_textures(&g_video_renderer_ctx);
    g_video_open_done = 0;
    reset_seek_bar_ui_state();
    update_main_window_title();
    MessageBoxA(g_hwnd,
                "The media could not be opened or played.\nCheck the URL or network, or see the FFmpeg log.",
                "ffplay-gui",
                MB_OK | MB_ICONWARNING);
}

void stop_playback_and_recreate_player()
{
    video_renderer_cleanup_textures(&g_video_renderer_ctx);
    ffplayer_free(&g_player);
    g_player = ffplayer_create(&g_audio_device);
    if (!g_player) {
        MessageBoxA(g_hwnd, "Failed to recreate player.", "Stop failed", MB_ICONERROR);
        shutdown_application_and_exit();
    }

    apply_renderer_settings_to_player();

    g_video_open_done = 0;
    reset_seek_bar_ui_state();
    g_stats_has_video_frame = false;
    g_stats_pipeline_zero_copy = false;
    g_stats_video_pix_fmt = AV_PIX_FMT_NONE;
    update_main_window_title();
}

// -----------------------------------------------------------------------------
// Overlay metrics & video presentation
// -----------------------------------------------------------------------------

void update_fps_overlay_stats()
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

void update_pipeline_stats_from_av_frame(const AVFrame *frame)
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

void draw_current_video_frame()
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
            update_pipeline_stats_from_av_frame(frame);
            video_renderer_draw_video(&g_video_renderer_ctx, frame, subtitle, 0, 0, win_w, win_h);
        } else {
            g_stats_has_video_frame = false;
        }
    } else {
        g_stats_has_video_frame = false;
    }
}

void render_imgui_frame()
{
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    update_fps_overlay_stats();

    const float main_menu_bar_bottom = ui_draw_main_menu_bar(&g_show_statistics_window);
    ui_draw_startup_defaults_panel(main_menu_bar_bottom);
    ui_draw_statistics_window();

    const bool stop_requested = ui_draw_player_controls_bar();

    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    if (stop_requested)
        stop_playback_and_recreate_player();
}

// -----------------------------------------------------------------------------
// Opening media & teardown
// -----------------------------------------------------------------------------

bool open_media_via_file_dialog()
{
    if (g_open_dialog_active)
        return false;
    g_open_dialog_active = true;

    std::string path;
    if (!pick_media_file_utf8(path)) {
        g_open_dialog_active = false;
        return false;
    }

    bool ok = open_media_source_and_play(path.c_str(), "Failed to open selected media file.");
    g_open_dialog_active = false;
    return ok;
}

bool open_media_from_url_field()
{
    std::string url(g_open_url_input);
    size_t begin = url.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos)
        return false;
    size_t end = url.find_last_not_of(" \t\r\n");
    url = url.substr(begin, end - begin + 1);
    if (!open_media_source_and_play(url.c_str(), "Failed to open media URL."))
        return false;
    g_open_url_input[0] = '\0';
    return true;
}

bool open_media_source_and_play(const char *source, const char *error_message)
{
    if (!source || !source[0])
        return false;

    ffplayer_close(g_player);
    video_renderer_cleanup_textures(&g_video_renderer_ctx);
    g_video_open_done = 0;
    g_stable_progress_ready = false;

    if (ffplayer_open(g_player, source) < 0) {
        MessageBoxA(g_hwnd, error_message, "Open failed", MB_ICONERROR);
        update_main_window_title();
        return false;
    }
    ffplayer_toggle_pause(g_player);
    ffplayer_request_refresh(g_player);
    g_stable_progress_ratio = 0.0f;
    update_main_window_title();
    return true;
}

void shutdown_application_and_exit()
{
    video_renderer_cleanup_textures(&g_video_renderer_ctx);
    ffplayer_free(&g_player);
    shutdown_imgui_dx11();
    video_renderer_shutdown(&g_video_renderer_ctx);
    if (g_hwnd) {
        DestroyWindow(g_hwnd);
        g_hwnd = nullptr;
    }
    avformat_network_deinit();
    SDL_Quit();
    exit(0);
}

void run_application_main_loop()
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
            shutdown_application_and_exit();

        if (ffplayer_has_quit_request(g_player)) {
            handle_fatal_playback_error();
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
            draw_current_video_frame();
        render_imgui_frame();
        video_renderer_present(&g_video_renderer_ctx);

        if (remaining_time > 0.0)
            std::this_thread::sleep_for(std::chrono::duration<double>(remaining_time));
    }
}

} // namespace

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    avdevice_register_all();
    avformat_network_init();

    if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
        fprintf(stderr, "Could not initialize SDL audio - %s\n", SDL_GetError());
        exit(1);
    }

    if (!create_main_window()) {
        fprintf(stderr, "Failed to create Win32 window\n");
        exit(1);
    }
    init_gui_video_renderer();
    ShowWindow(g_hwnd, SW_SHOWDEFAULT);
    UpdateWindow(g_hwnd);
    set_window_title_utf8(g_hwnd, "ffplay-gui");

    g_player = ffplayer_create(&g_audio_device);
    apply_renderer_settings_to_player();
    init_imgui_dx11();

    run_application_main_loop();
    return 0;
}

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
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_sdlrenderer2.h"
#include "spdlog/logger.h"
#include "spdlog/sinks/base_sink.h"
#include "spdlog/spdlog.h"

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#endif

#include "application.h"

extern "C" {
#include "audio_visualizer.h"
}

#define FF_QUIT_EVENT (SDL_USEREVENT + 2)

const char program_name[] = "ffplay";
const int program_birth_year = 2003;

static constexpr int kInitialDefaultWidth = 640;
static constexpr int kInitialDefaultHeight = 480;
static constexpr float kSeekIntervalSeconds = 10.0f;
static constexpr size_t kMaxLogLines = 2000;

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
    if (level <= AV_LOG_PANIC)
        return spdlog::level::critical;
    if (level <= AV_LOG_ERROR)
        return spdlog::level::err;
    if (level <= AV_LOG_WARNING)
        return spdlog::level::warn;
    if (level <= AV_LOG_INFO)
        return spdlog::level::info;
    if (level <= AV_LOG_VERBOSE)
        return spdlog::level::debug;
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

int Application::Execute()
{
    int flags;

    InitUiLogger();
    av_log_set_callback(UiAvLogCallback);
    av_log_set_flags(AV_LOG_SKIP_REPEATED);

    avdevice_register_all();
    avformat_network_init();

    signal(SIGINT , sigterm_handler);
    signal(SIGTERM, sigterm_handler);

    flags = SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER;
    if (!SDL_getenv("SDL_AUDIO_ALSA_SET_BUFFER_SIZE"))
        SDL_setenv("SDL_AUDIO_ALSA_SET_BUFFER_SIZE","1", 1);
    if (SDL_Init(flags)) {
        av_log(nullptr, AV_LOG_FATAL, "Could not initialize SDL - %s\n", SDL_GetError());
        av_log(nullptr, AV_LOG_FATAL, "(Did you set the DISPLAY variable?)\n");
        exit(1);
    }

    SDL_EventState(SDL_SYSWMEVENT, SDL_IGNORE);
    SDL_EventState(SDL_USEREVENT, SDL_IGNORE);

    InitWindowAndRenderer();
    ConfigureVideoRenderer();
    SDL_ShowWindow(window_);
    SDL_SetWindowTitle(window_, "ffplay-gui");

    player_ = ffplayer_create(&audio_device_);
    ffplayer_set_renderer_info(player_, &video_renderer_ctx_.renderer_info);
    ffplayer_set_frame_size_callback(player_, Application::OnFrameSizeChanged, this);
    InitImGui();

    EventLoop();
    return 0;
}

std::string Application::FormatDuration(double seconds)
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

void Application::InitImGui()
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui_ImplSDL2_InitForSDLRenderer(window_, renderer_);
    ImGui_ImplSDLRenderer2_Init(renderer_);
    imgui_ready_ = true;
}

void Application::ShutdownImGui()
{
    if (!imgui_ready_)
        return;
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    imgui_ready_ = false;
}

void Application::RefreshWindowTitle()
{
    if (ffplayer_is_open(player_)) {
        const char *title = ffplayer_get_media_title(player_);
        SDL_SetWindowTitle(window_, (title && title[0]) ? title : "ffplay-gui");
        return;
    }
    SDL_SetWindowTitle(window_, "ffplay-gui - No media loaded");
}

void Application::SeekToRatio(float ratio)
{
    ffplayer_seek_to_ratio(player_, ratio);
}

void Application::RenderImGui()
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

    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
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

            /*
             * Some short clips report container duration slightly longer than
             * decodable payload duration. At EOF, snap near-tail progress to 100%
             * so the thumb reaches the right edge instead of stopping around 4.x/5s.
             */
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
                /* Accept large backward jumps (explicit seek / restart). */
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
                /* Drag seeking: issue periodic seek requests while thumb is moving. */
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
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer_);
}

void Application::RenderLogPanel(float bar_height)
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
        if (log_level_filter_ == 1 && line.level < spdlog::level::info)
            continue;
        if (log_level_filter_ == 2 && line.level < spdlog::level::warn)
            continue;
        if (log_level_filter_ == 3 && line.level < spdlog::level::err)
            continue;

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

bool Application::OpenFileDialogAndPlay()
{
#ifdef _WIN32
    wchar_t file_name[MAX_PATH] = {0};
    OPENFILENAMEW ofn = {0};

    if (open_dialog_active_)
        return false;
    open_dialog_active_ = true;

    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = window_ ? GetActiveWindow() : nullptr;
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
    video_renderer_cleanup_textures(&video_renderer_ctx_);
    video_open_done_ = 0;
    stable_progress_ready_ = false;

    if (ffplayer_open(player_, utf8_path.data()) < 0) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Open failed", "Failed to open selected media file.", window_);
        RefreshWindowTitle();
        open_dialog_active_ = false;
        return false;
    }
    ffplayer_toggle_pause(player_);
    // ffplayer_toggle_mute(player_);
    ffplayer_request_refresh(player_);
    stable_progress_ratio_ = 0.0f;
    stable_progress_ready_ = false;

    RefreshWindowTitle();
    open_dialog_active_ = false;
    return true;
#else
    return false;
#endif
}

[[noreturn]] void Application::DoExit()
{
    video_renderer_cleanup_textures(&video_renderer_ctx_);
    ffplayer_free(&player_);
    ShutdownImGui();
    if (renderer_) {
        SDL_DestroyRenderer(renderer_);
        renderer_ = nullptr;
    }
    if (window_) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
    avformat_network_deinit();
    ShutdownUiLogger();
    SDL_Quit();
    av_log(nullptr, AV_LOG_QUIET, "%s", "");
    exit(0);
}

void Application::ToggleFullScreen()
{
    is_full_screen_ = !is_full_screen_;
    SDL_SetWindowFullscreen(window_, is_full_screen_ ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

void Application::RefreshLoopWaitEvent(SDL_Event *event)
{
    double remaining_time = 0.0;
    SDL_PumpEvents();
    while (!SDL_PeepEvents(event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT)) {
        if (!cursor_hidden_ && av_gettime_relative() - cursor_last_shown_ > FFPLAYER_CURSOR_HIDE_DELAY) {
            SDL_ShowCursor(0);
            cursor_hidden_ = 1;
        }
        if (remaining_time > 0.0)
            av_usleep((int64_t)(remaining_time * 1000000.0));
        remaining_time = FFPLAYER_REFRESH_RATE;
        if (ffplayer_needs_refresh(player_))
            ffplayer_refresh(player_, &remaining_time);
        video_renderer_clear(&video_renderer_ctx_);
        if (ffplayer_is_open(player_))
            DisplayVideo();
        RenderImGui();
        video_renderer_present(&video_renderer_ctx_);
        SDL_PumpEvents();
    }
}

void Application::InitWindowAndRenderer()
{
    int flags = SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE;

#ifdef SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR
    SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");
#endif
    window_ = SDL_CreateWindow(program_name, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                               kInitialDefaultWidth, kInitialDefaultHeight, flags);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    if (!window_) {
        av_log(nullptr, AV_LOG_FATAL, "Failed to create window: %s", SDL_GetError());
        DoExit();
    }

    renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer_) {
        av_log(nullptr, AV_LOG_WARNING, "Failed to initialize a hardware accelerated renderer: %s\n", SDL_GetError());
        renderer_ = SDL_CreateRenderer(window_, -1, 0);
    }
    if (renderer_) {
        if (!SDL_GetRendererInfo(renderer_, &video_renderer_ctx_.renderer_info))
            av_log(nullptr, AV_LOG_VERBOSE, "Initialized %s renderer.\n", video_renderer_ctx_.renderer_info.name);
    }
    if (!renderer_ || !video_renderer_ctx_.renderer_info.num_texture_formats) {
        av_log(nullptr, AV_LOG_FATAL, "Failed to create window or renderer: %s", SDL_GetError());
        DoExit();
    }
}

void Application::ConfigureVideoRenderer()
{
    video_renderer_ctx_.window = window_;
    video_renderer_ctx_.renderer = renderer_;
    video_renderer_ctx_.default_width = kInitialDefaultWidth;
    video_renderer_ctx_.default_height = kInitialDefaultHeight;
}

void Application::DisplayVideo()
{
    int win_w, win_h;
    SDL_GetWindowSize(window_, &win_w, &win_h);

    if (!video_open_done_) {
        video_renderer_open(&video_renderer_ctx_, &win_w, &win_h);
        ffplayer_set_window_size(player_, win_w, win_h);
        video_open_done_ = 1;
    }

    enum FFPlayerShowMode mode = ffplayer_get_show_mode(player_);
    if (mode != FFPLAYER_SHOW_MODE_VIDEO) {
        AudioVisualizer *vis = ffplayer_get_audio_visualizer(player_);
        if (vis)
            audio_visualizer_render(vis, renderer_, 0, 0, win_w, win_h);
    } else {
        AVFrame *frame = ffplayer_get_video_frame(player_);
        AVSubtitle *subtitle = ffplayer_get_subtitle(player_);
        if (frame)
            video_renderer_draw_video(&video_renderer_ctx_, frame, subtitle, 0, 0, win_w, win_h);
    }
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

void Application::EventLoop()
{
    SDL_Event event;
    double incr;

    for (;;) {
        RefreshLoopWaitEvent(&event);
        ImGui_ImplSDL2_ProcessEvent(&event);
        switch (event.type) {
        case SDL_KEYDOWN:
            if (event.key.keysym.sym == SDLK_ESCAPE || event.key.keysym.sym == SDLK_q) {
                DoExit();
                break;
            }
            if (ImGui::GetIO().WantCaptureKeyboard)
                break;
            if (event.key.keysym.sym == SDLK_o) {
                OpenFileDialogAndPlay();
                break;
            }
            if (event.key.keysym.sym == SDLK_f) {
                ToggleFullScreen();
                ffplayer_request_refresh(player_);
                break;
            }
            if (!ffplayer_is_open(player_))
                continue;
            if (!ffplayer_is_video_open(player_))
                continue;
            switch (event.key.keysym.sym) {
            case SDLK_p:
            case SDLK_SPACE:
                ffplayer_toggle_pause(player_);
                break;
            case SDLK_m:
                ffplayer_toggle_mute(player_);
                break;
            case SDLK_KP_MULTIPLY:
            case SDLK_0:
                ffplayer_adjust_volume_step(player_, 1, FFPLAYER_VOLUME_STEP);
                break;
            case SDLK_KP_DIVIDE:
            case SDLK_9:
                ffplayer_adjust_volume_step(player_, -1, FFPLAYER_VOLUME_STEP);
                break;
            case SDLK_s:
                ffplayer_step_frame(player_);
                break;
            case SDLK_a:
                ffplayer_cycle_audio_track(player_);
                break;
            case SDLK_v:
                ffplayer_cycle_video_track(player_);
                break;
            case SDLK_c:
                ffplayer_cycle_all_tracks(player_);
                break;
            case SDLK_t:
                ffplayer_cycle_subtitle_track(player_);
                break;
            case SDLK_w:
                ffplayer_toggle_audio_display(player_);
                break;
            case SDLK_PAGEUP:
                if (!ffplayer_has_chapters(player_)) {
                    incr = 600.0;
                    goto do_seek;
                }
                ffplayer_seek_chapter(player_, 1);
                break;
            case SDLK_PAGEDOWN:
                if (!ffplayer_has_chapters(player_)) {
                    incr = -600.0;
                    goto do_seek;
                }
                ffplayer_seek_chapter(player_, -1);
                break;
            case SDLK_LEFT:
                incr = -kSeekIntervalSeconds;
                goto do_seek;
            case SDLK_RIGHT:
                incr = kSeekIntervalSeconds;
                goto do_seek;
            case SDLK_UP:
                incr = 60.0;
                goto do_seek;
            case SDLK_DOWN:
                incr = -60.0;
do_seek:
                ffplayer_seek_relative(player_, incr);
                break;
            default:
                break;
            }
            break;
        case SDL_MOUSEBUTTONDOWN:
            if (ImGui::GetIO().WantCaptureMouse)
                break;
            if (event.button.button == SDL_BUTTON_LEFT) {
                static int64_t last_mouse_left_click = 0;
                if (av_gettime_relative() - last_mouse_left_click <= 500000) {
                    ToggleFullScreen();
                    ffplayer_request_refresh(player_);
                    last_mouse_left_click = 0;
                } else {
                    last_mouse_left_click = av_gettime_relative();
                }
            }
            if (cursor_hidden_) {
                SDL_ShowCursor(1);
                cursor_hidden_ = 0;
            }
            cursor_last_shown_ = av_gettime_relative();
            break;
        case SDL_MOUSEMOTION:
            if (cursor_hidden_) {
                SDL_ShowCursor(1);
                cursor_hidden_ = 0;
            }
            cursor_last_shown_ = av_gettime_relative();
            break;
        case SDL_WINDOWEVENT:
            switch (event.window.event) {
            case SDL_WINDOWEVENT_SIZE_CHANGED:
                ffplayer_handle_window_size_changed(player_, event.window.data1, event.window.data2);
            case SDL_WINDOWEVENT_EXPOSED:
                ffplayer_request_refresh(player_);
            }
            break;
        case SDL_QUIT:
        case FF_QUIT_EVENT:
            DoExit();
            break;
        default:
            break;
        }
    }
}

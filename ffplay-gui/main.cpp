#include <math.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string>
#include <vector>

#ifdef __cplusplus
extern "C" {
#endif

#include "libavutil/common.h"
#include "libavutil/log.h"
#include "libavutil/mathematics.h"
#include "libavutil/time.h"
#include "libavformat/avformat.h"
#include "libavdevice/avdevice.h"

#include <SDL.h>
#include <SDL_thread.h>

#ifdef _WIN32
#undef main
#include <windows.h>
#include <commdlg.h>
#endif

#include "clock.h"
#include "audio_device.h"
#include "audio_pipeline.h"
#include "demuxer.h"
#include "filter.h"
#include "stream.h"
#include "video_renderer.h"

#ifdef __cplusplus
}
#endif

#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_sdlrenderer2.h"

#ifdef __cplusplus
extern "C" {
#endif
void init_dynload(void)
{
}

#ifdef __cplusplus
}
#endif

const char program_name[] = "ffplay";
const int program_birth_year = 2003;

static constexpr int kInitialDefaultWidth = 640;
static constexpr int kInitialDefaultHeight = 480;
static constexpr float kSeekIntervalSeconds = 10.0f;
static constexpr double kDefaultRdftSpeed = 0.02;

static void sigterm_handler(int sig)
{
    exit(123);
}

static const char *video_window_title(VideoState *is, const char *fallback)
{
    const char *title = demuxer_get_input_name(is->demuxer);
    return (title && title[0]) ? title : fallback;
}

class Application {
public:
    int run()
    {
        int flags;

        init_dynload();
        av_log_set_flags(AV_LOG_SKIP_REPEATED);

        avdevice_register_all();
        avformat_network_init();

        signal(SIGINT , sigterm_handler);
        signal(SIGTERM, sigterm_handler);

        flags = SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER;
        if (!SDL_getenv("SDL_AUDIO_ALSA_SET_BUFFER_SIZE"))
            SDL_setenv("SDL_AUDIO_ALSA_SET_BUFFER_SIZE","1", 1);
        if (SDL_Init(flags)) {
            av_log(NULL, AV_LOG_FATAL, "Could not initialize SDL - %s\n", SDL_GetError());
            av_log(NULL, AV_LOG_FATAL, "(Did you set the DISPLAY variable?)\n");
            exit(1);
        }

        SDL_EventState(SDL_SYSWMEVENT, SDL_IGNORE);
        SDL_EventState(SDL_USEREVENT, SDL_IGNORE);

        init_window_and_renderer();
        configure_video_renderer();
        SDL_ShowWindow(window_);
        SDL_SetWindowTitle(window_, "ffplay-gui - No media loaded (Press O to Open)");

        audio_device_set_open_cb(&audio_device_, audio_pipeline_open);
        init_imgui();

        event_loop();
        return 0;
    }

private:
    SDL_Window *window_ = NULL;
    SDL_Renderer *renderer_ = NULL;
    AudioDevice audio_device_ = {0};
    int64_t cursor_last_shown_ = 0;
    int cursor_hidden_ = 0;
    int is_full_screen_ = 0;
    VideoState *stream_ = NULL;
    VideoRenderer video_renderer_ctx_ = {0};
    bool open_dialog_active_ = false;
    bool imgui_ready_ = false;
    float pending_seek_ratio_ = -1.0f;
    int64_t last_drag_seek_us_ = 0;
    float stable_progress_ratio_ = 0.0f;
    bool stable_progress_ready_ = false;

    static std::string format_duration(double seconds)
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

    void init_imgui()
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

    void shutdown_imgui()
    {
        if (!imgui_ready_)
            return;
        ImGui_ImplSDLRenderer2_Shutdown();
        ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext();
        imgui_ready_ = false;
    }

    void refresh_window_title()
    {
        if (stream_) {
            const char *title = video_window_title(stream_, "ffplay-gui");
            SDL_SetWindowTitle(window_, title ? title : "ffplay-gui");
            return;
        }
        SDL_SetWindowTitle(window_, "ffplay-gui - No media loaded (Press O to Open)");
    }

    void seek_to_ratio(float ratio)
    {
        int64_t ts;
        int64_t size;
        if (!stream_ || !stream_->ic)
            return;
        ratio = av_clipf(ratio, 0.0f, 1.0f);
        if (demuxer_get_seek_mode(stream_->demuxer) || stream_->ic->duration <= 0) {
            if (!stream_->ic->pb)
                return;
            size = avio_size(stream_->ic->pb);
            if (size <= 0)
                return;
            stream_seek(stream_, (int64_t)(size * ratio), 0, 1);
            return;
        }
        ts = (int64_t)(ratio * stream_->ic->duration);
        if (stream_->ic->start_time != AV_NOPTS_VALUE)
            ts += stream_->ic->start_time;
        stream_seek(stream_, ts, 0, 0);
    }

    void render_imgui()
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

        if (stream_ && stream_->ic) {
            has_known_duration = stream_->ic->duration > 0;
            can_approx_seek = stream_->ic->pb && avio_size(stream_->ic->pb) > 0;
            can_seek = has_known_duration || can_approx_seek;

            if (has_known_duration) {
                duration_sec = stream_->ic->duration / (double)AV_TIME_BASE;
                current_sec = get_master_clock(stream_);
                if (isnan(current_sec))
                    current_sec = 0.0;
                if (stream_->ic->start_time != AV_NOPTS_VALUE)
                    current_sec -= stream_->ic->start_time / (double)AV_TIME_BASE;
                current_sec = FFMAX(0.0, FFMIN(current_sec, duration_sec));
                progress = duration_sec > 0.0 ? (float)(current_sec / duration_sec) : 0.0f;

                /*
                 * Some short clips report container duration slightly longer than
                 * decodable payload duration. At EOF, snap near-tail progress to 100%
                 * so the thumb reaches the right edge instead of stopping around 4.x/5s.
                 */
                if (stream_->eof && progress > 0.90f)
                    progress = 1.0f;

                if (!isfinite(progress))
                    progress = 0.0f;
                using_stable_progress = true;
                time_text = format_duration(current_sec) + " / " + format_duration(duration_sec);
            } else if (can_approx_seek) {
                int64_t size = avio_size(stream_->ic->pb);
                int64_t pos = avio_tell(stream_->ic->pb);
                if (size > 0 && pos >= 0)
                    progress = av_clipf((float)pos / (float)size, 0.0f, 1.0f);
                time_text = "Unknown duration | Approx seek";
            } else {
                time_text = "Unknown duration";
            }

            play_text = stream_->paused ? "Play" : "Pause";
        }
        if (!stream_)
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
        if (stream_)
            volume_percent = 100.0f * (float)stream_->audio_volume / (float)SDL_MIX_MAXVOLUME;

        ImGui::SetNextWindowPos(ImVec2(0.0f, io.DisplaySize.y - bar_height));
        ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, bar_height));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.03f, 0.04f, 0.08f, 0.85f));
        ImGui::Begin("PlayerControls", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);

        if (!stream_) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("No media loaded");
            ImGui::SameLine();
            if (ImGui::Button("Open file..."))
                open_file_dialog_and_play();
        } else {
            float avail_w = ImGui::GetContentRegionAvail().x;
            float play_w = ImGui::CalcTextSize(play_text.c_str()).x + ImGui::GetStyle().FramePadding.x * 2.0f;
            float time_w = ImGui::CalcTextSize(time_text.c_str()).x;
            float volume_w = FFMIN(110.0f, FFMAX(72.0f, avail_w * 0.22f));
            float min_timeline_w = 80.0f;
            bool hide_time_text = false;
            float needed_w = play_w + margin + time_w + margin + min_timeline_w + margin + volume_w;

            if (avail_w < needed_w) {
                hide_time_text = true;
                needed_w = play_w + margin + min_timeline_w + margin + volume_w;
            }
            if (avail_w < needed_w) {
                volume_w = FFMAX(58.0f, avail_w * 0.18f);
            }

            float timeline_w = FFMAX(
                min_timeline_w,
                avail_w - play_w - margin - (hide_time_text ? 0.0f : (time_w + margin)) - margin - volume_w);

            if (ImGui::Button(play_text.c_str()))
                stream_toggle_pause_and_clear_step(stream_);
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
                        seek_to_ratio(pending_seek_ratio_);
                        last_drag_seek_us_ = now_us;
                    }
                }
            }
            if (pending_seek_ratio_ >= 0.0f && ImGui::IsItemDeactivatedAfterEdit()) {
                seek_to_ratio(pending_seek_ratio_);
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
                stream_->audio_volume = av_clip((int)((volume_percent / 100.0f) * SDL_MIX_MAXVOLUME), 0, SDL_MIX_MAXVOLUME);
            }
        }

        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
        ImGui::Render();
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer_);
    }

    bool open_file_dialog_and_play()
    {
#ifdef _WIN32
        wchar_t file_name[MAX_PATH] = {0};
        OPENFILENAMEW ofn = {0};

        if (open_dialog_active_)
            return false;
        open_dialog_active_ = true;

        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = window_ ? GetActiveWindow() : NULL;
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

        int utf8_len = WideCharToMultiByte(CP_UTF8, 0, file_name, -1, NULL, 0, NULL, NULL);
        if (utf8_len <= 0) {
            open_dialog_active_ = false;
            return false;
        }
        std::vector<char> utf8_path(static_cast<size_t>(utf8_len), 0);
        if (WideCharToMultiByte(CP_UTF8, 0, file_name, -1, utf8_path.data(), utf8_len, NULL, NULL) <= 0) {
            open_dialog_active_ = false;
            return false;
        }

        if (stream_) {
            stream_close(stream_);
            stream_ = NULL;
        }
        stable_progress_ready_ = false;

        stream_ = stream_open(utf8_path.data(), &audio_device_, &video_renderer_ctx_, NULL);
        if (!stream_) {
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Open failed", "Failed to open selected media file.", window_);
            refresh_window_title();
            open_dialog_active_ = false;
            return false;
        }
        stream_->force_refresh = 1;
        stable_progress_ratio_ = 0.0f;
        stable_progress_ready_ = false;

        refresh_window_title();
        open_dialog_active_ = false;
        return true;
#else
        return false;
#endif
    }

    [[noreturn]] void do_exit(VideoState *is)
    {
        VideoState *target = is ? is : stream_;
        if (target) {
            stream_close(target);
            if (target == stream_)
                stream_ = NULL;
        }
        shutdown_imgui();
        if (renderer_) {
            SDL_DestroyRenderer(renderer_);
            renderer_ = NULL;
        }
        if (window_) {
            SDL_DestroyWindow(window_);
            window_ = NULL;
        }
        avformat_network_deinit();
        SDL_Quit();
        av_log(NULL, AV_LOG_QUIET, "%s", "");
        exit(0);
    }

    void toggle_full_screen()
    {
        is_full_screen_ = !is_full_screen_;
        SDL_SetWindowFullscreen(window_, is_full_screen_ ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    }

    void refresh_loop_wait_event(SDL_Event *event)
    {
        double remaining_time = 0.0;
        SDL_PumpEvents();
        while (!SDL_PeepEvents(event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT)) {
            if (!cursor_hidden_ && av_gettime_relative() - cursor_last_shown_ > CURSOR_HIDE_DELAY) {
                SDL_ShowCursor(0);
                cursor_hidden_ = 1;
            }
            if (remaining_time > 0.0)
                av_usleep((int64_t)(remaining_time * 1000000.0));
            remaining_time = REFRESH_RATE;
            if (stream_ && stream_->show_mode != SHOW_MODE_NONE && (!stream_->paused || stream_->force_refresh))
                video_renderer_refresh(stream_->video_renderer, stream_, &remaining_time);
            if (stream_)
                video_renderer_display(stream_->video_renderer, stream_);
            else {
                SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
                SDL_RenderClear(renderer_);
            }
            render_imgui();
            video_renderer_present(stream_ ? stream_->video_renderer : &video_renderer_ctx_);
            SDL_PumpEvents();
        }
    }

    void init_window_and_renderer()
    {
        int flags = SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE;

#ifdef SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR
        SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");
#endif
        window_ = SDL_CreateWindow(program_name, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                   kInitialDefaultWidth, kInitialDefaultHeight, flags);
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
        if (!window_) {
            av_log(NULL, AV_LOG_FATAL, "Failed to create window: %s", SDL_GetError());
            do_exit(NULL);
        }

        renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (!renderer_) {
            av_log(NULL, AV_LOG_WARNING, "Failed to initialize a hardware accelerated renderer: %s\n", SDL_GetError());
            renderer_ = SDL_CreateRenderer(window_, -1, 0);
        }
        if (renderer_) {
            if (!SDL_GetRendererInfo(renderer_, &video_renderer_ctx_.renderer_info))
                av_log(NULL, AV_LOG_VERBOSE, "Initialized %s renderer.\n", video_renderer_ctx_.renderer_info.name);
        }
        if (!renderer_ || !video_renderer_ctx_.renderer_info.num_texture_formats) {
            av_log(NULL, AV_LOG_FATAL, "Failed to create window or renderer: %s", SDL_GetError());
            do_exit(NULL);
        }
    }

    void configure_video_renderer()
    {
        video_renderer_ctx_.window = window_;
        video_renderer_ctx_.renderer = renderer_;
        video_renderer_ctx_.default_width = kInitialDefaultWidth;
        video_renderer_ctx_.default_height = kInitialDefaultHeight;
        video_renderer_ctx_.is_full_screen = &is_full_screen_;
        video_renderer_ctx_.rdftspeed = kDefaultRdftSpeed;
        video_renderer_ctx_.program_name = program_name;
        video_renderer_ctx_.title_provider = video_window_title;
    }

    void event_loop()
    {
        SDL_Event event;
        double incr, frac;

        for (;;) {
            double x;
            refresh_loop_wait_event(&event);
            ImGui_ImplSDL2_ProcessEvent(&event);
            switch (event.type) {
            case SDL_KEYDOWN:
                if (event.key.keysym.sym == SDLK_ESCAPE || event.key.keysym.sym == SDLK_q) {
                    do_exit(stream_);
                    break;
                }
                if (ImGui::GetIO().WantCaptureKeyboard)
                    break;
                if (event.key.keysym.sym == SDLK_o) {
                    open_file_dialog_and_play();
                    break;
                }
                if (event.key.keysym.sym == SDLK_f) {
                    toggle_full_screen();
                    if (stream_)
                        stream_->force_refresh = 1;
                    break;
                }
                if (!stream_)
                    continue;
                if (!stream_->width)
                    continue;
                switch (event.key.keysym.sym) {
                case SDLK_p:
                case SDLK_SPACE:
                    stream_toggle_pause_and_clear_step(stream_);
                    break;
                case SDLK_m:
                    stream_toggle_mute(stream_);
                    break;
                case SDLK_KP_MULTIPLY:
                case SDLK_0:
                    stream_adjust_volume_step(stream_, 1, SDL_VOLUME_STEP);
                    break;
                case SDLK_KP_DIVIDE:
                case SDLK_9:
                    stream_adjust_volume_step(stream_, -1, SDL_VOLUME_STEP);
                    break;
                case SDLK_s:
                    stream_step(stream_);
                    break;
                case SDLK_a:
                    stream_cycle_channel(stream_, AVMEDIA_TYPE_AUDIO);
                    break;
                case SDLK_v:
                    stream_cycle_channel(stream_, AVMEDIA_TYPE_VIDEO);
                    break;
                case SDLK_c:
                    stream_cycle_channel(stream_, AVMEDIA_TYPE_VIDEO);
                    stream_cycle_channel(stream_, AVMEDIA_TYPE_AUDIO);
                    stream_cycle_channel(stream_, AVMEDIA_TYPE_SUBTITLE);
                    break;
                case SDLK_t:
                    stream_cycle_channel(stream_, AVMEDIA_TYPE_SUBTITLE);
                    break;
                case SDLK_w:
                    stream_toggle_audio_display(stream_);
                    break;
                case SDLK_PAGEUP:
                    if (stream_->ic->nb_chapters <= 1) {
                        incr = 600.0;
                        goto do_seek;
                    }
                    stream_seek_chapter(stream_, 1);
                    break;
                case SDLK_PAGEDOWN:
                    if (stream_->ic->nb_chapters <= 1) {
                        incr = -600.0;
                        goto do_seek;
                    }
                    stream_seek_chapter(stream_, -1);
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
                    stream_seek_relative(stream_, incr);
                    break;
                default:
                    break;
                }
                break;
            case SDL_MOUSEBUTTONDOWN:
                if (ImGui::GetIO().WantCaptureMouse)
                    break;
                if (!stream_ && event.button.button == SDL_BUTTON_LEFT) {
                    open_file_dialog_and_play();
                    break;
                }
                if (event.button.button == SDL_BUTTON_LEFT) {
                    static int64_t last_mouse_left_click = 0;
                    if (av_gettime_relative() - last_mouse_left_click <= 500000) {
                        toggle_full_screen();
                        if (stream_)
                            stream_->force_refresh = 1;
                        last_mouse_left_click = 0;
                    } else {
                        last_mouse_left_click = av_gettime_relative();
                    }
                }
            case SDL_MOUSEMOTION:
                if (cursor_hidden_) {
                    SDL_ShowCursor(1);
                    cursor_hidden_ = 0;
                }
                cursor_last_shown_ = av_gettime_relative();
                if (event.type == SDL_MOUSEBUTTONDOWN) {
                    if (event.button.button != SDL_BUTTON_RIGHT)
                        break;
                    if (!stream_)
                        break;
                    x = event.button.x;
                } else {
                    if (!(event.motion.state & SDL_BUTTON_RMASK))
                        break;
                    if (!stream_)
                        break;
                    x = event.motion.x;
                }
                if (demuxer_get_seek_mode(stream_->demuxer) || stream_->ic->duration <= 0) {
                    uint64_t size = avio_size(stream_->ic->pb);
                    stream_seek(stream_, size * x / stream_->width, 0, 1);
                } else {
                    int64_t ts;
                    int ns, hh, mm, ss;
                    int tns, thh, tmm, tss;
                    tns  = stream_->ic->duration / 1000000LL;
                    thh  = tns / 3600;
                    tmm  = (tns % 3600) / 60;
                    tss  = (tns % 60);
                    frac = x / stream_->width;
                    ns   = frac * tns;
                    hh   = ns / 3600;
                    mm   = (ns % 3600) / 60;
                    ss   = (ns % 60);
                    av_log(NULL, AV_LOG_INFO,
                           "Seek to %2.0f%% (%2d:%02d:%02d) of total duration (%2d:%02d:%02d)       \n", frac * 100,
                           hh, mm, ss, thh, tmm, tss);
                    ts = frac * stream_->ic->duration;
                    if (stream_->ic->start_time != AV_NOPTS_VALUE)
                        ts += stream_->ic->start_time;
                    stream_seek(stream_, ts, 0, 0);
                }
                break;
            case SDL_WINDOWEVENT:
                switch (event.window.event) {
                case SDL_WINDOWEVENT_SIZE_CHANGED:
                    if (stream_) {
                        stream_->width  = event.window.data1;
                        stream_->height = event.window.data2;
                        if (stream_->vis_texture) {
                            SDL_DestroyTexture(stream_->vis_texture);
                            stream_->vis_texture = NULL;
                        }
                    }
                case SDL_WINDOWEVENT_EXPOSED:
                    if (stream_)
                        stream_->force_refresh = 1;
                }
                break;
            case SDL_QUIT:
            case FF_QUIT_EVENT:
                do_exit(stream_);
                break;
            default:
                break;
            }
        }
    }
};
#ifdef _WIN32
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    Application app;
    return app.run();
}
#else
int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    Application app;
    return app.run();
}
#endif

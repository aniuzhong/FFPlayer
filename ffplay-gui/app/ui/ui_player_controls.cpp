#include "application.h"
#include "utils/time_format.h"

extern "C" {
#include <libavutil/common.h>
#include <libavutil/time.h>
}

#include <math.h>

#include "imgui.h"
#include <SDL.h>

bool UiDrawPlayerControls(Application &app)
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

    FFPlayer *pl = app.player();

    if (ffplayer_is_open(pl)) {
        duration_sec = ffplayer_get_duration(pl);
        has_known_duration = duration_sec > 0;
        can_seek = ffplayer_can_seek(pl);
        can_approx_seek = can_seek && !has_known_duration;

        if (has_known_duration) {
            current_sec = ffplayer_get_position(pl);
            current_sec = FFMAX(0.0, FFMIN(current_sec, duration_sec));
            progress = duration_sec > 0.0 ? (float)(current_sec / duration_sec) : 0.0f;

            if (ffplayer_is_eof(pl) && progress > 0.90f)
                progress = 1.0f;

            if (!isfinite(progress))
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
        app.ui_stable_progress_ready() = false;

    float &pending = app.ui_pending_seek_ratio();
    float &stable_r = app.ui_stable_progress_ratio();
    bool &stable_ok = app.ui_stable_progress_ready();
    int64_t &drag_us = app.ui_last_drag_seek_us();

    if (using_stable_progress && pending < 0.0f) {
        if (!stable_ok) {
            stable_r = progress;
            stable_ok = true;
        } else {
            float delta = progress - stable_r;
            if (delta >= -0.003f) {
                stable_r = FFMAX(stable_r, progress);
            } else if (delta <= -0.08f) {
                stable_r = progress;
            }
        }
        progress = stable_r;
    }
    if (!isfinite(progress))
        progress = 0.0f;
    if (ffplayer_is_open(pl))
        volume_percent = 100.0f * (float)ffplayer_get_volume(pl) / (float)SDL_MIX_MAXVOLUME;

    ImGui::SetNextWindowPos(ImVec2(0.0f, io.DisplaySize.y - bar_height));
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, bar_height));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.03f, 0.04f, 0.08f, 0.85f));
    ImGui::Begin("PlayerControls", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);

    const int url_cap = static_cast<int>(Application::ui_open_url_buffer_bytes());

    if (!ffplayer_is_open(pl)) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("No media loaded");
        ImGui::SameLine();
        if (ImGui::Button("Open file..."))
            app.ui_open_file_dialog_and_play();
        ImGui::SameLine();
        bool open_url_clicked = ImGui::Button("Open URL");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(FFMAX(220.0f, ImGui::GetContentRegionAvail().x));
        bool url_entered = ImGui::InputTextWithHint(
            "##open_url",
            "https://... / rtsp://... / file path",
            app.ui_open_url_buffer(),
            url_cap,
            ImGuiInputTextFlags_EnterReturnsTrue);
        if (open_url_clicked || url_entered)
            app.ui_open_url_and_play();
    } else {
        float avail_w = ImGui::GetContentRegionAvail().x;
        float play_w = ImGui::CalcTextSize(play_text.c_str()).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        float stop_w = ImGui::CalcTextSize("Stop").x + ImGui::GetStyle().FramePadding.x * 2.0f;
        float time_w = ImGui::CalcTextSize(time_text.c_str()).x;
        float volume_w = FFMIN(110.0f, FFMAX(72.0f, avail_w * 0.22f));
        constexpr float min_timeline_w = 80.0f;
        bool hide_time_text = false;
        float needed_w = play_w + margin + stop_w + margin + time_w + margin + min_timeline_w + margin + volume_w;

        if (avail_w < needed_w) {
            hide_time_text = true;
            needed_w = play_w + margin + stop_w + margin + min_timeline_w + margin + volume_w;
        }
        if (avail_w < needed_w) {
            volume_w = FFMAX(58.0f, avail_w * 0.18f);
        }

        float timeline_w = FFMAX(
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
                    int64_t now_us = av_gettime_relative();
                    if (drag_us == 0 || now_us - drag_us >= 40000) {
                        app.ui_seek_to_ratio(pending);
                        drag_us = now_us;
                    }
                }
            }
            if (pending >= 0.0f && ImGui::IsItemDeactivatedAfterEdit()) {
                app.ui_seek_to_ratio(pending);
                stable_r = pending;
                stable_ok = true;
                pending = -1.0f;
                drag_us = 0;
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

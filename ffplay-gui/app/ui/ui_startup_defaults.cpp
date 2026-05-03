#include "application.h"

extern "C" {
#include <libavutil/common.h>
}

#include "imgui.h"

void UiDrawStartupDefaults(Application &app, float main_menu_bar_bottom)
{
    if (ffplayer_is_open(app.player()))
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
    int &infbuf = app.ui_startup_infinite_buffer();
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
        inf_combo_w = FFMAX(inf_combo_w, ImGui::CalcTextSize(inf_labels[i], nullptr, true).x);
    inf_combo_w += st.FramePadding.x * 2.0f + ImGui::GetFrameHeight();
    inf_combo_w = FFMIN(inf_combo_w, ImGui::GetContentRegionAvail().x);
    ImGui::SetNextItemWidth(inf_combo_w);
    if (ImGui::Combo("##infbuf", &inf_idx, inf_labels, IM_ARRAYSIZE(inf_labels))) {
        infbuf = (inf_idx == 0) ? -1 : (inf_idx == 1) ? 0 : 1;
        ffplayer_set_infinite_buffer(app.player(), infbuf);
    }

    ImGui::SameLine(0.0f, 24.0f);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("-sync");
    ImGui::SameLine();
    ImGui::TextUnformatted("=");
    ImGui::SameLine();
    int sync_idx = app.ui_startup_av_sync_type();
    if (sync_idx < FFPLAYER_AV_SYNC_AUDIO_MASTER || sync_idx > FFPLAYER_AV_SYNC_EXTERNAL_CLOCK)
        sync_idx = FFPLAYER_AV_SYNC_AUDIO_MASTER;
    static const char *sync_labels[] = {
        "audio",
        "video",
        "ext",
    };
    float sync_combo_w = 0.0f;
    for (int i = 0; i < IM_ARRAYSIZE(sync_labels); ++i)
        sync_combo_w = FFMAX(sync_combo_w, ImGui::CalcTextSize(sync_labels[i], nullptr, true).x);
    sync_combo_w += st.FramePadding.x * 2.0f + ImGui::GetFrameHeight();
    sync_combo_w = FFMIN(sync_combo_w, ImGui::GetContentRegionAvail().x);
    ImGui::SetNextItemWidth(sync_combo_w);
    if (ImGui::Combo("##sync", &sync_idx, sync_labels, IM_ARRAYSIZE(sync_labels))) {
        app.ui_startup_av_sync_type() = sync_idx;
        ffplayer_set_av_sync_type(app.player(), app.ui_startup_av_sync_type());
    }

    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

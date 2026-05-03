#include "application.h"

extern "C" {
#include <libavutil/pixdesc.h>
}

#include "imgui.h"

void UiDrawStatisticsWindow(Application &app)
{
    if (!app.ui_show_statistics())
        return;

    FFPlayer *pl = app.player();
    const bool decoder_hw = ffplayer_is_video_decoder_hardware(pl) != 0;
    const bool hw_fallback = ffplayer_has_video_hw_fallback(pl) != 0;
    const char *pipeline_mode = "No Frame";
    if (app.ui_stats_has_video_frame())
        pipeline_mode = app.ui_stats_pipeline_zero_copy() ? "Zero-Copy (D3D11)" : "Software Upload";
    const char *pix_fmt_name = app.ui_stats_has_video_frame() ? av_get_pix_fmt_name(app.ui_stats_video_pix_fmt()) : nullptr;
    if (!pix_fmt_name)
        pix_fmt_name = "N/A";

    ImGui::SetNextWindowSize(ImVec2(360.0f, 190.0f), ImGuiCond_FirstUseEver);
    bool &show = app.ui_show_statistics();
    if (ImGui::Begin("Statistics", &show)) {
        ImGui::Text("Render FPS: %.2f", app.ui_render_fps());
        ImGui::Text("Frame Time: %.2f ms", app.ui_render_frame_time_ms());
        ImGui::Text("Pipeline Mode: %s", pipeline_mode);
        ImGui::Text("Video PixFmt: %s", pix_fmt_name);
        ImGui::Text("Decoder Mode: %s", decoder_hw ? "HW" : "SW");
        ImGui::Text("HW Fallback: %s", hw_fallback ? "Triggered" : "No");
    }
    ImGui::End();
}

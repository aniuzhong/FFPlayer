#include "ui/ui_main_menu.h"

#include "imgui.h"

float UiDrawMainMenuBar(bool *show_statistics_window)
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

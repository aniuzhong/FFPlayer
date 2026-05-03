#include "native/win32_window_proc.h"

#include "application.h"

extern "C" {
#include <libavutil/time.h>
}

#include "imgui.h"
#include "backends/imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {

constexpr float kSeekIntervalSeconds = 10.0f;

void DispatchKeyDown(Application &app, WPARAM wParam)
{
    if (wParam == VK_ESCAPE || wParam == 'Q') {
        app.ui_quit_application();
        return;
    }
    if (ImGui::GetIO().WantCaptureKeyboard)
        return;
    if (wParam == 'O') {
        app.ui_open_file_dialog_and_play();
        return;
    }
    if (wParam == 'F') {
        app.ui_toggle_fullscreen();
        app.ui_player_request_refresh();
        return;
    }

    FFPlayer *pl = app.player();
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

    Application *app = reinterpret_cast<Application *>(GetWindowLongPtr(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_SIZE:
        if (app)
            app->win32_on_client_resize(wParam, LOWORD(lParam), HIWORD(lParam));
        return 0;
    case WM_KEYDOWN:
        if (app)
            DispatchKeyDown(*app, wParam);
        return 0;
    case WM_LBUTTONDOWN:
        if (app) {
            if (ImGui::GetIO().WantCaptureMouse)
                break;
            static int64_t last_click = 0;
            if (av_gettime_relative() - last_click <= 500000) {
                app->ui_toggle_fullscreen();
                app->ui_player_request_refresh();
                last_click = 0;
            } else {
                last_click = av_gettime_relative();
            }
            app->ui_note_mouse_activity_show_cursor();
        }
        return 0;
    case WM_MOUSEMOVE:
        if (app)
            app->ui_note_mouse_activity_show_cursor();
        return 0;
    case WM_CLOSE:
    case WM_DESTROY:
        if (app)
            app->win32_request_quit();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

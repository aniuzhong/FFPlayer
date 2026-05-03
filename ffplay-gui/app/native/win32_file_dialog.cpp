#include "native/win32_file_dialog.h"

#include <commdlg.h>
#include <vector>

bool Win32PickMediaFileUtf8(HWND owner, std::string &out_utf8_path)
{
    out_utf8_path.clear();

    wchar_t file_name[MAX_PATH] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFile = file_name;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter =
        L"Media Files\0*.mp4;*.mkv;*.avi;*.mov;*.wmv;*.flv;*.webm;*.mp3;*.wav;*.flac;*.aac;*.ogg;*.m4a\0"
        L"All Files\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    ofn.lpstrTitle = L"Open media file";

    if (!GetOpenFileNameW(&ofn))
        return false;

    const int utf8_len = WideCharToMultiByte(CP_UTF8, 0, file_name, -1, nullptr, 0, nullptr, nullptr);
    if (utf8_len <= 0)
        return false;

    std::vector<char> utf8(static_cast<size_t>(utf8_len), 0);
    if (WideCharToMultiByte(CP_UTF8, 0, file_name, -1, utf8.data(), utf8_len, nullptr, nullptr) <= 0)
        return false;

    out_utf8_path.assign(utf8.data());

    return true;
}

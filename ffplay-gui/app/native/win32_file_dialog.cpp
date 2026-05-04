#include "native/win32_file_dialog.h"

#include <windows.h>
#include <commdlg.h>

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

    /*
     * cchWideChar=-1: NUL-terminated wide path; WideCharToMultiByte sizes/converts including UTF-8 NUL.
     * Use explicit byte counts for std::string — never strlen(assign(char*)) on the conversion buffer.
     */
    const int nbytes =
        WideCharToMultiByte(CP_UTF8, 0, file_name, -1, nullptr, 0, nullptr, nullptr);
    if (nbytes <= 0)
        return false;

    out_utf8_path.resize(static_cast<size_t>(nbytes - 1));
    if (WideCharToMultiByte(CP_UTF8, 0, file_name, -1, out_utf8_path.data(), nbytes, nullptr,
                            nullptr) != nbytes)
        return false;

    return true;
}

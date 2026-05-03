#ifndef FFPLAY_GUI_APP_NATIVE_WIN32_FILE_DIALOG_H
#define FFPLAY_GUI_APP_NATIVE_WIN32_FILE_DIALOG_H

#include <windows.h>

#include <string>

/**
 * GetOpenFileNameW (classic dialog).
 * On OK: fills UTF-8 path for libav / SDL style APIs.
 */
bool Win32PickMediaFileUtf8(HWND owner, std::string &out_utf8_path);

#endif

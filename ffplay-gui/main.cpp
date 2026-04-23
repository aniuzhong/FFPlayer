#include <windows.h>

#include "application.h"

#undef main

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    Application app;
    return app.Execute();
}

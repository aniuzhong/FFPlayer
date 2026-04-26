#include <windows.h>

#ifdef FFPLAYER_USE_D3D11
#include "application_d3d11.h"
#else
#include "application.h"
#endif

#undef main

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
#ifdef FFPLAYER_USE_D3D11
    ApplicationD3D11 app;
#else
    Application app;
#endif
    return app.Execute();
}

#include "application.h"

#ifdef _WIN32
#undef main
#include <windows.h>

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    Application app;
    return app.Run();
}
#else
int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    Application app;
    return app.Run();
}
#endif

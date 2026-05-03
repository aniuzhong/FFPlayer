#include "utils/time_format.h"

#include <cstdio>

std::string FormatDuration(double seconds)
{
    int total = (int)(seconds >= 0.0 ? seconds : 0.0);
    int h = total / 3600;
    int m = (total % 3600) / 60;
    int s = total % 60;
    char buf[32];
    if (h > 0)
        snprintf(buf, sizeof(buf), "%d:%02d:%02d", h, m, s);
    else
        snprintf(buf, sizeof(buf), "%02d:%02d", m, s);
    return std::string(buf);
}

/**
 * The dev package omit libavutil/libm.h. This stub exposes the standard C
 * math API via <math.h> so fftools can build without pulling in the full FFmpeg tree.
 */

#ifndef AVUTIL_LIBM_H
#define AVUTIL_LIBM_H

#include <math.h>

#endif /* AVUTIL_LIBM_H */

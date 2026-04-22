#include "demuxer.h"

#include <errno.h>
#include <string.h>

#include "libavutil/error.h"
#include "libavutil/mem.h"

int demuxer_init(Demuxer *demuxer, const char *input_url)
{
    if (!demuxer || !input_url)
        return AVERROR(EINVAL);
    memset(demuxer, 0, sizeof(*demuxer));
    demuxer->seek_mode = -1;
    demuxer->input_url = av_strdup(input_url);
    if (!demuxer->input_url)
        return AVERROR(ENOMEM);
    return 0;
}

void demuxer_destroy(Demuxer *demuxer)
{
    if (!demuxer)
        return;
    avformat_close_input(&demuxer->ic);
    av_freep(&demuxer->input_url);
    demuxer->seek_mode = -1;
    demuxer->abort_request = 0;
}

void demuxer_request_abort(Demuxer *demuxer)
{
    if (!demuxer)
        return;
    demuxer->abort_request = 1;
}

int demuxer_is_aborted(const Demuxer *demuxer)
{
    if (!demuxer)
        return 0;
    return demuxer->abort_request;
}

int demuxer_get_seek_mode(const Demuxer *demuxer)
{
    if (!demuxer || demuxer->seek_mode < 0)
        return 0;
    return demuxer->seek_mode;
}

const char *demuxer_get_input_name(const Demuxer *demuxer)
{
    if (!demuxer || !demuxer->input_url)
        return "";
    return demuxer->input_url;
}


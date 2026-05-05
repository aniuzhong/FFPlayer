# HW decode detection: OSD reports "hardware" for unsupported codecs

**Root cause** — `stream.c:794` uses `avctx->hw_device_ctx` to set
`video_decoder_uses_hw`.  This pointer is non-null whenever the renderer
attached a D3D11VA device to the codec context *before* open, regardless
of whether the codec actually supports D3D11VA (VP8 has no hwaccel in
FFmpeg, yet `hw_device_ctx` stays set after `avcodec_open2`).

**Fix** — Check `avctx->hw_frames_ctx` instead.  It is only assigned
when `ffplay_get_format` successfully selects `AV_PIX_FMT_D3D11` and
initialises the hardware frames pool.

| Codec | Old (`hw_device_ctx`) | New (`hw_frames_ctx`) |
|-------|-----------------------|-----------------------|
| VP8   | `!= NULL` → **false positive** | `== NULL` → correct |
| H.264 D3D11VA | `!= NULL` → correct | `!= NULL` → correct |

**Files touched**
- `ffplay-gui/ffplayer/stream.c` — one-line change

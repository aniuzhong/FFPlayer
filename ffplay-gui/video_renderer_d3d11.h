#ifndef FFPLAY_GUI_VIDEO_RENDERER_D3D11_H
#define FFPLAY_GUI_VIDEO_RENDERER_D3D11_H

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VideoRendererD3D11 {
    HWND hwnd;
    int default_width;
    int default_height;

    /* D3D11 core objects */
    ID3D11Device            *device;
    ID3D11DeviceContext     *context;
    IDXGISwapChain          *swap_chain;
    ID3D11RenderTargetView  *rtv;

    /* Pipeline state for textured-quad rendering */
    ID3D11VertexShader      *vertex_shader;
    ID3D11PixelShader       *pixel_shader;
    ID3D11InputLayout       *input_layout;
    ID3D11SamplerState      *sampler;
    ID3D11Buffer            *vertex_buffer;
    ID3D11BlendState        *blend_state;

    /* Video texture (BGRA) */
    ID3D11Texture2D         *vid_texture;
    ID3D11ShaderResourceView *vid_srv;
    int vid_tex_width;
    int vid_tex_height;

    /* Subtitle texture (BGRA with alpha) */
    ID3D11Texture2D         *sub_texture;
    ID3D11ShaderResourceView *sub_srv;
    int sub_tex_width;
    int sub_tex_height;
    struct SwsContext        *sub_convert_ctx;

    /* Frame-tracking to avoid redundant uploads */
    const uint8_t *last_vid_data;
    int last_flip_v;

    /* Vertex buffer data for positioning the quad */
    float target_rect[4]; /* x, y, w, h in pixels */
} VideoRendererD3D11;

int  video_renderer_d3d11_init(VideoRendererD3D11 *vr, HWND hwnd);
void video_renderer_d3d11_shutdown(VideoRendererD3D11 *vr);

void video_renderer_d3d11_set_default_window_size(VideoRendererD3D11 *vr,
                                                   int screen_width, int screen_height,
                                                   int width, int height, AVRational sar);
int  video_renderer_d3d11_get_supported_pixel_formats(const VideoRendererD3D11 *vr,
                                                      enum AVPixelFormat *out_fmts,
                                                      int max_fmts);
int  video_renderer_d3d11_open(VideoRendererD3D11 *vr, int *width, int *height);
void video_renderer_d3d11_draw_video(VideoRendererD3D11 *vr, AVFrame *frame, AVSubtitle *subtitle,
                                     int xleft, int ytop, int width, int height);
void video_renderer_d3d11_clear(VideoRendererD3D11 *vr);
void video_renderer_d3d11_present(VideoRendererD3D11 *vr);
void video_renderer_d3d11_cleanup_textures(VideoRendererD3D11 *vr);
void video_renderer_d3d11_resize(VideoRendererD3D11 *vr, int width, int height);

#ifdef __cplusplus
}
#endif

#endif

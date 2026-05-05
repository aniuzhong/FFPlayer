#ifndef FFPLAY_GUI_VIDEO_RENDERER_H
#define FFPLAY_GUI_VIDEO_RENDERER_H

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

/* Output color path. Step 1 only ships SDR sRGB (HDR/DV content is
 * tone-mapped to BT.709 SDR via BT.2390 EETF); HDR10_PQ is the
 * extension point for a future Step 2 that detects an HDR-capable
 * monitor with Windows HDR enabled and switches the swapchain to
 * R10G10B10A2_UNORM + DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020. */
enum class RendererColorTarget {
    SDR_SRGB = 0,
    HDR10_PQ = 1,
};

/* Cached frame-side color metadata that drives the constant buffer
 * used by the NV12/P010 sampling shader. We rebuild the cbuffer only
 * when one of these fields changes between frames, since 99% of
 * frames in a stream share the same color metadata. */
struct RendererColorState {
    int   colorspace = 0;          /* AVColorSpace */
    int   color_primaries = 0;     /* AVColorPrimaries */
    int   color_trc = 0;           /* AVColorTransferCharacteristic */
    int   color_range = 0;         /* AVColorRange */
    float peak_nits = 0.0f;        /* mastering display max luminance */
    float max_cll_nits = 0.0f;     /* AV_FRAME_DATA_CONTENT_LIGHT_LEVEL.MaxCLL */
    int   target = 0;              /* RendererColorTarget */
};

class VideoRenderer {
public:
    VideoRenderer() = default;
    ~VideoRenderer();

    /* Non-copyable, non-movable to keep the COM lifecycle simple. */
    VideoRenderer(const VideoRenderer &) = delete;
    VideoRenderer &operator=(const VideoRenderer &) = delete;

    /* Setup / teardown */
    int  init(HWND hwnd);
    void shutdown();

    /* Borrowed pointer to the renderer-owned D3D11VA AVHWDeviceContext.
     * Caller must av_buffer_ref() if it intends to outlive the renderer.
     * Returns NULL if hardware acceleration could not be initialised. */
    AVBufferRef *get_hw_device_ctx() const;

    /* Player-facing configuration */
    void set_default_window_size(int screen_width, int screen_height,
                                 int width, int height, AVRational sar);
    int  get_supported_pixel_formats(enum AVPixelFormat *out_fmts,
                                     int max_fmts) const;
    int  open(int *width, int *height);
    void draw_video(AVFrame *frame, AVSubtitle *subtitle,
                    int xleft, int ytop, int width, int height);
    void clear();
    void present();
    void cleanup_textures();
    void resize(int width, int height);

    /* True after init() has created the D3D11 device. */
    bool is_device_created() const { return device != nullptr; }

    /* Raw device / context access needed by ImGui_ImplDX11_Init. */
    ID3D11Device        *d3d_device()  const { return device.Get(); }
    ID3D11DeviceContext *d3d_context() const { return context.Get(); }

    /* --- publicly visible configuration --- */
    HWND hwnd            = nullptr;
    int  default_width   = 640;
    int  default_height  = 480;

private:
    /* ------------------------------------------------------------------ */
    /* D3D11 core objects                                                 */
    /* ------------------------------------------------------------------ */
    ComPtr<ID3D11Device>           device;
    ComPtr<ID3D11DeviceContext>    context;
    ComPtr<IDXGISwapChain>         swap_chain;
    ComPtr<ID3D11RenderTargetView> rtv;

    /* HW acceleration: shared with libavcodec D3D11VA decoder.
     * Owned by the renderer; the application forwards a borrowed
     * reference to FFPlayer for zero-copy decoding. */
    AVBufferRef                    *hw_device_ref = nullptr;

    /* Pipeline state shared by BGRA / NV12 paths */
    ComPtr<ID3D11VertexShader>     vertex_shader;
    ComPtr<ID3D11PixelShader>      pixel_shader;         /* BGRA sampling  */
    ComPtr<ID3D11PixelShader>      pixel_shader_nv12;    /* NV12 -> RGB    */
    ComPtr<ID3D11InputLayout>      input_layout;
    ComPtr<ID3D11SamplerState>     sampler;
    ComPtr<ID3D11Buffer>           vertex_buffer;
    ComPtr<ID3D11BlendState>       blend_state;

    /* SW BGRA upload texture (sw decode + sws -> BGRA) */
    ComPtr<ID3D11Texture2D>        vid_texture;
    ComPtr<ID3D11ShaderResourceView> vid_srv;
    int                             vid_tex_width  = 0;
    int                             vid_tex_height = 0;

    /* HW NV12-family (NV12/P010/P016) SRV cache, lazily populated per
     * array slice. Invalidated when the underlying decoder texture array
     * changes (e.g. resolution / hwframes pool reallocation, or a
     * different stream is opened). The ID3D11Texture2D is a *borrowed*
     * reference from the decoder — we never AddRef/Release it here. */
    ID3D11Texture2D                *nv12_array        = nullptr;
    DXGI_FORMAT                     nv12_array_format = DXGI_FORMAT_UNKNOWN;
    ID3D11ShaderResourceView      **nv12_srv_y        = nullptr;
    ID3D11ShaderResourceView      **nv12_srv_uv       = nullptr;
    int                             nv12_array_size   = 0;

    /* Color-management constant buffer + memoized state. Bound to
     * b0 of the NV12/P010 pixel shader. */
    ComPtr<ID3D11Buffer>           color_cb;
    RendererColorState              color_state{};
    int                             color_target       = 0;
    int                             color_state_dirty  = 1;

    /* Subtitle texture (BGRA with alpha) */
    ComPtr<ID3D11Texture2D>        sub_texture;
    ComPtr<ID3D11ShaderResourceView> sub_srv;
    int                             sub_tex_width  = 0;
    int                             sub_tex_height = 0;
    struct SwsContext              *sub_convert_ctx = nullptr;

    /* Frame-tracking to avoid redundant uploads (SW path) */
    const uint8_t                  *last_vid_data = nullptr;
    int                             last_flip_v   = 0;

    /* Vertex buffer data for positioning the quad */
    float                           target_rect[4]{};

    /* ------------------------------------------------------------------ */
    /* Private helpers                                                    */
    /* ------------------------------------------------------------------ */
    void create_rtv();
    int  ensure_texture(ComPtr<ID3D11Texture2D> &tex,
                        ComPtr<ID3D11ShaderResourceView> &srv,
                        int &cur_w, int &cur_h,
                        int new_w, int new_h, int init_zero);
    void release_nv12_srv_cache();
    int  ensure_nv12_srvs(ID3D11Texture2D *tex, UINT slice,
                          ID3D11ShaderResourceView **out_y,
                          ID3D11ShaderResourceView **out_uv);
    int  upload_bgra_frame(AVFrame *frame);
    void update_color_params(const AVFrame *frame);
    void update_vertex_buffer(const void *quad_data);
    int  create_hw_device_ref();
};

#endif

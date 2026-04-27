#include <limits.h>
#include <math.h>
#include <string.h>
#include <stdio.h>

#include <initguid.h>
#include <d3dcompiler.h>

#include <libavutil/common.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixdesc.h>

#include "video_renderer_d3d11.h"

/* ------------------------------------------------------------------ */
/* Embedded HLSL shaders (compiled at runtime via D3DCompile)         */
/* ------------------------------------------------------------------ */

static const char s_shader_src[] =
    "struct VS_INPUT  { float2 pos : POSITION; float2 uv : TEXCOORD0; };\n"
    "struct PS_INPUT  { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };\n"
    "Texture2D    g_tex     : register(t0);\n"
    "SamplerState g_sampler : register(s0);\n"
    "PS_INPUT VSMain(VS_INPUT input) {\n"
    "    PS_INPUT o;\n"
    "    o.pos = float4(input.pos, 0.0f, 1.0f);\n"
    "    o.uv  = input.uv;\n"
    "    return o;\n"
    "}\n"
    "float4 PSMain(PS_INPUT input) : SV_TARGET {\n"
    "    return g_tex.Sample(g_sampler, input.uv);\n"
    "}\n";

/* Quad vertex: position (NDC) + texcoord */
typedef struct Vertex {
    float x, y;
    float u, v;
} Vertex;

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

#define SAFE_RELEASE(p) do { if (p) { (p)->lpVtbl->Release(p); (p) = NULL; } } while (0)

static void calculate_display_rect(int out_rect[4],
                                   int scr_xleft, int scr_ytop,
                                   int scr_width, int scr_height,
                                   int pic_width, int pic_height,
                                   AVRational pic_sar)
{
    AVRational aspect_ratio = pic_sar;
    int64_t width, height, x, y;

    if (av_cmp_q(aspect_ratio, av_make_q(0, 1)) <= 0)
        aspect_ratio = av_make_q(1, 1);
    aspect_ratio = av_mul_q(aspect_ratio, av_make_q(pic_width, pic_height));

    height = scr_height;
    width = av_rescale(height, aspect_ratio.num, aspect_ratio.den) & ~1;
    if (width > scr_width) {
        width = scr_width;
        height = av_rescale(width, aspect_ratio.den, aspect_ratio.num) & ~1;
    }
    x = (scr_width - width) / 2;
    y = (scr_height - height) / 2;
    out_rect[0] = scr_xleft + (int)x;
    out_rect[1] = scr_ytop  + (int)y;
    out_rect[2] = FFMAX((int)width,  1);
    out_rect[3] = FFMAX((int)height, 1);
}

static void rect_to_ndc(int rect[4], int vp_w, int vp_h, Vertex quad[4])
{
    /* Convert pixel rect to NDC [-1,+1] with y-up */
    float x0 = (float)rect[0] / (float)vp_w * 2.0f - 1.0f;
    float y0 = 1.0f - (float)rect[1] / (float)vp_h * 2.0f;
    float x1 = (float)(rect[0] + rect[2]) / (float)vp_w * 2.0f - 1.0f;
    float y1 = 1.0f - (float)(rect[1] + rect[3]) / (float)vp_h * 2.0f;

    /* Two-triangle strip: TL, TR, BL, BR */
    quad[0] = (Vertex){ x0, y0, 0.0f, 0.0f };
    quad[1] = (Vertex){ x1, y0, 1.0f, 0.0f };
    quad[2] = (Vertex){ x0, y1, 0.0f, 1.0f };
    quad[3] = (Vertex){ x1, y1, 1.0f, 1.0f };
}

static void update_vertex_buffer(VideoRendererD3D11 *vr, Vertex quad[4])
{
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = vr->context->lpVtbl->Map(vr->context,
        (ID3D11Resource *)vr->vertex_buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr)) {
        memcpy(mapped.pData, quad, sizeof(Vertex) * 4);
        vr->context->lpVtbl->Unmap(vr->context, (ID3D11Resource *)vr->vertex_buffer, 0);
    }
}

/* ------------------------------------------------------------------ */
/* Create / recreate the render target view after swap-chain resize    */
/* ------------------------------------------------------------------ */

static void create_rtv(VideoRendererD3D11 *vr)
{
    ID3D11Texture2D *back_buffer = NULL;
    SAFE_RELEASE(vr->rtv);
    vr->swap_chain->lpVtbl->GetBuffer(vr->swap_chain, 0,
        &IID_ID3D11Texture2D, (void **)&back_buffer);
    if (back_buffer) {
        vr->device->lpVtbl->CreateRenderTargetView(vr->device,
            (ID3D11Resource *)back_buffer, NULL, &vr->rtv);
        back_buffer->lpVtbl->Release(back_buffer);
    }
}

/* ------------------------------------------------------------------ */
/* Texture helpers                                                    */
/* ------------------------------------------------------------------ */

static int ensure_texture(VideoRendererD3D11 *vr,
                          ID3D11Texture2D **tex, ID3D11ShaderResourceView **srv,
                          int *cur_w, int *cur_h,
                          int new_w, int new_h, int init_zero)
{
    if (*tex && *cur_w == new_w && *cur_h == new_h)
        return 0;

    SAFE_RELEASE(*srv);
    SAFE_RELEASE(*tex);

    D3D11_TEXTURE2D_DESC td = {0};
    td.Width = new_w;
    td.Height = new_h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DYNAMIC;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    HRESULT hr = vr->device->lpVtbl->CreateTexture2D(vr->device, &td, NULL, tex);
    if (FAILED(hr))
        return -1;

    hr = vr->device->lpVtbl->CreateShaderResourceView(vr->device,
            (ID3D11Resource *)*tex, NULL, srv);
    if (FAILED(hr)) {
        SAFE_RELEASE(*tex);
        return -1;
    }

    *cur_w = new_w;
    *cur_h = new_h;

    if (init_zero) {
        D3D11_MAPPED_SUBRESOURCE m;
        hr = vr->context->lpVtbl->Map(vr->context,
                (ID3D11Resource *)*tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
        if (SUCCEEDED(hr)) {
            for (int y = 0; y < new_h; y++)
                memset((uint8_t *)m.pData + y * m.RowPitch, 0, new_w * 4);
            vr->context->lpVtbl->Unmap(vr->context, (ID3D11Resource *)*tex, 0);
        }
    }
    return 0;
}

static int upload_bgra_frame(VideoRendererD3D11 *vr, AVFrame *frame)
{
    if (ensure_texture(vr, &vr->vid_texture, &vr->vid_srv,
                       &vr->vid_tex_width, &vr->vid_tex_height,
                       frame->width, frame->height, 0) < 0)
        return -1;

    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = vr->context->lpVtbl->Map(vr->context,
        (ID3D11Resource *)vr->vid_texture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr))
        return -1;

    const uint8_t *src = frame->data[0];
    int src_pitch = frame->linesize[0];
    int flip = src_pitch < 0;
    if (flip) {
        src = src + src_pitch * (frame->height - 1);
        src_pitch = -src_pitch;
    }

    int copy_bytes = frame->width * 4;
    for (int y = 0; y < frame->height; y++) {
        int src_y = flip ? (frame->height - 1 - y) : y;
        memcpy((uint8_t *)mapped.pData + y * mapped.RowPitch,
               frame->data[0] + frame->linesize[0] * (flip ? (frame->height - 1 - y) : y),
               copy_bytes);
    }

    vr->context->lpVtbl->Unmap(vr->context, (ID3D11Resource *)vr->vid_texture, 0);
    vr->last_vid_data = frame->data[0];
    vr->last_flip_v = flip;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Init / Shutdown                                                    */
/* ------------------------------------------------------------------ */

int video_renderer_d3d11_init(VideoRendererD3D11 *vr, HWND hwnd)
{
    HRESULT hr;
    memset(vr, 0, sizeof(*vr));
    vr->hwnd = hwnd;

    /* Create device + swap chain */
    DXGI_SWAP_CHAIN_DESC scd = {0};
    scd.BufferCount = 2;
    scd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hwnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT flags = 0;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL feature_level;
    hr = D3D11CreateDeviceAndSwapChain(
        NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, flags, NULL, 0,
        D3D11_SDK_VERSION, &scd, &vr->swap_chain,
        &vr->device, &feature_level, &vr->context);
    if (FAILED(hr))
        return -1;

    create_rtv(vr);

    /* Compile shaders */
    ID3DBlob *vs_blob = NULL, *ps_blob = NULL, *err_blob = NULL;
    hr = D3DCompile(s_shader_src, strlen(s_shader_src), NULL, NULL, NULL,
                    "VSMain", "vs_4_0", 0, 0, &vs_blob, &err_blob);
    if (FAILED(hr)) { SAFE_RELEASE(err_blob); return -1; }

    hr = D3DCompile(s_shader_src, strlen(s_shader_src), NULL, NULL, NULL,
                    "PSMain", "ps_4_0", 0, 0, &ps_blob, &err_blob);
    if (FAILED(hr)) { SAFE_RELEASE(vs_blob); SAFE_RELEASE(err_blob); return -1; }

    hr = vr->device->lpVtbl->CreateVertexShader(vr->device,
            vs_blob->lpVtbl->GetBufferPointer(vs_blob),
            vs_blob->lpVtbl->GetBufferSize(vs_blob), NULL, &vr->vertex_shader);
    if (FAILED(hr)) goto shader_fail;

    hr = vr->device->lpVtbl->CreatePixelShader(vr->device,
            ps_blob->lpVtbl->GetBufferPointer(ps_blob),
            ps_blob->lpVtbl->GetBufferSize(ps_blob), NULL, &vr->pixel_shader);
    if (FAILED(hr)) goto shader_fail;

    /* Input layout */
    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,   0, 0,                            D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,   0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    hr = vr->device->lpVtbl->CreateInputLayout(vr->device, layout, 2,
            vs_blob->lpVtbl->GetBufferPointer(vs_blob),
            vs_blob->lpVtbl->GetBufferSize(vs_blob), &vr->input_layout);
    if (FAILED(hr)) goto shader_fail;

    SAFE_RELEASE(vs_blob);
    SAFE_RELEASE(ps_blob);

    /* Vertex buffer (dynamic, 4 vertices for triangle strip) */
    {
        D3D11_BUFFER_DESC bd = {0};
        bd.ByteWidth = sizeof(Vertex) * 4;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = vr->device->lpVtbl->CreateBuffer(vr->device, &bd, NULL, &vr->vertex_buffer);
        if (FAILED(hr)) return -1;
    }

    /* Sampler */
    {
        D3D11_SAMPLER_DESC sd = {0};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        hr = vr->device->lpVtbl->CreateSamplerState(vr->device, &sd, &vr->sampler);
        if (FAILED(hr)) return -1;
    }

    /* Blend state for subtitle overlay (premultiplied alpha) */
    {
        D3D11_BLEND_DESC bd = {0};
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        hr = vr->device->lpVtbl->CreateBlendState(vr->device, &bd, &vr->blend_state);
        if (FAILED(hr)) return -1;
    }

    return 0;

shader_fail:
    SAFE_RELEASE(vs_blob);
    SAFE_RELEASE(ps_blob);
    SAFE_RELEASE(err_blob);
    return -1;
}

void video_renderer_d3d11_shutdown(VideoRendererD3D11 *vr)
{
    if (!vr)
        return;
    video_renderer_d3d11_cleanup_textures(vr);
    SAFE_RELEASE(vr->blend_state);
    SAFE_RELEASE(vr->sampler);
    SAFE_RELEASE(vr->vertex_buffer);
    SAFE_RELEASE(vr->input_layout);
    SAFE_RELEASE(vr->pixel_shader);
    SAFE_RELEASE(vr->vertex_shader);
    SAFE_RELEASE(vr->rtv);
    SAFE_RELEASE(vr->swap_chain);
    SAFE_RELEASE(vr->context);
    SAFE_RELEASE(vr->device);
    memset(vr, 0, sizeof(*vr));
}

/* ------------------------------------------------------------------ */
/* Public API — mirrors video_renderer.c for SDL2                     */
/* ------------------------------------------------------------------ */

void video_renderer_d3d11_set_default_window_size(VideoRendererD3D11 *vr,
                                                   int screen_width, int screen_height,
                                                   int width, int height, AVRational sar)
{
    int rect[4];
    int max_width  = screen_width > 0  ? screen_width  : INT_MAX;
    int max_height = screen_height > 0 ? screen_height : INT_MAX;
    if (max_width == INT_MAX && max_height == INT_MAX)
        max_height = height;
    calculate_display_rect(rect, 0, 0, max_width, max_height, width, height, sar);
    vr->default_width  = rect[2];
    vr->default_height = rect[3];
}

int video_renderer_d3d11_get_supported_pixel_formats(const VideoRendererD3D11 *vr,
                                                     enum AVPixelFormat *out_fmts,
                                                     int max_fmts)
{
    (void)vr;
    if (!out_fmts || max_fmts <= 0)
        return 0;
    /* D3D11 backend: we only accept BGRA — swscale converts everything else */
    out_fmts[0] = AV_PIX_FMT_BGRA;
    return 1;
}

int video_renderer_d3d11_open(VideoRendererD3D11 *vr, int *width, int *height)
{
    int w = (*width > 0)  ? *width  : vr->default_width;
    int h = (*height > 0) ? *height : vr->default_height;

    RECT rc = { 0, 0, w, h };
    AdjustWindowRect(&rc, GetWindowLong(vr->hwnd, GWL_STYLE), FALSE);
    SetWindowPos(vr->hwnd, NULL, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                 SWP_NOMOVE | SWP_NOZORDER);

    *width  = w;
    *height = h;
    return 0;
}

void video_renderer_d3d11_draw_video(VideoRendererD3D11 *vr,
                                     AVFrame *frame, AVSubtitle *subtitle,
                                     int xleft, int ytop, int width, int height)
{
    if (!frame || !vr->context)
        return;

    /* Upload video frame if data changed */
    if (vr->last_vid_data != frame->data[0]) {
        if (upload_bgra_frame(vr, frame) < 0)
            return;
    }

    /* Calculate display rect */
    int rect[4];
    calculate_display_rect(rect, xleft, ytop, width, height,
                           frame->width, frame->height, frame->sample_aspect_ratio);

    /* Draw the video quad (no blending) */
    Vertex quad[4];
    rect_to_ndc(rect, width, height, quad);
    update_vertex_buffer(vr, quad);

    UINT stride = sizeof(Vertex), offset = 0;
    vr->context->lpVtbl->IASetVertexBuffers(vr->context, 0, 1, &vr->vertex_buffer, &stride, &offset);
    vr->context->lpVtbl->IASetInputLayout(vr->context, vr->input_layout);
    vr->context->lpVtbl->IASetPrimitiveTopology(vr->context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    vr->context->lpVtbl->VSSetShader(vr->context, vr->vertex_shader, NULL, 0);
    vr->context->lpVtbl->PSSetShader(vr->context, vr->pixel_shader, NULL, 0);
    vr->context->lpVtbl->PSSetSamplers(vr->context, 0, 1, &vr->sampler);
    vr->context->lpVtbl->PSSetShaderResources(vr->context, 0, 1, &vr->vid_srv);

    /* No blending for video */
    float blend_factor[4] = { 0, 0, 0, 0 };
    vr->context->lpVtbl->OMSetBlendState(vr->context, NULL, blend_factor, 0xFFFFFFFF);
    vr->context->lpVtbl->Draw(vr->context, 4, 0);

    /* Subtitle overlay */
    if (subtitle && subtitle->num_rects > 0 && vr->blend_state) {
        if (ensure_texture(vr, &vr->sub_texture, &vr->sub_srv,
                           &vr->sub_tex_width, &vr->sub_tex_height,
                           frame->width, frame->height, 1) == 0) {
            /* Convert and upload subtitle rects */
            D3D11_MAPPED_SUBRESOURCE mapped;
            HRESULT hr = vr->context->lpVtbl->Map(vr->context,
                (ID3D11Resource *)vr->sub_texture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
            if (SUCCEEDED(hr)) {
                /* Clear to transparent */
                for (int y = 0; y < frame->height; y++)
                    memset((uint8_t *)mapped.pData + y * mapped.RowPitch, 0, frame->width * 4);

                for (int i = 0; i < (int)subtitle->num_rects; i++) {
                    AVSubtitleRect *sr = subtitle->rects[i];
                    int sx = av_clip(sr->x, 0, frame->width);
                    int sy = av_clip(sr->y, 0, frame->height);
                    int sw = av_clip(sr->w, 0, frame->width - sx);
                    int sh = av_clip(sr->h, 0, frame->height - sy);

                    vr->sub_convert_ctx = sws_getCachedContext(vr->sub_convert_ctx,
                        sw, sh, AV_PIX_FMT_PAL8,
                        sw, sh, AV_PIX_FMT_BGRA,
                        0, NULL, NULL, NULL);
                    if (!vr->sub_convert_ctx)
                        continue;

                    uint8_t *dst = (uint8_t *)mapped.pData + sy * mapped.RowPitch + sx * 4;
                    int dst_pitch = mapped.RowPitch;
                    sws_scale(vr->sub_convert_ctx,
                              (const uint8_t * const *)sr->data, sr->linesize,
                              0, sh, &dst, &dst_pitch);
                }
                vr->context->lpVtbl->Unmap(vr->context, (ID3D11Resource *)vr->sub_texture, 0);
            }

            /* Draw subtitle quad with alpha blending */
            vr->context->lpVtbl->OMSetBlendState(vr->context, vr->blend_state, blend_factor, 0xFFFFFFFF);
            vr->context->lpVtbl->PSSetShaderResources(vr->context, 0, 1, &vr->sub_srv);
            vr->context->lpVtbl->Draw(vr->context, 4, 0);
            vr->context->lpVtbl->OMSetBlendState(vr->context, NULL, blend_factor, 0xFFFFFFFF);
        }
    }

    /* Unbind SRV to avoid warnings */
    ID3D11ShaderResourceView *null_srv = NULL;
    vr->context->lpVtbl->PSSetShaderResources(vr->context, 0, 1, &null_srv);
}

void video_renderer_d3d11_clear(VideoRendererD3D11 *vr)
{
    if (!vr || !vr->context || !vr->rtv)
        return;
    float clear_color[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    vr->context->lpVtbl->ClearRenderTargetView(vr->context, vr->rtv, clear_color);

    /* Set render target + viewport */
    vr->context->lpVtbl->OMSetRenderTargets(vr->context, 1, &vr->rtv, NULL);

    RECT rc;
    GetClientRect(vr->hwnd, &rc);
    D3D11_VIEWPORT vp = { 0.0f, 0.0f, (float)(rc.right - rc.left), (float)(rc.bottom - rc.top), 0.0f, 1.0f };
    vr->context->lpVtbl->RSSetViewports(vr->context, 1, &vp);
}

void video_renderer_d3d11_present(VideoRendererD3D11 *vr)
{
    if (!vr || !vr->swap_chain)
        return;
    vr->swap_chain->lpVtbl->Present(vr->swap_chain, 1, 0); /* VSync */
}

void video_renderer_d3d11_cleanup_textures(VideoRendererD3D11 *vr)
{
    if (!vr)
        return;
    sws_freeContext(vr->sub_convert_ctx);
    vr->sub_convert_ctx = NULL;
    SAFE_RELEASE(vr->vid_srv);
    SAFE_RELEASE(vr->vid_texture);
    vr->vid_tex_width = 0;
    vr->vid_tex_height = 0;
    SAFE_RELEASE(vr->sub_srv);
    SAFE_RELEASE(vr->sub_texture);
    vr->sub_tex_width = 0;
    vr->sub_tex_height = 0;
    vr->last_vid_data = NULL;
    vr->last_flip_v = 0;
}

void video_renderer_d3d11_resize(VideoRendererD3D11 *vr, int width, int height)
{
    if (!vr || !vr->swap_chain || width <= 0 || height <= 0)
        return;
    SAFE_RELEASE(vr->rtv);
    vr->swap_chain->lpVtbl->ResizeBuffers(vr->swap_chain, 0,
        (UINT)width, (UINT)height, DXGI_FORMAT_UNKNOWN, 0);
    create_rtv(vr);
}

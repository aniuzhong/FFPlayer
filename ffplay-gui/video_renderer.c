#include <limits.h>
#include <math.h>
#include <string.h>
#include <stdio.h>

#include <initguid.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>

#include <libavutil/common.h>
#include <libavutil/frame.h>
#include <libavutil/log.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>

#include "video_renderer.h"

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

/* NV12 / P010 / P016 sampling shader with full color-management.
 *
 * Pipeline per pixel:
 *   1. Sample Y/UV planes (8 or 16-bit UNORM views, normalized to [0,1])
 *   2. Limited / full range expansion using cbuffer.range (y_off / y_scale / c_off / c_scale)
 *   3. YCbCr -> RGB via cbuffer.yuv2rgb (3x3 packed in 3 float4 with .w as offset)
 *   4. Inverse EOTF -> linear light, normalized so 1.0 == 10000 nits (PQ) or 1.0 == 1.0 (BT.709/sRGB)
 *   5. Color primary conversion via cbuffer.prim2tgt (BT.2020 -> BT.709 etc.)
 *   6. BT.2390 EETF tone-map (PQ source -> SDR target peak); skipped for SDR sources
 *   7. Encode to display: sRGB for SDR target (HDR10 PQ encode is ready for Step 2)
 *
 * The same shader works for SDR BT.709 8-bit content because the matrices
 * and TRC selectors degenerate (BT.709 -> BT.709 is identity, gamma 2.4
 * EOTF skips the whole HDR pipeline).
 */
static const char s_shader_nv12_src[] =
    "struct PS_INPUT { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };\n"
    "Texture2D<float>  g_y  : register(t0);\n"
    "Texture2D<float2> g_uv : register(t1);\n"
    "SamplerState g_sampler : register(s0);\n"
    "\n"
    "cbuffer ColorParams : register(b0) {\n"
    "    float4 yuv_r;     /* row 0: ay,au,av,offset_y */\n"
    "    float4 yuv_g;     /* row 1                       */\n"
    "    float4 yuv_b;     /* row 2                       */\n"
    "    float4 prim_r;    /* RGB primary 3x3, .w padding */\n"
    "    float4 prim_g;\n"
    "    float4 prim_b;\n"
    "    float4 range;     /* y_off, y_scale, c_off, c_scale (full-range expansion) */\n"
    "    float4 luma;      /* peak_nits, sdr_white_nits, knee_start_pq, _ */\n"
    "    int4   modes;     /* trc, primaries_match, target_kind, _ */\n"
    "};\n"
    "\n"
    "/* ST.2084 PQ EOTF: non-linear [0,1] -> linear [0,1] (1.0 == 10000 nits) */\n"
    "static const float PQ_M1 = 0.1593017578125;\n"
    "static const float PQ_M2 = 78.84375;\n"
    "static const float PQ_C1 = 0.8359375;\n"
    "static const float PQ_C2 = 18.8515625;\n"
    "static const float PQ_C3 = 18.6875;\n"
    "float pq_eotf(float E) {\n"
    "    float Em = pow(max(E, 0.0), 1.0 / PQ_M2);\n"
    "    float num = max(Em - PQ_C1, 0.0);\n"
    "    float den = PQ_C2 - PQ_C3 * Em;\n"
    "    return pow(num / max(den, 1e-6), 1.0 / PQ_M1);\n"
    "}\n"
    "float pq_inv_eotf(float L) {\n"
    "    float Lm = pow(max(L, 0.0), PQ_M1);\n"
    "    return pow((PQ_C1 + PQ_C2 * Lm) / (1.0 + PQ_C3 * Lm), PQ_M2);\n"
    "}\n"
    "\n"
    "/* HLG inverse OETF (per ITU-R BT.2100). Output is scene linear in [0,12]. */\n"
    "float hlg_inv_oetf(float E) {\n"
    "    static const float HLG_a = 0.17883277;\n"
    "    static const float HLG_b = 0.28466892;\n"
    "    static const float HLG_c = 0.55991073;\n"
    "    if (E <= 0.5) return (E * E) / 3.0;\n"
    "    return (exp((E - HLG_c) / HLG_a) + HLG_b) / 12.0;\n"
    "}\n"
    "\n"
    "/* BT.709 / SMPTE-170M gamma-2.4 EOTF (close enough for both). */\n"
    "float bt709_eotf(float E) {\n"
    "    return (E < 0.081) ? (E / 4.5) : pow((E + 0.099) / 1.099, 1.0 / 0.45);\n"
    "}\n"
    "\n"
    "/* BT.2390-10 §5.4 EETF in normalized PQ space: maps source PQ values\n"
    "   in [0,1] (1.0 == source mastering peak) to display PQ values in\n"
    "   [0, max_pq] using a Hermite spline knee-curve. */\n"
    "float bt2390_eetf(float E1, float max_pq) {\n"
    "    float ks = 1.5 * max_pq - 0.5;\n"
    "    if (E1 < ks) return E1;\n"
    "    float t  = (E1 - ks) / max(1.0 - ks, 1e-6);\n"
    "    float t2 = t * t;\n"
    "    float t3 = t2 * t;\n"
    "    return (2.0 * t3 - 3.0 * t2 + 1.0) * ks +\n"
    "           (t3 - 2.0 * t2 + t) * (1.0 - ks) +\n"
    "           (-2.0 * t3 + 3.0 * t2) * max_pq;\n"
    "}\n"
    "\n"
    "/* Linear -> sRGB encode (IEC 61966-2-1). */\n"
    "float linear_to_srgb(float L) {\n"
    "    L = saturate(L);\n"
    "    return (L <= 0.0031308) ? (12.92 * L) : (1.055 * pow(L, 1.0 / 2.4) - 0.055);\n"
    "}\n"
    "\n"
    "float4 PSMainNV12(PS_INPUT input) : SV_TARGET {\n"
    "    float  y  = g_y.Sample(g_sampler,  input.uv).r;\n"
    "    float2 cc = g_uv.Sample(g_sampler, input.uv).rg;\n"
    "\n"
    "    /* 1) Range expansion (limited -> full or pass-through for full). */\n"
    "    float y_full  = (y    - range.x) * range.y;\n"
    "    float cb_full = (cc.x - range.z) * range.w;\n"
    "    float cr_full = (cc.y - range.z) * range.w;\n"
    "    float4 yuvw = float4(y_full, cb_full, cr_full, 1.0);\n"
    "\n"
    "    /* 2) YCbCr -> RGB (matrix premultiplies offset in .w). */\n"
    "    float3 rgb_nl = float3(dot(yuv_r, yuvw),\n"
    "                           dot(yuv_g, yuvw),\n"
    "                           dot(yuv_b, yuvw));\n"
    "\n"
    "    /* 3) Inverse EOTF -> linear light (normalized, 1.0 == 10000 nits for PQ,\n"
    "        1.0 == ~1.0 for BT.709/sRGB). */\n"
    "    float3 rgb_lin;\n"
    "    int trc = modes.x;\n"
    "    if (trc == 1) {\n"
    "        /* PQ: rgb_lin in [0,1] where 1.0 == 10000 nits */\n"
    "        rgb_lin = float3(pq_eotf(rgb_nl.r), pq_eotf(rgb_nl.g), pq_eotf(rgb_nl.b));\n"
    "    } else if (trc == 2) {\n"
    "        /* HLG scene-linear; we approximate the OOTF gamma=1.2 reference scene-to-display. */\n"
    "        float3 sl = float3(hlg_inv_oetf(rgb_nl.r), hlg_inv_oetf(rgb_nl.g), hlg_inv_oetf(rgb_nl.b));\n"
    "        float Y = dot(sl, float3(0.2627, 0.6780, 0.0593));\n"
    "        rgb_lin = sl * pow(max(Y, 1e-6), 0.2);\n"
    "        /* HLG nominal peak in our normalized space (= 1000 nit / 10000 nit). */\n"
    "        rgb_lin *= 0.1;\n"
    "    } else if (trc == 3) {\n"
    "        rgb_lin = rgb_nl;\n"
    "    } else {\n"
    "        rgb_lin = float3(bt709_eotf(rgb_nl.r), bt709_eotf(rgb_nl.g), bt709_eotf(rgb_nl.b));\n"
    "    }\n"
    "\n"
    "    /* 4) Color primary conversion (BT.2020 -> BT.709 etc.). */\n"
    "    if (modes.y == 0) {\n"
    "        float3 conv = float3(dot(prim_r.rgb, rgb_lin),\n"
    "                             dot(prim_g.rgb, rgb_lin),\n"
    "                             dot(prim_b.rgb, rgb_lin));\n"
    "        rgb_lin = conv;\n"
    "    }\n"
    "\n"
    "    /* 5) Tone-map HDR -> SDR (only for PQ/HLG sources targeting SDR).\n"
    "        We work in normalized PQ space per BT.2390 §5: encode linear\n"
    "        light to absolute PQ, normalize so 1.0 == source mastering peak,\n"
    "        run the EETF, then de-normalize and decode back to linear. */\n"
    "    int target = modes.z;\n"
    "    if (target == 0 && (trc == 1 || trc == 2)) {\n"
    "        float peak    = max(luma.x, 1.0);\n"
    "        float sdr     = max(luma.y, 1.0);\n"
    "        float src_pq  = pq_inv_eotf(min(peak / 10000.0, 1.0));\n"
    "        float dst_pq  = pq_inv_eotf(min(sdr  / 10000.0, 1.0));\n"
    "        float max_pq  = saturate(dst_pq / max(src_pq, 1e-6));\n"
    "        float3 e_pq_abs = float3(pq_inv_eotf(rgb_lin.r),\n"
    "                                 pq_inv_eotf(rgb_lin.g),\n"
    "                                 pq_inv_eotf(rgb_lin.b));\n"
    "        float3 e1 = saturate(e_pq_abs / max(src_pq, 1e-6));\n"
    "        float3 e2 = float3(bt2390_eetf(e1.r, max_pq),\n"
    "                           bt2390_eetf(e1.g, max_pq),\n"
    "                           bt2390_eetf(e1.b, max_pq));\n"
    "        float3 e_pq_out = e2 * src_pq;\n"
    "        rgb_lin = float3(pq_eotf(e_pq_out.r),\n"
    "                         pq_eotf(e_pq_out.g),\n"
    "                         pq_eotf(e_pq_out.b));\n"
    "        rgb_lin *= (10000.0 / max(sdr, 1.0));\n"
    "    }\n"
    "\n"
    "    /* 6) Output encode. SDR -> sRGB. (HDR10 PQ output is the Step-2 path.) */\n"
    "    float3 out_rgb = saturate(rgb_lin);\n"
    "    if (target == 0) {\n"
    "        out_rgb = float3(linear_to_srgb(out_rgb.r),\n"
    "                         linear_to_srgb(out_rgb.g),\n"
    "                         linear_to_srgb(out_rgb.b));\n"
    "    } else if (target == 1) {\n"
    "        /* HDR10 PQ on the BT.2020 primaries the converted pixel already lives in. */\n"
    "        out_rgb = float3(pq_inv_eotf(out_rgb.r),\n"
    "                         pq_inv_eotf(out_rgb.g),\n"
    "                         pq_inv_eotf(out_rgb.b));\n"
    "    }\n"
    "    return float4(out_rgb, 1.0);\n"
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

/* ------------------------------------------------------------------ */
/* Color-management constant buffer.                                   */
/*                                                                    */
/* Layout MUST match `cbuffer ColorParams : register(b0)` in the      */
/* NV12/P010 shader; HLSL packs each row on a 16-byte boundary.       */
/* ------------------------------------------------------------------ */
typedef struct ColorParamsCB {
    float yuv_r[4];   /* R = dot(yuv_r, float4(y,cb,cr,1)) */
    float yuv_g[4];
    float yuv_b[4];
    float prim_r[4];  /* RGB primary 3x3, .w padding */
    float prim_g[4];
    float prim_b[4];
    float range[4];   /* y_off, y_scale, c_off, c_scale */
    float luma[4];    /* peak_nits, sdr_white_nits, _, _ */
    int   modes[4];   /* trc, primaries_match, target_kind, _ */
} ColorParamsCB;

/* Map AVColorTransferCharacteristic to the small enum the shader
 * understands (0=BT.709 gamma, 1=PQ/SMPTE2084, 2=HLG, 3=Linear). */
static int trc_to_shader(int trc)
{
    switch (trc) {
        case AVCOL_TRC_SMPTE2084:    return 1;
        case AVCOL_TRC_ARIB_STD_B67: return 2;
        case AVCOL_TRC_LINEAR:       return 3;
        default:                     return 0;
    }
}

/* Pick a YCbCr->RGB matrix from AVColorSpace; rows are 1-by-3 weights
 * acting on (y_full, cb_full, cr_full) where cb/cr are already in
 * [-0.5, 0.5]. The constant in .w stays zero — the shader applies any
 * range expansion itself before this matrix. */
static void fill_yuv_matrix(int colorspace, float yuv[3][4])
{
    float Kr, Kb;
    switch (colorspace) {
        case AVCOL_SPC_BT2020_NCL:
        case AVCOL_SPC_BT2020_CL:
            Kr = 0.2627f; Kb = 0.0593f; break;
        case AVCOL_SPC_BT470BG:
        case AVCOL_SPC_SMPTE170M:
            Kr = 0.299f;  Kb = 0.114f;  break;
        case AVCOL_SPC_BT709:
        default:
            Kr = 0.2126f; Kb = 0.0722f; break;
    }
    float Kg = 1.0f - Kr - Kb;
    yuv[0][0] = 1.0f; yuv[0][1] = 0.0f;            yuv[0][2] = 2.0f * (1.0f - Kr); yuv[0][3] = 0.0f;
    yuv[1][0] = 1.0f; yuv[1][1] = -2.0f * Kb * (1.0f - Kb) / Kg;
                      yuv[1][2] = -2.0f * Kr * (1.0f - Kr) / Kg;
                                                                                   yuv[1][3] = 0.0f;
    yuv[2][0] = 1.0f; yuv[2][1] = 2.0f * (1.0f - Kb); yuv[2][2] = 0.0f; yuv[2][3] = 0.0f;
}

/* True if the source primaries match the renderer's current output
 * primaries (BT.709 for SDR sRGB target, BT.2020 for HDR10 target).
 * Shader skips the primary-conversion matrix in that case. */
static int primaries_match_target(int color_primaries, int target)
{
    if (target == RENDERER_COLOR_TARGET_HDR10_PQ) {
        return color_primaries == AVCOL_PRI_BT2020;
    }
    /* SDR sRGB target. Treat unknown / BT.709 / BT.601 as "match" so
     * the cheap matrix is skipped — the small BT.601 hue shift is
     * imperceptible compared to running the full conversion path. */
    return color_primaries != AVCOL_PRI_BT2020;
}

/* BT.2020 RGB -> BT.709 RGB matrix (D65 white point, no chromatic
 * adaptation needed since both standards share D65). Used only when
 * the SDR path receives BT.2020 content. */
static void fill_primary_matrix_2020_to_709(float prim[3][4])
{
    static const float M[3][3] = {
        {  1.6604904f, -0.5876411f, -0.0728493f },
        { -0.1245505f,  1.1328999f, -0.0083494f },
        { -0.0181508f, -0.1005789f,  1.1187297f },
    };
    for (int i = 0; i < 3; i++) {
        prim[i][0] = M[i][0];
        prim[i][1] = M[i][1];
        prim[i][2] = M[i][2];
        prim[i][3] = 0.0f;
    }
}

/* Source mastering peak luminance in nits. Preference order:
 *   1) AV_FRAME_DATA_MASTERING_DISPLAY_METADATA::max_luminance
 *   2) AV_FRAME_DATA_CONTENT_LIGHT_LEVEL::MaxCLL
 *   3) Hard-coded 1000 nit (most common HDR10 master).
 * Only consulted for PQ/HLG sources; SDR rolls off the tone-map step. */
static float frame_peak_luminance_nits(const AVFrame *frame)
{
    if (!frame)
        return 1000.0f;
    AVFrameSideData *sd = av_frame_get_side_data((AVFrame *)frame,
                                                 AV_FRAME_DATA_MASTERING_DISPLAY_METADATA);
    if (sd && sd->size >= sizeof(AVMasteringDisplayMetadata)) {
        const AVMasteringDisplayMetadata *m = (const AVMasteringDisplayMetadata *)sd->data;
        if (m->has_luminance && m->max_luminance.den != 0) {
            double v = av_q2d(m->max_luminance);
            if (v > 1.0)
                return (float)v;
        }
    }
    sd = av_frame_get_side_data((AVFrame *)frame, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL);
    if (sd && sd->size >= sizeof(AVContentLightMetadata)) {
        const AVContentLightMetadata *cll = (const AVContentLightMetadata *)sd->data;
        if (cll->MaxCLL > 0)
            return (float)cll->MaxCLL;
    }
    return 1000.0f;
}

/* Detect whether a frame's color metadata changed enough to require
 * rebuilding the cbuffer. Avoids a Map/Unmap on every single frame. */
static int color_state_differs(const RendererColorState *a, const RendererColorState *b)
{
    return a->colorspace      != b->colorspace      ||
           a->color_primaries != b->color_primaries ||
           a->color_trc       != b->color_trc       ||
           a->color_range     != b->color_range     ||
           a->target          != b->target          ||
           a->peak_nits       != b->peak_nits       ||
           a->max_cll_nits    != b->max_cll_nits;
}

/* Refresh the renderer's color cbuffer from the supplied frame's
 * color metadata, but only when something changed. */
static void update_color_params(VideoRenderer *vr, const AVFrame *frame)
{
    if (!vr->color_cb || !frame)
        return;

    RendererColorState ns = {0};
    ns.colorspace      = frame->colorspace;
    ns.color_primaries = frame->color_primaries;
    ns.color_trc       = frame->color_trc;
    ns.color_range     = frame->color_range;
    ns.target          = vr->color_target;
    ns.peak_nits       = frame_peak_luminance_nits(frame);
    AVFrameSideData *sd = av_frame_get_side_data((AVFrame *)frame,
                                                 AV_FRAME_DATA_CONTENT_LIGHT_LEVEL);
    if (sd && sd->size >= sizeof(AVContentLightMetadata))
        ns.max_cll_nits = (float)((const AVContentLightMetadata *)sd->data)->MaxCLL;

    if (!vr->color_state_dirty && !color_state_differs(&vr->color_state, &ns))
        return;

    ColorParamsCB cb = {0};

    /* YCbCr -> RGB matrix from colorspace metadata. */
    float yuv[3][4];
    fill_yuv_matrix(ns.colorspace, yuv);
    memcpy(cb.yuv_r, yuv[0], sizeof(yuv[0]));
    memcpy(cb.yuv_g, yuv[1], sizeof(yuv[1]));
    memcpy(cb.yuv_b, yuv[2], sizeof(yuv[2]));

    /* Primary conversion (only used when shader sees primaries_match=0). */
    int prim_match = primaries_match_target(ns.color_primaries, ns.target);
    float prim[3][4] = {0};
    if (!prim_match && ns.target == RENDERER_COLOR_TARGET_SDR_SRGB)
        fill_primary_matrix_2020_to_709(prim);
    else
        for (int i = 0; i < 3; i++) prim[i][i] = 1.0f;
    memcpy(cb.prim_r, prim[0], sizeof(prim[0]));
    memcpy(cb.prim_g, prim[1], sizeof(prim[1]));
    memcpy(cb.prim_b, prim[2], sizeof(prim[2]));

    /* Range expansion: limited (16-235 / 16-240) vs full (0-255). */
    if (ns.color_range == AVCOL_RANGE_JPEG) {
        cb.range[0] = 0.0f;          /* y_off  */
        cb.range[1] = 1.0f;          /* y_scale*/
        cb.range[2] = 128.0f / 255.0f; /* c_off */
        cb.range[3] = 1.0f;          /* c_scale*/
    } else {
        cb.range[0] = 16.0f  / 255.0f;
        cb.range[1] = 255.0f / 219.0f;
        cb.range[2] = 128.0f / 255.0f;
        cb.range[3] = 255.0f / 224.0f;
    }

    cb.luma[0] = ns.peak_nits;     /* mastering peak */
    cb.luma[1] = 100.0f;           /* SDR white nits target */
    cb.luma[2] = 0.0f;
    cb.luma[3] = 0.0f;

    cb.modes[0] = trc_to_shader(ns.color_trc);
    cb.modes[1] = prim_match ? 1 : 0;
    cb.modes[2] = ns.target;
    cb.modes[3] = 0;

    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = vr->context->lpVtbl->Map(vr->context,
        (ID3D11Resource *)vr->color_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr)) {
        memcpy(mapped.pData, &cb, sizeof(cb));
        vr->context->lpVtbl->Unmap(vr->context, (ID3D11Resource *)vr->color_cb, 0);
        vr->color_state = ns;
        vr->color_state_dirty = 0;
    }
}

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

static void update_vertex_buffer(VideoRenderer *vr, Vertex quad[4])
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

static void create_rtv(VideoRenderer *vr)
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

static int ensure_texture(VideoRenderer *vr,
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

/* Drop the per-array-slice SRV cache built for the previous decoder
 * frames pool. Called when the underlying ID3D11Texture2D array
 * changes (e.g. the decoder reallocated its pool, or playback closed). */
static void release_nv12_srv_cache(VideoRenderer *vr)
{
    if (vr->nv12_srv_y) {
        for (int i = 0; i < vr->nv12_array_size; i++)
            SAFE_RELEASE(vr->nv12_srv_y[i]);
        av_freep(&vr->nv12_srv_y);
    }
    if (vr->nv12_srv_uv) {
        for (int i = 0; i < vr->nv12_array_size; i++)
            SAFE_RELEASE(vr->nv12_srv_uv[i]);
        av_freep(&vr->nv12_srv_uv);
    }
    vr->nv12_array = NULL;
    vr->nv12_array_size = 0;
    vr->nv12_array_format = DXGI_FORMAT_UNKNOWN;
}

/* Map an NV12-family decoder texture format to the per-plane SRV
 * formats we sample with. Y is single-channel UNORM, UV is two-channel
 * UNORM at half resolution; bit-depth follows the underlying surface
 * (NV12 -> 8-bit, P010/P016 -> 16-bit). For P010 the 10-bit data sits
 * in the high bits of the R16 channel, so an R16_UNORM view returns
 * floats scaled by 1023*64 / 65535 ~ 0.9990 of the true 10-bit value
 * which our BT.709 shader treats as a near-imperceptible luma dim. */
static int dxgi_nv12_plane_formats(DXGI_FORMAT tex_fmt,
                                   DXGI_FORMAT *out_y,
                                   DXGI_FORMAT *out_uv)
{
    switch (tex_fmt) {
        case DXGI_FORMAT_NV12:
            *out_y  = DXGI_FORMAT_R8_UNORM;
            *out_uv = DXGI_FORMAT_R8G8_UNORM;
            return 0;
        case DXGI_FORMAT_P010:
        case DXGI_FORMAT_P016:
            *out_y  = DXGI_FORMAT_R16_UNORM;
            *out_uv = DXGI_FORMAT_R16G16_UNORM;
            return 0;
        default:
            return -1;
    }
}

/* Lazily create one Y and one UV plane SRV per array slice on the
 * decoder's NV12-family texture array, so each decoded surface can be
 * sampled directly without any GPU-side copy. The SRV element formats
 * are chosen from the underlying texture's DXGI format so we transparently
 * support 8-bit (NV12) and 10/12-bit (P010/P016) HW frames. */
static int ensure_nv12_srvs(VideoRenderer *vr,
                            ID3D11Texture2D *tex, UINT slice,
                            ID3D11ShaderResourceView **out_y,
                            ID3D11ShaderResourceView **out_uv)
{
    HRESULT hr;

    if (vr->nv12_array != tex) {
        release_nv12_srv_cache(vr);
        D3D11_TEXTURE2D_DESC desc = {0};
        tex->lpVtbl->GetDesc(tex, &desc);
        if (desc.ArraySize == 0)
            return -1;

        DXGI_FORMAT fmt_y, fmt_uv;
        if (dxgi_nv12_plane_formats(desc.Format, &fmt_y, &fmt_uv) < 0) {
            av_log(NULL, AV_LOG_ERROR,
                   "Unsupported D3D11 hwframe DXGI format 0x%x; cannot create SRV.\n",
                   (unsigned)desc.Format);
            return -1;
        }

        vr->nv12_array_size   = (int)desc.ArraySize;
        vr->nv12_array_format = desc.Format;
        vr->nv12_srv_y  = av_calloc(desc.ArraySize, sizeof(*vr->nv12_srv_y));
        vr->nv12_srv_uv = av_calloc(desc.ArraySize, sizeof(*vr->nv12_srv_uv));
        if (!vr->nv12_srv_y || !vr->nv12_srv_uv) {
            release_nv12_srv_cache(vr);
            return -1;
        }
        vr->nv12_array = tex;
    }

    if (slice >= (UINT)vr->nv12_array_size)
        return -1;

    DXGI_FORMAT fmt_y, fmt_uv;
    if (dxgi_nv12_plane_formats(vr->nv12_array_format, &fmt_y, &fmt_uv) < 0)
        return -1;

    if (!vr->nv12_srv_y[slice]) {
        D3D11_SHADER_RESOURCE_VIEW_DESC svd = {0};
        svd.Format = fmt_y;
        svd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        svd.Texture2DArray.MostDetailedMip = 0;
        svd.Texture2DArray.MipLevels = 1;
        svd.Texture2DArray.FirstArraySlice = slice;
        svd.Texture2DArray.ArraySize = 1;
        hr = vr->device->lpVtbl->CreateShaderResourceView(vr->device,
                (ID3D11Resource *)tex, &svd, &vr->nv12_srv_y[slice]);
        if (FAILED(hr)) {
            av_log(NULL, AV_LOG_ERROR,
                   "CreateShaderResourceView(Y plane fmt=0x%x slice=%u) failed: 0x%lx\n",
                   (unsigned)fmt_y, slice, (unsigned long)hr);
            return -1;
        }
    }
    if (!vr->nv12_srv_uv[slice]) {
        D3D11_SHADER_RESOURCE_VIEW_DESC svd = {0};
        svd.Format = fmt_uv;
        svd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        svd.Texture2DArray.MostDetailedMip = 0;
        svd.Texture2DArray.MipLevels = 1;
        svd.Texture2DArray.FirstArraySlice = slice;
        svd.Texture2DArray.ArraySize = 1;
        hr = vr->device->lpVtbl->CreateShaderResourceView(vr->device,
                (ID3D11Resource *)tex, &svd, &vr->nv12_srv_uv[slice]);
        if (FAILED(hr)) {
            av_log(NULL, AV_LOG_ERROR,
                   "CreateShaderResourceView(UV plane fmt=0x%x slice=%u) failed: 0x%lx\n",
                   (unsigned)fmt_uv, slice, (unsigned long)hr);
            return -1;
        }
    }

    *out_y  = vr->nv12_srv_y[slice];
    *out_uv = vr->nv12_srv_uv[slice];
    return 0;
}

static int upload_bgra_frame(VideoRenderer *vr, AVFrame *frame)
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

/* ------------------------------------------------------------------ */
/* Hardware-acceleration plumbing                                      */
/* ------------------------------------------------------------------ */

/* Enable internal serialization on the immediate context so that the
 * libavcodec D3D11VA decoder thread and the main render thread can
 * share the same ID3D11DeviceContext without explicit user locking. */
static void enable_multithread_protection(ID3D11DeviceContext *ctx)
{
    ID3D11Multithread *mt = NULL;
    HRESULT hr = ctx->lpVtbl->QueryInterface(ctx, &IID_ID3D11Multithread, (void **)&mt);
    if (SUCCEEDED(hr) && mt) {
        mt->lpVtbl->SetMultithreadProtected(mt, TRUE);
        mt->lpVtbl->Release(mt);
    }
}

/* Wrap the renderer's ID3D11Device into an AVHWDeviceContext so that
 * the libavcodec D3D11VA decoder produces frames whose underlying
 * ID3D11Texture2D lives on the same device that we render with. The
 * BindFlags propagate to the frames pool so each decoded surface is
 * directly bindable as a shader resource (no GPU-internal copy). */
static int create_hw_device_ref(VideoRenderer *vr)
{
    AVBufferRef *ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (!ref)
        return -1;

    AVHWDeviceContext *dctx = (AVHWDeviceContext *)ref->data;
    AVD3D11VADeviceContext *d3dctx = (AVD3D11VADeviceContext *)dctx->hwctx;

    /* Hand our device to ffmpeg. ffmpeg will Release() it on free, so
     * AddRef here to keep it alive for the renderer's own usage. */
    vr->device->lpVtbl->AddRef(vr->device);
    d3dctx->device = vr->device;

    /* D3D11_BIND_DECODER is required for hwaccel; D3D11_BIND_SHADER_RESOURCE
     * makes the same surfaces directly sample-able in our pixel shader. */
    d3dctx->BindFlags = D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE;

    if (av_hwdevice_ctx_init(ref) < 0) {
        av_buffer_unref(&ref);
        return -1;
    }
    vr->hw_device_ref = ref;
    return 0;
}

int video_renderer_init(VideoRenderer *vr, HWND hwnd)
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

    /* VIDEO_SUPPORT enables D3D11VA hwaccel; BGRA_SUPPORT keeps the
     * existing BGRA swap-chain / SW-upload path working unchanged. */
    UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT |
                 D3D11_CREATE_DEVICE_BGRA_SUPPORT;
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

    enable_multithread_protection(vr->context);

    if (create_hw_device_ref(vr) < 0) {
        av_log(NULL, AV_LOG_WARNING,
               "D3D11VA hardware device context could not be initialized; "
               "video will fall back to the software upload path.\n");
        /* Non-fatal: SW path still works. */
    }

    create_rtv(vr);

    /* Compile shaders */
    ID3DBlob *vs_blob = NULL, *ps_blob = NULL, *ps_nv12_blob = NULL, *err_blob = NULL;
    hr = D3DCompile(s_shader_src, strlen(s_shader_src), NULL, NULL, NULL,
                    "VSMain", "vs_4_0", 0, 0, &vs_blob, &err_blob);
    if (FAILED(hr)) { SAFE_RELEASE(err_blob); return -1; }

    hr = D3DCompile(s_shader_src, strlen(s_shader_src), NULL, NULL, NULL,
                    "PSMain", "ps_4_0", 0, 0, &ps_blob, &err_blob);
    if (FAILED(hr)) { SAFE_RELEASE(vs_blob); SAFE_RELEASE(err_blob); return -1; }

    hr = D3DCompile(s_shader_nv12_src, strlen(s_shader_nv12_src), NULL, NULL, NULL,
                    "PSMainNV12", "ps_4_0", 0, 0, &ps_nv12_blob, &err_blob);
    if (FAILED(hr)) { SAFE_RELEASE(vs_blob); SAFE_RELEASE(ps_blob); SAFE_RELEASE(err_blob); return -1; }

    hr = vr->device->lpVtbl->CreateVertexShader(vr->device,
            vs_blob->lpVtbl->GetBufferPointer(vs_blob),
            vs_blob->lpVtbl->GetBufferSize(vs_blob), NULL, &vr->vertex_shader);
    if (FAILED(hr)) goto shader_fail;

    hr = vr->device->lpVtbl->CreatePixelShader(vr->device,
            ps_blob->lpVtbl->GetBufferPointer(ps_blob),
            ps_blob->lpVtbl->GetBufferSize(ps_blob), NULL, &vr->pixel_shader);
    if (FAILED(hr)) goto shader_fail;

    hr = vr->device->lpVtbl->CreatePixelShader(vr->device,
            ps_nv12_blob->lpVtbl->GetBufferPointer(ps_nv12_blob),
            ps_nv12_blob->lpVtbl->GetBufferSize(ps_nv12_blob), NULL, &vr->pixel_shader_nv12);
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
    SAFE_RELEASE(ps_nv12_blob);

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

    /* Color-management constant buffer (mapped per-frame only when
     * the source color metadata changes). Lives in slot b0 of the
     * NV12/P010 pixel shader. */
    {
        D3D11_BUFFER_DESC bd = {0};
        bd.ByteWidth = sizeof(ColorParamsCB);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = vr->device->lpVtbl->CreateBuffer(vr->device, &bd, NULL, &vr->color_cb);
        if (FAILED(hr)) return -1;
        vr->color_target       = RENDERER_COLOR_TARGET_SDR_SRGB;
        vr->color_state_dirty  = 1;
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
    SAFE_RELEASE(ps_nv12_blob);
    SAFE_RELEASE(err_blob);
    return -1;
}

void video_renderer_shutdown(VideoRenderer *vr)
{
    if (!vr)
        return;
    video_renderer_cleanup_textures(vr);
    SAFE_RELEASE(vr->color_cb);
    SAFE_RELEASE(vr->blend_state);
    SAFE_RELEASE(vr->sampler);
    SAFE_RELEASE(vr->vertex_buffer);
    SAFE_RELEASE(vr->input_layout);
    SAFE_RELEASE(vr->pixel_shader_nv12);
    SAFE_RELEASE(vr->pixel_shader);
    SAFE_RELEASE(vr->vertex_shader);
    SAFE_RELEASE(vr->rtv);
    SAFE_RELEASE(vr->swap_chain);
    /* Release the hwdevice ref before the device itself, so that
     * ffmpeg's internal Release() of the device pointer happens while
     * our retained reference still holds it alive (we hand-rolled an
     * AddRef in create_hw_device_ref). */
    av_buffer_unref(&vr->hw_device_ref);
    SAFE_RELEASE(vr->context);
    SAFE_RELEASE(vr->device);
    memset(vr, 0, sizeof(*vr));
}

AVBufferRef *video_renderer_get_hw_device_ctx(const VideoRenderer *vr)
{
    return vr ? vr->hw_device_ref : NULL;
}

/* ------------------------------------------------------------------ */
/* Public API — mirrors video_renderer.c for SDL2                     */
/* ------------------------------------------------------------------ */

void video_renderer_set_default_window_size(VideoRenderer *vr,
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

int video_renderer_get_supported_pixel_formats(const VideoRenderer *vr,
                                                     enum AVPixelFormat *out_fmts,
                                                     int max_fmts)
{
    if (!out_fmts || max_fmts <= 0)
        return 0;
    /* The list below is consumed by the buffersink in the SW filter
     * graph as the set of acceptable output pixel formats. The HW
     * decode path bypasses the filter entirely (see video_thread.c)
     * and is selected via avctx->get_format, so AV_PIX_FMT_D3D11 must
     * NOT appear here — the filter cannot produce hwframes for us. */
    int n = 0;
    if (n < max_fmts) out_fmts[n++] = AV_PIX_FMT_BGRA;
    (void)vr;
    return n;
}

int video_renderer_open(VideoRenderer *vr, int *width, int *height)
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

void video_renderer_draw_video(VideoRenderer *vr,
                                     AVFrame *frame, AVSubtitle *subtitle,
                                     int xleft, int ytop, int width, int height)
{
    if (!frame || !vr->context)
        return;

    const int is_hw_d3d11 = (frame->format == AV_PIX_FMT_D3D11);

    /* Resolve the per-frame shader resources. For SW frames we
     * Map+memcpy into a dynamic BGRA texture; for D3D11VA HW frames we
     * just look up (or lazily create) two SRVs onto the decoder's
     * NV12 texture-array slice — no pixel ever leaves the GPU. */
    ID3D11ShaderResourceView *srv_y = NULL, *srv_uv = NULL;

    if (is_hw_d3d11) {
        ID3D11Texture2D *tex = (ID3D11Texture2D *)frame->data[0];
        UINT slice = (UINT)(uintptr_t)frame->data[1];
        if (!tex || ensure_nv12_srvs(vr, tex, slice, &srv_y, &srv_uv) < 0)
            return;
        update_color_params(vr, frame);
    } else {
        if (vr->last_vid_data != frame->data[0]) {
            if (upload_bgra_frame(vr, frame) < 0)
                return;
        }
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
    vr->context->lpVtbl->PSSetSamplers(vr->context, 0, 1, &vr->sampler);

    if (is_hw_d3d11) {
        ID3D11ShaderResourceView *srvs[2] = { srv_y, srv_uv };
        vr->context->lpVtbl->PSSetShader(vr->context, vr->pixel_shader_nv12, NULL, 0);
        vr->context->lpVtbl->PSSetShaderResources(vr->context, 0, 2, srvs);
        vr->context->lpVtbl->PSSetConstantBuffers(vr->context, 0, 1, &vr->color_cb);
    } else {
        vr->context->lpVtbl->PSSetShader(vr->context, vr->pixel_shader, NULL, 0);
        vr->context->lpVtbl->PSSetShaderResources(vr->context, 0, 1, &vr->vid_srv);
    }

    /* No blending for video */
    float blend_factor[4] = { 0, 0, 0, 0 };
    vr->context->lpVtbl->OMSetBlendState(vr->context, NULL, blend_factor, 0xFFFFFFFF);
    vr->context->lpVtbl->Draw(vr->context, 4, 0);

    /* Subtitle overlay (always BGRA-on-CPU; subtitle decoders produce
     * palette/bitmap output so this path is independent of the video
     * decode mode). */
    if (subtitle && subtitle->num_rects > 0 && vr->blend_state) {
        if (ensure_texture(vr, &vr->sub_texture, &vr->sub_srv,
                           &vr->sub_tex_width, &vr->sub_tex_height,
                           frame->width, frame->height, 1) == 0) {
            D3D11_MAPPED_SUBRESOURCE mapped;
            HRESULT hr = vr->context->lpVtbl->Map(vr->context,
                (ID3D11Resource *)vr->sub_texture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
            if (SUCCEEDED(hr)) {
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

            /* Switch back to the BGRA pixel shader for the subtitle quad. */
            vr->context->lpVtbl->PSSetShader(vr->context, vr->pixel_shader, NULL, 0);
            vr->context->lpVtbl->OMSetBlendState(vr->context, vr->blend_state, blend_factor, 0xFFFFFFFF);
            vr->context->lpVtbl->PSSetShaderResources(vr->context, 0, 1, &vr->sub_srv);
            vr->context->lpVtbl->Draw(vr->context, 4, 0);
            vr->context->lpVtbl->OMSetBlendState(vr->context, NULL, blend_factor, 0xFFFFFFFF);
        }
    }

    /* Unbind SRVs to avoid debug-layer warnings on the next pass. */
    ID3D11ShaderResourceView *null_srvs[2] = { NULL, NULL };
    vr->context->lpVtbl->PSSetShaderResources(vr->context, 0, 2, null_srvs);
}

void video_renderer_clear(VideoRenderer *vr)
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

void video_renderer_present(VideoRenderer *vr)
{
    if (!vr || !vr->swap_chain)
        return;
    vr->swap_chain->lpVtbl->Present(vr->swap_chain, 1, 0); /* VSync */
}

void video_renderer_cleanup_textures(VideoRenderer *vr)
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
    /* The NV12 SRVs reference decoder-owned ID3D11Texture2D arrays
     * which may be released as soon as the player closes its stream.
     * Drop our cached views so we do not hold a dangling reference
     * across stream open/close cycles. */
    release_nv12_srv_cache(vr);
}

void video_renderer_resize(VideoRenderer *vr, int width, int height)
{
    if (!vr || !vr->swap_chain || width <= 0 || height <= 0)
        return;
    SAFE_RELEASE(vr->rtv);
    vr->swap_chain->lpVtbl->ResizeBuffers(vr->swap_chain, 0,
        (UINT)width, (UINT)height, DXGI_FORMAT_UNKNOWN, 0);
    create_rtv(vr);
}

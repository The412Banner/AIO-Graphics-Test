// AIO Graphics Test - HDR test card (Direct3D 11 / DXGI HDR10). See hdr_scene.h.
//
// What it checks, in the order a D3D11 game would:
//   - IDXGIOutput6::GetDesc1: the output's colour space, bit depth, luminance and
//     primaries. Under DXVK these come from the monitor's EDID; when no EDID is
//     readable DXVK substitutes 1499 / 799 / 0.01 nits and P3 primaries, which is
//     how the card tells "the screen description arrived" from "the stand-in".
//   - IDXGISwapChain3::CheckColorSpaceSupport for HDR10 (R10G10B10A2 + PQ BT.2020),
//     scRGB (R16G16B16A16F + linear BT.709) and sRGB, each asked with the buffers in
//     the matching format (the answer is per current buffer format).
//   - SetColorSpace1 + IDXGISwapChain4::SetHDRMetaData with the output's own
//     primaries and luminance.
//   - A Vulkan surface probe with no DXVK involved: which formats / colour spaces
//     the window system offers a plain Vulkan app.
// and it shows patterns that make real HDR, tone-mapped SDR, washed-out PQ and 8-bit
// banding obvious by eye. The card is mostly black with small bright areas, and the
// frame rate is vsync with an optional cap, to keep GPU and panel load low.
//
// Copyright (c) 2026 The412Banner. Licensed under Apache-2.0 (see LICENSE).

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <cfloat>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

// Vulkan is used only for the surface-format probe, through vkGetInstanceProcAddr
// from a LoadLibrary'd vulkan-1.dll. No prototypes, so this object adds no vk*
// symbol to the vulkan-1 import lib the CI derives from undefined references.
#define VK_NO_PROTOTYPES
#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <vulkan/vulkan.h>

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "hdr_scene.h"
#include "shell_imgui.h"  // aio_diag_log
#include "menu.h"         // AIO_VERSION

extern "C" {
#include "bench.h"  // aio_results_path
}

namespace {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
enum Mode { MODE_HDR10 = 0, MODE_SCRGB = 1, MODE_SDR = 2, MODE_COUNT = 3 };
const char *const kModeLabel[MODE_COUNT] = {"HDR10", "scRGB", "SDR"};

enum { SUP_UNKNOWN = -1, SUP_NO = 0, SUP_YES = 1 };

// Pattern kinds, matched by the card pixel shader.
enum { K_GREY = 0, K_COLOUR = 1, K_RAMP = 2, K_GRAD = 3, K_GRAD8 = 4, K_SUN = 5 };

// BT.2408 reference white. SDR 1.0 maps to it, and it is the UI layer's white in
// the HDR modes.
const float kSdrWhite = 203.0f;

// Luminance patches (nits). Slot 5 is replaced by the DXGI MaxLuminance.
const float kPatchNits[7] = {80.0f, 203.0f, 400.0f, 600.0f, 1000.0f, 0.0f, 10000.0f};

// Card colours. The card is a test pattern on black, so it does not follow the theme.
const ImU32 C_TEXT = IM_COL32(236, 242, 248, 255);
const ImU32 C_MUTED = IM_COL32(150, 164, 180, 255);
const ImU32 C_FAINT = IM_COL32(96, 110, 126, 255);
const ImU32 C_GOOD = IM_COL32(88, 214, 141, 255);
const ImU32 C_WARN = IM_COL32(245, 180, 70, 255);
const ImU32 C_BAD = IM_COL32(245, 104, 104, 255);
const ImU32 C_ACCENT = IM_COL32(47, 214, 195, 255);
const ImU32 C_PANEL = IM_COL32(9, 13, 18, 240);
const ImU32 C_LINE = IM_COL32(255, 255, 255, 34);

// ---------------------------------------------------------------------------
// Shaders (compiled at runtime through the shell's lazy D3DCompile shim).
// ---------------------------------------------------------------------------
// Card patterns. Every quad carries its pattern kind and parameters in two
// per-vertex float4s (constant across the quad) plus the output mode in p1.w; the
// pixel shader works out the pattern's light as linear BT.2020 nits, then encodes it
// for the swapchain: PQ for HDR10, linear BT.709 with 1.0 = 80 nits for scRGB, sRGB
// with 203 nits = 1.0 (clipped) for SDR. The banding strips are defined in the
// output's code values instead, so the 8-bit strip is quantised exactly as an 8-bit
// buffer in that encoding would be.
const char *kCardHLSL = R"HLSL(
struct VSIn { float2 pos : POSITION; float2 uv : TEXCOORD0; float4 p0 : TEXCOORD1; float4 p1 : TEXCOORD2; };
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; float4 p0 : TEXCOORD1; float4 p1 : TEXCOORD2; };
VSOut VSMain(VSIn i) {
  VSOut o;
  o.pos = float4(i.pos, 0.0, 1.0);
  o.uv = i.uv;
  o.p0 = i.p0;
  o.p1 = i.p1;
  return o;
}
float3 pq_from_nits(float3 n) {
  float3 y = pow(saturate(n / 10000.0), 0.1593017578125);
  return pow((0.8359375 + 18.8515625 * y) / (1.0 + 18.6875 * y), 78.84375);
}
float nits_from_pq(float c) {
  float e = pow(saturate(c), 1.0 / 78.84375);
  return 10000.0 * pow(max(e - 0.8359375, 0.0) / (18.8515625 - 18.6875 * e), 1.0 / 0.1593017578125);
}
float3 srgb_from_linear(float3 l) {
  float3 lo = l * 12.92;
  float3 hi = 1.055 * pow(l, 1.0 / 2.4) - 0.055;
  return lerp(hi, lo, step(l, 0.0031308));
}
float3 bt709_from_bt2020(float3 c) {
  return float3(dot(float3(1.6604910, -0.5876411, -0.0728499), c),
                dot(float3(-0.1245505, 1.1328999, -0.0083494), c),
                dot(float3(-0.0181508, -0.1005789, 1.1187297), c));
}
float3 encode(float3 n2020, float mode) {
  if (mode < 0.5) return pq_from_nits(max(n2020, 0.0));
  float3 n709 = bt709_from_bt2020(n2020);
  if (mode < 1.5) return n709 / 80.0;
  return srgb_from_linear(saturate(n709 / 203.0));
}
float4 PSMain(VSOut i) : SV_TARGET {
  float mode = i.p1.w;
  int kind = (int)(i.p0.x + 0.5);
  float3 c = float3(0.0, 0.0, 0.0);
  if (kind == 0) {
    c = encode(float3(i.p0.y, i.p0.y, i.p0.y), mode);
  } else if (kind == 1) {
    c = encode(i.p1.xyz, mode);
  } else if (kind == 2) {
    float n = nits_from_pq(i.uv.x);
    c = encode(float3(n, n, n), mode);
  } else if (kind == 3 || kind == 4) {
    float code = i.uv.x;
    if (mode < 1.5) code = i.uv.x * pq_from_nits(float3(i.p0.y, i.p0.y, i.p0.y)).x;
    if (kind == 4) code = floor(code * 255.0 + 0.5) / 255.0;
    if (mode > 0.5 && mode < 1.5) code = nits_from_pq(code) / 80.0;
    c = float3(code, code, code);
  } else if (kind == 5) {
    float2 d2 = (i.uv - i.p0.yz) * float2(i.p1.x, 1.0);
    float d = length(d2);
    float r = i.p0.w;
    float core = 1.0 - smoothstep(r * 0.94, r, d);
    float e = max(d - r, 0.0);
    float glow = (0.20 * exp(-e / (0.30 * r)) + 0.05 * exp(-e / (2.2 * r))) * (1.0 - core);
    float bg = 0.25 + 1.4 * i.uv.y * i.uv.y;
    float peak = i.p1.y;
    float3 warm = float3(1.0, 0.82, 0.56) / 0.8519;
    float3 n = float3(1.0, 1.0, 1.0) * (peak * core + bg) + warm * (peak * glow);
    c = encode(n, mode);
  }
  return float4(c, 1.0);
}
)HLSL";

// UI layer composite. ImGui is rendered into a cleared RGBA8 target, which leaves
// premultiplied colour; this un-premultiplies, linearises the sRGB UI colour, puts
// UI white at comp.y nits and re-encodes for the swapchain (comp.x: 0 = PQ BT.2020,
// 1 = scRGB), outputting premultiplied for a ONE / INV_SRC_ALPHA blend.
const char *kCompHLSL = R"HLSL(
Texture2D ui : register(t0);
cbuffer Comp : register(b0) { float4 comp; };
float4 VSFull(uint id : SV_VertexID) : SV_POSITION {
  float2 p = float2((id << 1) & 2, id & 2);
  return float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);
}
float3 pq_from_nits(float3 n) {
  float3 y = pow(saturate(n / 10000.0), 0.1593017578125);
  return pow((0.8359375 + 18.8515625 * y) / (1.0 + 18.6875 * y), 78.84375);
}
float3 linear_from_srgb(float3 s) {
  float3 lo = s / 12.92;
  float3 hi = pow((s + 0.055) / 1.055, 2.4);
  return lerp(hi, lo, step(s, 0.04045));
}
float3 bt2020_from_bt709(float3 c) {
  return float3(dot(float3(0.6274040, 0.3292820, 0.0433136), c),
                dot(float3(0.0690970, 0.9195400, 0.0113612), c),
                dot(float3(0.0163916, 0.0880132, 0.8955950), c));
}
float4 PSComp(float4 pos : SV_POSITION) : SV_TARGET {
  float4 u = ui.Load(int3((int)pos.x, (int)pos.y, 0));
  if (u.a <= 0.0) return float4(0.0, 0.0, 0.0, 0.0);
  float3 n709 = linear_from_srgb(saturate(u.rgb / u.a)) * comp.y;
  float3 o = n709 / 80.0;
  if (comp.x < 0.5) o = pq_from_nits(max(bt2020_from_bt709(n709), 0.0));
  return float4(o * u.a, u.a);
}
)HLSL";

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
struct CardVtx {
    float x, y, u, v;
    float p0[4];
    float p1[4];
};

// One card element, recorded by the UI pass in window pixels and turned into
// vertices by the render pass.
struct CardQuad {
    float x0, y0, x1, y1;
    float p0[4];  // kind, a, b, c
    float p1[4];  // d, e, f, (mode, filled at render time)
};
const int kMaxQuads = 48;
CardQuad g_quad[kMaxQuads];
int g_nquad = 0;

struct State {
    bool active;
    bool flip;              // our flip-model swapchain is in the host's slot
    bool legacy;            // no flip-model swapchain: SDR on the shell's own swapchain
    char legacy_why[160];
    HRESULT hr_create;
    UINT create_w, create_h;

    IDXGISwapChain3 *sc3;
    IDXGISwapChain4 *sc4;
    IDXGIOutput6 *out6;
    bool have_desc;
    DXGI_OUTPUT_DESC1 desc_start;  // read before our first SetColorSpace1
    DXGI_OUTPUT_DESC1 desc;        // latest read
    char adapter[128];
    char dxvk_hdr[32];             // DXVK_HDR value, or "(not set)"
    bool dxvk_hdr_set;

    int sup_hdr10, sup_scrgb, sup_srgb;
    UINT flags_hdr10, flags_scrgb, flags_srgb;
    HRESULT hr_fp16, hr_10;

    int mode;
    int pending_mode;              // -1 = none
    bool sdr_chosen;               // SDR picked by the tester, not by fallback
    bool mode_failed[MODE_COUNT];  // SetColorSpace1 refused it at runtime
    bool recheck;
    HRESULT hr_resize, hr_cs, hr_meta;
    DXGI_HDR_METADATA_HDR10 meta;

    int cap;                       // frame cap on top of vsync: 0 = off, 60, 30
    bool show_values;
    float scroll;
    LONGLONG t0;                   // QPC at enter (animation clock)

    bool pipe_ok;
    char pipe_err[96];
    ID3D11VertexShader *vs, *vs_full;
    ID3D11PixelShader *ps, *ps_comp;
    ID3D11InputLayout *il;
    ID3D11Buffer *vb, *cb_comp;
    ID3D11RasterizerState *rs;
    ID3D11BlendState *bs_premul;

    ID3D11Texture2D *ui_tex;
    ID3D11RenderTargetView *ui_rtv;
    ID3D11ShaderResourceView *ui_srv;
    UINT ui_w, ui_h;

    char report_path[MAX_PATH];
    bool report_ok, mirror_ok;
    char ev[40][224];
    int nev;
};
State S;

// Vulkan surface probe (background thread; results read once g_vk_state == 2).
struct VkProbe {
    bool loader;          // vulkan-1.dll + vkGetInstanceProcAddr
    bool instance;        // vkCreateInstance succeeded
    bool ext_colorspace;  // instance offers VK_EXT_swapchain_colorspace
    bool ext_win32;       // instance offers VK_KHR_win32_surface
    bool dev_hdr_meta;    // device offers VK_EXT_hdr_metadata
    bool surface;         // surface created and formats read
    bool hdr10;           // a format in VK_COLOR_SPACE_HDR10_ST2084_EXT
    bool scrgb;           // a format in VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT
    int nformats;
    char device[128];
    char hdr10_fmt[40];
    char formats[640];    // "SRGB_NONLINEAR: B8G8R8A8_UNORM R8G8B8A8_UNORM | HDR10_ST2084: ..."
    char error[128];
};
volatile LONG g_vk_state = 0;  // 0 idle, 1 running, 2 done
VkProbe g_vk;
bool g_vk_logged = false;      // main thread: result already in the event log / report

AioHdrFonts F;  // the shell's fonts, copied each frame

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
// Event log: kept for the report (last 40) and mirrored to the startup diag log.
void ev(const char *fmt, ...) {
    char msg[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    const int cap = (int)(sizeof(S.ev) / sizeof(S.ev[0]));
    if (S.nev == cap) {
        memmove(S.ev[0], S.ev[1], sizeof(S.ev) - sizeof(S.ev[0]));
        S.nev = cap - 1;
    }
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(S.ev[S.nev++], sizeof(S.ev[0]), "%02d:%02d:%02d.%03d  %s", st.wHour, st.wMinute,
             st.wSecond, st.wMilliseconds, msg);
    char line[224];
    snprintf(line, sizeof(line), "hdr: %s", msg);
    aio_diag_log(line);
}

const char *hr_str(HRESULT hr, char *buf, size_t cap) {
    const char *name = nullptr;
    switch ((unsigned long)hr) {
        case 0x80004001UL: name = "E_NOTIMPL"; break;
        case 0x80004002UL: name = "E_NOINTERFACE"; break;
        case 0x80004005UL: name = "E_FAIL"; break;
        case 0x80070005UL: name = "E_ACCESSDENIED"; break;
        case 0x80070057UL: name = "E_INVALIDARG"; break;
        case 0x887A0001UL: name = "DXGI_ERROR_INVALID_CALL"; break;
        case 0x887A0002UL: name = "DXGI_ERROR_NOT_FOUND"; break;
        case 0x887A0004UL: name = "DXGI_ERROR_UNSUPPORTED"; break;
        case 0x887A0005UL: name = "DXGI_ERROR_DEVICE_REMOVED"; break;
        default: break;
    }
    if (hr == S_OK) snprintf(buf, cap, "S_OK");
    else if (name) snprintf(buf, cap, "0x%08lX %s", (unsigned long)hr, name);
    else snprintf(buf, cap, "0x%08lX", (unsigned long)hr);
    return buf;
}

const char *cs_str(DXGI_COLOR_SPACE_TYPE cs) {
    switch (cs) {
        case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709: return "sRGB (G22 P709)";
        case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709: return "scRGB (G10 P709)";
        case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020: return "HDR10 (G2084 P2020)";
        default: return "other";
    }
}

const char *fmt_str(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2_UNORM";
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT";
        case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
        case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
        default: return "other";
    }
}

const char *sup_str(int s) { return s == SUP_YES ? "yes" : (s == SUP_NO ? "no" : "unknown"); }

DXGI_FORMAT mode_format(int m) {
    return m == MODE_HDR10 ? DXGI_FORMAT_R10G10B10A2_UNORM
                           : (m == MODE_SCRGB ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM);
}

DXGI_COLOR_SPACE_TYPE mode_cs(int m) {
    return m == MODE_HDR10 ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
                           : (m == MODE_SCRGB ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709
                                              : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
}

bool mode_available(int m) {
    if (m == MODE_SDR) return true;
    if (!S.flip || !S.pipe_ok || !S.sc3 || S.mode_failed[m]) return false;
    return (m == MODE_HDR10 ? S.sup_hdr10 : S.sup_scrgb) == SUP_YES;
}

int best_mode() { return mode_available(MODE_HDR10) ? MODE_HDR10 : MODE_SDR; }

float pq_code(float nits) {
    double y = nits / 10000.0;
    if (y < 0.0) y = 0.0;
    if (y > 1.0) y = 1.0;
    double ym = pow(y, 0.1593017578125);
    return (float)pow((0.8359375 + 18.8515625 * ym) / (1.0 + 18.6875 * ym), 78.84375);
}

// The output's peak, or 1000 nits when DXGI gave none (then labelled "assumed").
bool have_max() { return S.have_desc && S.desc.MaxLuminance > 1.0f; }
float max_nits() { return have_max() ? S.desc.MaxLuminance : 1000.0f; }

double scene_time() {
    LARGE_INTEGER f, n;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&n);
    return f.QuadPart ? (double)(n.QuadPart - S.t0) / (double)f.QuadPart : 0.0;
}

// ---------------------------------------------------------------------------
// Screen description verdict (the layer's EDID vs DXVK's stand-in)
// ---------------------------------------------------------------------------
enum { V_NONE = 0, V_SCREEN, V_STANDIN_HDR, V_STANDIN_SDR, V_EMPTY };

bool near_f(float a, float b, float tol) { return fabsf(a - b) <= tol; }

int verdict() {
    if (!S.have_desc) return V_NONE;
    const DXGI_OUTPUT_DESC1 &d = S.desc;
    // A DXGI that fills no luminance at all (DXVK never does: it substitutes).
    if (d.MaxLuminance <= 1.0f && d.MaxFullFrameLuminance <= 1.0f) return V_EMPTY;
    // DXVK's NormalizeDisplayMetadata values for a missing EDID (wsi_edid.h).
    if (near_f(d.MaxLuminance, 1499.0f, 0.5f) && near_f(d.MaxFullFrameLuminance, 799.0f, 0.5f) &&
        near_f(d.MinLuminance, 0.01f, 0.0005f))
        return V_STANDIN_HDR;
    if (near_f(d.MaxLuminance, 270.0f, 0.5f) && near_f(d.MaxFullFrameLuminance, 270.0f, 0.5f) &&
        near_f(d.MinLuminance, 0.5f, 0.0005f))
        return V_STANDIN_SDR;
    return V_SCREEN;
}

// DXVK's HDR stand-in primaries (P3, D65) when no chromaticity data arrived.
bool p3_standin_primaries() {
    if (!S.have_desc) return false;
    const DXGI_OUTPUT_DESC1 &d = S.desc;
    const float t = 0.0005f;
    return near_f(d.RedPrimary[0], 0.680f, t) && near_f(d.RedPrimary[1], 0.320f, t) &&
           near_f(d.GreenPrimary[0], 0.265f, t) && near_f(d.GreenPrimary[1], 0.690f, t) &&
           near_f(d.BluePrimary[0], 0.150f, t) && near_f(d.BluePrimary[1], 0.060f, t);
}

const char *verdict_text(char *buf, size_t cap, ImU32 *col) {
    switch (verdict()) {
        case V_SCREEN:
            snprintf(buf, cap, "DXGI reports your screen (~%.0f nits): the layer's screen description arrived",
                     S.desc.MaxLuminance);
            if (col) *col = C_GOOD;
            break;
        case V_STANDIN_HDR:
            snprintf(buf, cap,
                     "DXGI reports DXVK's stand-in (1499/799/0.01): the layer's screen description did not arrive");
            if (col) *col = C_WARN;
            break;
        case V_STANDIN_SDR:
            snprintf(buf, cap,
                     "DXGI reports DXVK's SDR stand-in (270/270/0.5): DXVK HDR is off and no screen description arrived");
            if (col) *col = C_WARN;
            break;
        case V_EMPTY:
            snprintf(buf, cap, "DXGI reports no luminance at all: this DXGI does not describe the screen (not DXVK's?)");
            if (col) *col = C_WARN;
            break;
        default:
            snprintf(buf, cap, "IDXGIOutput6::GetDesc1 is unavailable: the screen description cannot be checked");
            if (col) *col = C_BAD;
            break;
    }
    return buf;
}

// Why HDR10 is not selectable.
const char *hdr10_reason() {
    if (S.legacy) return S.legacy_why;
    if (!S.sc3) return "IDXGISwapChain3 (DXGI 1.4) is not available";
    if (!S.pipe_ok) return S.pipe_err[0] ? S.pipe_err : "the card's shaders could not be built";
    if (S.mode_failed[MODE_HDR10]) return "SetColorSpace1(HDR10) was refused";
    bool pq_out = S.have_desc && S.desc_start.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
    if (S.sup_hdr10 == SUP_NO && !pq_out) {
        if (S.dxvk_hdr_set && strcmp(S.dxvk_hdr, "1") == 0)
            return "DXVK_HDR=1 is set but DXGI reports an SDR output (not DXVK's DXGI, or dxvk.conf turns HDR off)";
        return "DXVK HDR is off: DXVK_HDR is not set for this process (Bannerlator sets it at launch when HDR output is on)";
    }
    if (S.sup_hdr10 == SUP_NO) return "DXVK HDR is on, but the window's surface does not offer HDR10 (PQ): compositor or driver side";
    return "CheckColorSpaceSupport gave no answer";
}

// ---------------------------------------------------------------------------
// Vulkan surface probe
// ---------------------------------------------------------------------------
const char *vk_format_name(VkFormat f) {
    switch (f) {
        case VK_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
        case VK_FORMAT_B8G8R8A8_SRGB: return "B8G8R8A8_SRGB";
        case VK_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
        case VK_FORMAT_R8G8B8A8_SRGB: return "R8G8B8A8_SRGB";
        case VK_FORMAT_A8B8G8R8_UNORM_PACK32: return "A8B8G8R8_UNORM";
        case VK_FORMAT_A8B8G8R8_SRGB_PACK32: return "A8B8G8R8_SRGB";
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return "A2B10G10R10_UNORM";
        case VK_FORMAT_A2R10G10B10_UNORM_PACK32: return "A2R10G10B10_UNORM";
        case VK_FORMAT_R16G16B16A16_SFLOAT: return "R16G16B16A16_SFLOAT";
        case VK_FORMAT_R5G6B5_UNORM_PACK16: return "R5G6B5_UNORM";
        case VK_FORMAT_B5G6R5_UNORM_PACK16: return "B5G6R5_UNORM";
        case VK_FORMAT_A1R5G5B5_UNORM_PACK16: return "A1R5G5B5_UNORM";
        default: return nullptr;
    }
}

const char *vk_cs_name(VkColorSpaceKHR cs) {
    switch ((int)cs) {
        case VK_COLOR_SPACE_SRGB_NONLINEAR_KHR: return "SRGB_NONLINEAR";
        case VK_COLOR_SPACE_HDR10_ST2084_EXT: return "HDR10_ST2084";
        case VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT: return "EXTENDED_SRGB_LINEAR";
        case VK_COLOR_SPACE_EXTENDED_SRGB_NONLINEAR_EXT: return "EXTENDED_SRGB_NONLINEAR";
        case VK_COLOR_SPACE_DISPLAY_P3_NONLINEAR_EXT: return "DISPLAY_P3_NONLINEAR";
        case VK_COLOR_SPACE_DISPLAY_P3_LINEAR_EXT: return "DISPLAY_P3_LINEAR";
        case VK_COLOR_SPACE_DCI_P3_NONLINEAR_EXT: return "DCI_P3_NONLINEAR";
        case VK_COLOR_SPACE_BT709_LINEAR_EXT: return "BT709_LINEAR";
        case VK_COLOR_SPACE_BT709_NONLINEAR_EXT: return "BT709_NONLINEAR";
        case VK_COLOR_SPACE_BT2020_LINEAR_EXT: return "BT2020_LINEAR";
        case VK_COLOR_SPACE_HDR10_HLG_EXT: return "HDR10_HLG";
        case VK_COLOR_SPACE_DOLBYVISION_EXT: return "DOLBYVISION";
        case VK_COLOR_SPACE_ADOBERGB_LINEAR_EXT: return "ADOBERGB_LINEAR";
        case VK_COLOR_SPACE_ADOBERGB_NONLINEAR_EXT: return "ADOBERGB_NONLINEAR";
        case VK_COLOR_SPACE_PASS_THROUGH_EXT: return "PASS_THROUGH";
        default: return nullptr;
    }
}

// Append printf-style text to a fixed buffer, never overflowing it.
void appendf(char *buf, size_t cap, const char *fmt, ...) {
    size_t n = strlen(buf);
    if (n + 1 >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf + n, cap - n, fmt, ap);
    va_end(ap);
}

// The probe asks a plain Vulkan instance what a Win32 surface offers. The surface is
// made on a hidden window of its own, so the shell window (and its DXVK swapchain)
// is never touched.
void vk_probe_run(VkProbe &r) {
    HMODULE lib = LoadLibraryA("vulkan-1.dll");
    PFN_vkGetInstanceProcAddr gipa =
        lib ? (PFN_vkGetInstanceProcAddr)GetProcAddress(lib, "vkGetInstanceProcAddr") : nullptr;
    if (!gipa) {
        snprintf(r.error, sizeof(r.error), "vulkan-1.dll or vkGetInstanceProcAddr not found");
        return;
    }
    r.loader = true;
    PFN_vkEnumerateInstanceExtensionProperties enum_iext =
        (PFN_vkEnumerateInstanceExtensionProperties)gipa(nullptr, "vkEnumerateInstanceExtensionProperties");
    PFN_vkCreateInstance create_inst = (PFN_vkCreateInstance)gipa(nullptr, "vkCreateInstance");
    if (!enum_iext || !create_inst) {
        snprintf(r.error, sizeof(r.error), "loader entry points missing");
        return;
    }

    static VkExtensionProperties iext[160];
    uint32_t n = 160;
    bool has_surface = false;
    if (enum_iext(nullptr, &n, iext) >= 0) {
        for (uint32_t i = 0; i < n; ++i) {
            if (!strcmp(iext[i].extensionName, VK_KHR_SURFACE_EXTENSION_NAME)) has_surface = true;
            if (!strcmp(iext[i].extensionName, VK_KHR_WIN32_SURFACE_EXTENSION_NAME)) r.ext_win32 = true;
            if (!strcmp(iext[i].extensionName, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME)) r.ext_colorspace = true;
        }
    }
    const char *names[3];
    uint32_t nn = 0;
    if (has_surface) names[nn++] = VK_KHR_SURFACE_EXTENSION_NAME;
    if (r.ext_win32) names[nn++] = VK_KHR_WIN32_SURFACE_EXTENSION_NAME;
    if (r.ext_colorspace) names[nn++] = VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME;

    VkApplicationInfo app;
    memset(&app, 0, sizeof(app));
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "AIO Graphics Test HDR probe";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ci;
    memset(&ci, 0, sizeof(ci));
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = nn;
    ci.ppEnabledExtensionNames = names;
    VkInstance inst = nullptr;
    VkResult vr = create_inst(&ci, nullptr, &inst);
    if (vr != VK_SUCCESS || !inst) {
        snprintf(r.error, sizeof(r.error), "vkCreateInstance failed (%d)", (int)vr);
        return;
    }
    r.instance = true;

    PFN_vkDestroyInstance destroy_inst = (PFN_vkDestroyInstance)gipa(inst, "vkDestroyInstance");
    PFN_vkEnumeratePhysicalDevices enum_pd = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties get_props =
        (PFN_vkGetPhysicalDeviceProperties)gipa(inst, "vkGetPhysicalDeviceProperties");
    PFN_vkEnumerateDeviceExtensionProperties enum_dext =
        (PFN_vkEnumerateDeviceExtensionProperties)gipa(inst, "vkEnumerateDeviceExtensionProperties");
    PFN_vkCreateWin32SurfaceKHR create_surf = (PFN_vkCreateWin32SurfaceKHR)gipa(inst, "vkCreateWin32SurfaceKHR");
    PFN_vkDestroySurfaceKHR destroy_surf = (PFN_vkDestroySurfaceKHR)gipa(inst, "vkDestroySurfaceKHR");
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR get_fmts =
        (PFN_vkGetPhysicalDeviceSurfaceFormatsKHR)gipa(inst, "vkGetPhysicalDeviceSurfaceFormatsKHR");

    VkPhysicalDevice pd = nullptr;
    uint32_t npd = 1;
    if (!enum_pd || enum_pd(inst, &npd, &pd) < 0 || npd == 0) pd = nullptr;
    if (pd && get_props) {
        VkPhysicalDeviceProperties p;
        get_props(pd, &p);
        snprintf(r.device, sizeof(r.device), "%s", p.deviceName);
    }
    if (pd && enum_dext) {
        static VkExtensionProperties dext[512];
        uint32_t nd = 512;
        if (enum_dext(pd, nullptr, &nd, dext) >= 0) {
            for (uint32_t i = 0; i < nd; ++i)
                if (!strcmp(dext[i].extensionName, VK_EXT_HDR_METADATA_EXTENSION_NAME)) r.dev_hdr_meta = true;
        }
    }

    if (!pd) {
        snprintf(r.error, sizeof(r.error), "no Vulkan physical device");
    } else if (!has_surface || !r.ext_win32 || !create_surf || !destroy_surf || !get_fmts) {
        snprintf(r.error, sizeof(r.error), "VK_KHR_surface / VK_KHR_win32_surface not available");
    } else {
        HINSTANCE hi = GetModuleHandleA(nullptr);
        WNDCLASSA wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = DefWindowProcA;
        wc.hInstance = hi;
        wc.lpszClassName = "AIOHdrVkProbe";
        RegisterClassA(&wc);  // fails harmlessly when an earlier probe registered it
        HWND hw = CreateWindowExA(WS_EX_TOOLWINDOW, "AIOHdrVkProbe", "", WS_POPUP, 0, 0, 64, 64, nullptr,
                                  nullptr, hi, nullptr);
        if (!hw) {
            snprintf(r.error, sizeof(r.error), "could not create the probe window");
        } else {
            VkWin32SurfaceCreateInfoKHR sci;
            memset(&sci, 0, sizeof(sci));
            sci.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
            sci.hinstance = hi;
            sci.hwnd = hw;
            VkSurfaceKHR surf = VK_NULL_HANDLE;
            vr = create_surf(inst, &sci, nullptr, &surf);
            if (vr != VK_SUCCESS) {
                snprintf(r.error, sizeof(r.error), "vkCreateWin32SurfaceKHR failed (%d)", (int)vr);
            } else {
                VkSurfaceFormatKHR fm[64];
                uint32_t nf = 64;
                vr = get_fmts(pd, surf, &nf, fm);
                if (vr < 0) {
                    snprintf(r.error, sizeof(r.error), "vkGetPhysicalDeviceSurfaceFormatsKHR failed (%d)", (int)vr);
                } else {
                    r.surface = true;
                    r.nformats = (int)nf;
                    // Group the formats by colour space.
                    bool used[64];
                    memset(used, 0, sizeof(used));
                    for (uint32_t i = 0; i < nf; ++i) {
                        if (used[i]) continue;
                        VkColorSpaceKHR cs = fm[i].colorSpace;
                        const char *csn = vk_cs_name(cs);
                        if (r.formats[0]) appendf(r.formats, sizeof(r.formats), " | ");
                        if (csn) appendf(r.formats, sizeof(r.formats), "%s:", csn);
                        else appendf(r.formats, sizeof(r.formats), "colour space %d:", (int)cs);
                        for (uint32_t j = i; j < nf; ++j) {
                            if (used[j] || fm[j].colorSpace != cs) continue;
                            used[j] = true;
                            const char *fn = vk_format_name(fm[j].format);
                            if (fn) appendf(r.formats, sizeof(r.formats), " %s", fn);
                            else appendf(r.formats, sizeof(r.formats), " fmt%d", (int)fm[j].format);
                            if (cs == VK_COLOR_SPACE_HDR10_ST2084_EXT && !r.hdr10_fmt[0])
                                snprintf(r.hdr10_fmt, sizeof(r.hdr10_fmt), "%s", fn ? fn : "other format");
                        }
                        if (cs == VK_COLOR_SPACE_HDR10_ST2084_EXT) r.hdr10 = true;
                        if (cs == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT) r.scrgb = true;
                    }
                }
                destroy_surf(inst, surf, nullptr);
            }
            DestroyWindow(hw);
        }
    }
    if (destroy_inst) destroy_inst(inst, nullptr);
}

DWORD WINAPI vk_probe_thread(LPVOID) {
    static VkProbe r;  // static: keeps the result off the thread stack
    memset(&r, 0, sizeof(r));
    vk_probe_run(r);
    g_vk = r;
    InterlockedExchange(&g_vk_state, 2);  // publish after the struct is filled
    return 0;
}

void vk_probe_kick() {
    // Start from idle, or re-run after a finished probe; never while one runs.
    if (InterlockedCompareExchange(&g_vk_state, 1, 0) != 0 &&
        InterlockedCompareExchange(&g_vk_state, 1, 2) != 2)
        return;
    g_vk_logged = false;
    HANDLE t = CreateThread(nullptr, 0, vk_probe_thread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
    else InterlockedExchange(&g_vk_state, 0);
}

// ---------------------------------------------------------------------------
// Swapchain plumbing
// ---------------------------------------------------------------------------
void bb_release(const AioHdrHost *h) {
    h->ctx->OMSetRenderTargets(0, nullptr, nullptr);  // a bound RTV keeps the buffer referenced
    if (*h->rtv) {
        (*h->rtv)->Release();
        *h->rtv = nullptr;
    }
}

void bb_create(const AioHdrHost *h) {
    if (!*h->swap || *h->rtv) return;
    ID3D11Texture2D *back = nullptr;
    if (SUCCEEDED((*h->swap)->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&back)) && back) {
        h->dev->CreateRenderTargetView(back, nullptr, h->rtv);
        back->Release();
    }
}

void bb_size(IDXGISwapChain *sw, UINT *w, UINT *hh) {
    *w = *hh = 0;
    DXGI_SWAP_CHAIN_DESC d;
    if (sw && SUCCEEDED(sw->GetDesc(&d))) {
        *w = d.BufferDesc.Width;
        *hh = d.BufferDesc.Height;
    }
}

// The device's adapter and its DXGI factories. Caller releases what it asked for.
void dxgi_objects(ID3D11Device *dev, IDXGIAdapter **ad, IDXGIFactory **f1, IDXGIFactory2 **f2) {
    *ad = nullptr;
    if (f1) *f1 = nullptr;
    if (f2) *f2 = nullptr;
    IDXGIDevice *dd = nullptr;
    if (FAILED(dev->QueryInterface(__uuidof(IDXGIDevice), (void **)&dd)) || !dd) return;
    dd->GetAdapter(ad);
    dd->Release();
    if (!*ad) return;
    if (f1) (*ad)->GetParent(__uuidof(IDXGIFactory), (void **)f1);
    if (f2) (*ad)->GetParent(__uuidof(IDXGIFactory2), (void **)f2);
}

// Recreate the shell's swapchain exactly as its create_device made it: bitblt
// DISCARD, 2 x R8G8B8A8, window-sized. Only called once the previous one is gone.
bool create_shell_swapchain(const AioHdrHost *h) {
    IDXGIAdapter *ad = nullptr;
    IDXGIFactory *f1 = nullptr;
    dxgi_objects(h->dev, &ad, &f1, nullptr);
    HRESULT hr = E_NOINTERFACE;
    if (f1) {
        DXGI_SWAP_CHAIN_DESC scd;
        ZeroMemory(&scd, sizeof(scd));
        scd.BufferCount = 2;
        scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scd.OutputWindow = h->hwnd;
        scd.SampleDesc.Count = 1;
        scd.Windowed = TRUE;
        scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        hr = f1->CreateSwapChain(h->dev, &scd, h->swap);
        f1->Release();
    }
    if (ad) ad->Release();
    char b[48];
    ev("shell swapchain (bitblt R8G8B8A8) recreated: %s", hr_str(hr, b, sizeof(b)));
    return SUCCEEDED(hr) && *h->swap;
}

void read_desc(bool at_start) {
    if (!S.out6) {
        S.have_desc = false;
        return;
    }
    DXGI_OUTPUT_DESC1 d;
    ZeroMemory(&d, sizeof(d));
    if (FAILED(S.out6->GetDesc1(&d))) {
        S.have_desc = false;
        return;
    }
    if (at_start) S.desc_start = d;
    S.desc = d;
    S.have_desc = true;
}

UINT16 xy_units(float v) {  // chromaticity in 0.00002 units
    float x = v * 50000.0f + 0.5f;
    if (x < 0.0f) x = 0.0f;
    if (x > 65535.0f) x = 65535.0f;
    return (UINT16)x;
}

UINT16 nits_u16(float v) {
    float x = v + 0.5f;
    if (x < 0.0f) x = 0.0f;
    if (x > 65535.0f) x = 65535.0f;
    return (UINT16)x;
}

// HDR10 metadata from the output's own description (what a game that trusts DXGI
// sends). Content light levels are set to the output's peak / full-frame values.
void build_meta() {
    DXGI_HDR_METADATA_HDR10 &m = S.meta;
    ZeroMemory(&m, sizeof(m));
    if (S.have_desc) {
        const DXGI_OUTPUT_DESC1 &d = S.desc;
        m.RedPrimary[0] = xy_units(d.RedPrimary[0]);
        m.RedPrimary[1] = xy_units(d.RedPrimary[1]);
        m.GreenPrimary[0] = xy_units(d.GreenPrimary[0]);
        m.GreenPrimary[1] = xy_units(d.GreenPrimary[1]);
        m.BluePrimary[0] = xy_units(d.BluePrimary[0]);
        m.BluePrimary[1] = xy_units(d.BluePrimary[1]);
        m.WhitePoint[0] = xy_units(d.WhitePoint[0]);
        m.WhitePoint[1] = xy_units(d.WhitePoint[1]);
        m.MaxMasteringLuminance = (UINT)(d.MaxLuminance + 0.5f);
        m.MinMasteringLuminance = (UINT)(d.MinLuminance * 10000.0f + 0.5f);  // 0.0001-nit units
        m.MaxContentLightLevel = nits_u16(d.MaxLuminance);
        m.MaxFrameAverageLightLevel = nits_u16(d.MaxFullFrameLuminance);
    } else {
        // No output description: BT.2020 primaries and generic levels.
        m.RedPrimary[0] = xy_units(0.708f);
        m.RedPrimary[1] = xy_units(0.292f);
        m.GreenPrimary[0] = xy_units(0.170f);
        m.GreenPrimary[1] = xy_units(0.797f);
        m.BluePrimary[0] = xy_units(0.131f);
        m.BluePrimary[1] = xy_units(0.046f);
        m.WhitePoint[0] = xy_units(0.3127f);
        m.WhitePoint[1] = xy_units(0.3290f);
        m.MaxMasteringLuminance = 1000;
        m.MinMasteringLuminance = 100;
        m.MaxContentLightLevel = 1000;
        m.MaxFrameAverageLightLevel = 400;
    }
}

int support_of(DXGI_COLOR_SPACE_TYPE cs, UINT *flags) {
    *flags = 0;
    if (!S.sc3) return SUP_UNKNOWN;
    UINT f = 0;
    if (FAILED(S.sc3->CheckColorSpaceSupport(cs, &f))) return SUP_UNKNOWN;
    *flags = f;
    return (f & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) ? SUP_YES : SUP_NO;
}

// CheckColorSpaceSupport answers for the buffers' CURRENT format, so each pairing is
// asked with the buffers in that format: FP16 for scRGB, R10G10B10A2 for HDR10 and
// sRGB. apply_mode() puts the mode's own format back right after.
void probe_support(const AioHdrHost *h) {
    S.sup_hdr10 = S.sup_scrgb = S.sup_srgb = SUP_UNKNOWN;
    S.flags_hdr10 = S.flags_scrgb = S.flags_srgb = 0;
    if (!S.flip || !S.sc3 || !*h->swap) return;
    IDXGISwapChain *sw = *h->swap;
    UINT w = 0, hh = 0;
    bb_size(sw, &w, &hh);
    bb_release(h);
    S.hr_fp16 = sw->ResizeBuffers(0, w, hh, DXGI_FORMAT_R16G16B16A16_FLOAT, 0);
    if (SUCCEEDED(S.hr_fp16)) S.sup_scrgb = support_of(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709, &S.flags_scrgb);
    S.hr_10 = sw->ResizeBuffers(0, w, hh, DXGI_FORMAT_R10G10B10A2_UNORM, 0);
    if (SUCCEEDED(S.hr_10)) {
        S.sup_hdr10 = support_of(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020, &S.flags_hdr10);
        S.sup_srgb = support_of(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709, &S.flags_srgb);
    }
    bb_create(h);
    char a[48], b[48];
    ev("CheckColorSpaceSupport: HDR10 %s (0x%X), scRGB %s (0x%X), sRGB %s (0x%X); resize FP16 %s, 10-bit %s",
       sup_str(S.sup_hdr10), S.flags_hdr10, sup_str(S.sup_scrgb), S.flags_scrgb, sup_str(S.sup_srgb),
       S.flags_srgb, hr_str(S.hr_fp16, a, sizeof(a)), hr_str(S.hr_10, b, sizeof(b)));
}

// Switch the swapchain to mode m: buffer format, then colour space, then metadata.
// A mode the swapchain refuses falls back to SDR, so the UI layer is never
// composited for an encoding the buffers are not presented in.
void apply_mode(const AioHdrHost *h, int m) {
    if (!mode_available(m)) m = MODE_SDR;
    IDXGISwapChain *sw = *h->swap;
    if (!sw) return;
    if (!S.flip) {  // legacy: the shell's own 8-bit swapchain, left as it is
        S.mode = MODE_SDR;
        return;
    }
    UINT w = 0, hh = 0;
    bb_size(sw, &w, &hh);
    bb_release(h);
    S.hr_resize = sw->ResizeBuffers(0, w, hh, mode_format(m), 0);
    bb_create(h);
    S.hr_cs = S.sc3 ? S.sc3->SetColorSpace1(mode_cs(m)) : E_NOINTERFACE;
    if (S.sc4) {
        if (m == MODE_SDR) S.hr_meta = S.sc4->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_NONE, 0, nullptr);
        else S.hr_meta = S.sc4->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_HDR10, sizeof(S.meta), &S.meta);
    } else {
        S.hr_meta = E_NOINTERFACE;
    }
    S.mode = m;
    read_desc(false);
    char a[48], b[48], c[48];
    ev("mode %s: ResizeBuffers(%s) %s, SetColorSpace1(%s) %s, SetHDRMetaData %s", kModeLabel[m],
       fmt_str(mode_format(m)), hr_str(S.hr_resize, a, sizeof(a)), cs_str(mode_cs(m)),
       hr_str(S.hr_cs, b, sizeof(b)), hr_str(S.hr_meta, c, sizeof(c)));
    if (m != MODE_SDR && (FAILED(S.hr_resize) || FAILED(S.hr_cs))) {
        S.mode_failed[m] = true;
        ev("mode %s refused, falling back to SDR", kModeLabel[m]);
        apply_mode(h, MODE_SDR);
    }
}

// ---------------------------------------------------------------------------
// Card pipeline + UI layer
// ---------------------------------------------------------------------------
bool compile(const char *src, const char *entry, const char *target, ID3DBlob **out) {
    *out = nullptr;
    ID3DBlob *err = nullptr;
    HRESULT hr = D3DCompile(src, strlen(src), "hdr_card.hlsl", nullptr, nullptr, entry, target, 0, 0, out, &err);
    if (FAILED(hr) || !*out) {
        char m[320];
        snprintf(m, sizeof(m), "shader %s (%s) failed: 0x%08lX %.200s", entry, target, (unsigned long)hr,
                 err ? (const char *)err->GetBufferPointer() : "(no compiler log)");
        ev("%s", m);
        snprintf(S.pipe_err, sizeof(S.pipe_err), "the card's shader %s failed to compile", entry);
        if (err) err->Release();
        if (*out) {
            (*out)->Release();
            *out = nullptr;
        }
        return false;
    }
    if (err) err->Release();
    return true;
}

template <typename T> void safe_release(T *&p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

void release_pipeline() {
    safe_release(S.vs);
    safe_release(S.ps);
    safe_release(S.vs_full);
    safe_release(S.ps_comp);
    safe_release(S.il);
    safe_release(S.vb);
    safe_release(S.cb_comp);
    safe_release(S.rs);
    safe_release(S.bs_premul);
    S.pipe_ok = false;
}

void release_ui_layer() {
    safe_release(S.ui_srv);
    safe_release(S.ui_rtv);
    safe_release(S.ui_tex);
    S.ui_w = S.ui_h = 0;
}

void create_pipeline(ID3D11Device *dev) {
    release_pipeline();
    S.pipe_err[0] = '\0';
    ID3DBlob *vsb = nullptr, *psb = nullptr, *vfb = nullptr, *pcb = nullptr;
    bool ok = compile(kCardHLSL, "VSMain", "vs_4_0", &vsb) && compile(kCardHLSL, "PSMain", "ps_4_0", &psb) &&
              compile(kCompHLSL, "VSFull", "vs_4_0", &vfb) && compile(kCompHLSL, "PSComp", "ps_4_0", &pcb);
    if (ok) {
        dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &S.vs);
        dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &S.ps);
        dev->CreateVertexShader(vfb->GetBufferPointer(), vfb->GetBufferSize(), nullptr, &S.vs_full);
        dev->CreatePixelShader(pcb->GetBufferPointer(), pcb->GetBufferSize(), nullptr, &S.ps_comp);
        const D3D11_INPUT_ELEMENT_DESC il[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0},
        };
        dev->CreateInputLayout(il, 4, vsb->GetBufferPointer(), vsb->GetBufferSize(), &S.il);
    }
    safe_release(vsb);
    safe_release(psb);
    safe_release(vfb);
    safe_release(pcb);

    D3D11_BUFFER_DESC bd;
    ZeroMemory(&bd, sizeof(bd));
    bd.ByteWidth = (UINT)(sizeof(CardVtx) * 6 * kMaxQuads);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    dev->CreateBuffer(&bd, nullptr, &S.vb);
    ZeroMemory(&bd, sizeof(bd));
    bd.ByteWidth = 16;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    dev->CreateBuffer(&bd, nullptr, &S.cb_comp);

    D3D11_RASTERIZER_DESC rd;
    ZeroMemory(&rd, sizeof(rd));
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;  // fullscreen triangle + quads of either winding
    rd.DepthClipEnable = TRUE;
    dev->CreateRasterizerState(&rd, &S.rs);

    D3D11_BLEND_DESC bld;
    ZeroMemory(&bld, sizeof(bld));
    bld.RenderTarget[0].BlendEnable = TRUE;
    bld.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    bld.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bld.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bld.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bld.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bld.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    dev->CreateBlendState(&bld, &S.bs_premul);

    S.pipe_ok = S.vs && S.ps && S.vs_full && S.ps_comp && S.il && S.vb && S.cb_comp && S.rs && S.bs_premul;
    if (!S.pipe_ok && !S.pipe_err[0]) snprintf(S.pipe_err, sizeof(S.pipe_err), "D3D11 object creation failed");
    ev("card pipeline: %s", S.pipe_ok ? "ready" : S.pipe_err);
}

bool ensure_ui_layer(ID3D11Device *dev, UINT w, UINT h) {
    if (w == 0 || h == 0) return false;
    if (S.ui_tex && S.ui_w == w && S.ui_h == h) return true;
    release_ui_layer();
    D3D11_TEXTURE2D_DESC td;
    ZeroMemory(&td, sizeof(td));
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &S.ui_tex)) || !S.ui_tex) return false;
    dev->CreateRenderTargetView(S.ui_tex, nullptr, &S.ui_rtv);
    dev->CreateShaderResourceView(S.ui_tex, nullptr, &S.ui_srv);
    if (!S.ui_rtv || !S.ui_srv) {
        release_ui_layer();
        return false;
    }
    S.ui_w = w;
    S.ui_h = h;
    return true;
}

// ---------------------------------------------------------------------------
// Readout rows (shared by the on-screen panel and the report file)
// ---------------------------------------------------------------------------
enum { R_HEAD = 0, R_KV = 1, R_TEXT = 2 };
struct Row {
    int kind;
    const char *k;
    char v[288];
    ImU32 c;
};
Row g_rows[80];
int g_nrows = 0;

void row_head(const char *title) {
    if (g_nrows >= (int)(sizeof(g_rows) / sizeof(g_rows[0]))) return;
    Row &r = g_rows[g_nrows++];
    r.kind = R_HEAD;
    r.k = title;
    r.v[0] = '\0';
    r.c = C_ACCENT;
}

void row_kv(const char *k, ImU32 c, const char *fmt, ...) {
    if (g_nrows >= (int)(sizeof(g_rows) / sizeof(g_rows[0]))) return;
    Row &r = g_rows[g_nrows++];
    r.kind = R_KV;
    r.k = k;
    r.c = c;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r.v, sizeof(r.v), fmt, ap);
    va_end(ap);
}

void row_text(ImU32 c, const char *fmt, ...) {
    if (g_nrows >= (int)(sizeof(g_rows) / sizeof(g_rows[0]))) return;
    Row &r = g_rows[g_nrows++];
    r.kind = R_TEXT;
    r.k = "";
    r.c = c;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r.v, sizeof(r.v), fmt, ap);
    va_end(ap);
}

void build_rows() {
    g_nrows = 0;
    char b1[48], b2[48];

    row_head("DXGI output  (IDXGIOutput6::GetDesc1)");
    if (S.have_desc) {
        const DXGI_OUTPUT_DESC1 &d = S.desc;
        bool hdr_on = d.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
        row_kv("Colour space", hdr_on ? C_GOOD : C_WARN, "%s: HDR %s", cs_str(d.ColorSpace), hdr_on ? "on" : "off");
        if (S.desc_start.ColorSpace != d.ColorSpace)
            row_kv("At scene start", C_MUTED, "%s", cs_str(S.desc_start.ColorSpace));
        row_kv("Bits per colour", C_TEXT, "%u", d.BitsPerColor);
        row_kv("Max luminance", C_TEXT, "%.1f nits", d.MaxLuminance);
        row_kv("Full-frame max", C_TEXT, "%.1f nits", d.MaxFullFrameLuminance);
        row_kv("Min luminance", C_TEXT, "%.4f nits", d.MinLuminance);
        row_kv("Red primary", C_TEXT, "%.4f, %.4f", d.RedPrimary[0], d.RedPrimary[1]);
        row_kv("Green primary", C_TEXT, "%.4f, %.4f", d.GreenPrimary[0], d.GreenPrimary[1]);
        row_kv("Blue primary", C_TEXT, "%.4f, %.4f", d.BluePrimary[0], d.BluePrimary[1]);
        row_kv("White point", C_TEXT, "%.4f, %.4f", d.WhitePoint[0], d.WhitePoint[1]);
        if (p3_standin_primaries())
            row_kv("Primaries", C_WARN, "DXVK's P3 stand-in (no chromaticity data arrived)");
    } else {
        row_kv("GetDesc1", C_BAD, "%s", S.out6 ? "failed" : "IDXGIOutput6 not available");
    }
    {
        char v[200];
        ImU32 vc = C_TEXT;
        verdict_text(v, sizeof(v), &vc);
        row_kv("Verdict", vc, "%s", v);
    }

    row_head("Swap chain");
    if (S.flip)
        row_kv("Model", C_TEXT, "flip-discard, 2 buffers, created %u x %u", S.create_w, S.create_h);
    else
        row_kv("Model", C_WARN, "the shell's own bitblt swapchain (%s)", S.legacy_why);
    row_kv("Current mode", C_TEXT, "%s: %s + %s", kModeLabel[S.mode], fmt_str(mode_format(S.mode)),
           cs_str(mode_cs(S.mode)));
    if (S.sup_hdr10 == SUP_YES)
        row_kv("HDR10 support", C_GOOD, "yes (R10G10B10A2 + G2084 P2020)");
    else
        row_kv("HDR10 support", C_BAD, "%s: %s", sup_str(S.sup_hdr10), hdr10_reason());
    row_kv("scRGB support", S.sup_scrgb == SUP_YES ? C_TEXT : C_WARN, "%s (R16G16B16A16 float + G10 P709)",
           sup_str(S.sup_scrgb));
    row_text(C_MUTED,
             "DXVK answers yes for scRGB on every FP16 swapchain and converts it to what the surface takes, so "
             "this does not show what the compositor offers. The Vulkan section below does.");
    row_kv("sRGB support", S.sup_srgb == SUP_YES ? C_TEXT : C_WARN, "%s", sup_str(S.sup_srgb));
    if (!S.flip) {
        row_kv("SetColorSpace1", C_MUTED, "not called (the shell's own swapchain is in use)");
        row_kv("SetHDRMetaData", C_MUTED, "not called");
    } else {
        row_kv("SetColorSpace1", SUCCEEDED(S.hr_cs) ? C_TEXT : C_BAD, "%s", hr_str(S.hr_cs, b1, sizeof(b1)));
        if (S.mode == MODE_SDR)
            row_kv("SetHDRMetaData", SUCCEEDED(S.hr_meta) ? C_TEXT : C_BAD, "%s (type NONE in SDR)",
                   hr_str(S.hr_meta, b2, sizeof(b2)));
        else
            row_kv("SetHDRMetaData", SUCCEEDED(S.hr_meta) ? C_TEXT : C_BAD,
                   "%s (HDR10: max %u, min %.4f, MaxCLL %u, MaxFALL %u nits)", hr_str(S.hr_meta, b2, sizeof(b2)),
                   S.meta.MaxMasteringLuminance, S.meta.MinMasteringLuminance / 10000.0,
                   (unsigned)S.meta.MaxContentLightLevel, (unsigned)S.meta.MaxFrameAverageLightLevel);
    }
    row_kv("DXVK_HDR", S.dxvk_hdr_set ? C_TEXT : C_WARN, "%s", S.dxvk_hdr);
    row_kv("Adapter", C_TEXT, "%s", S.adapter[0] ? S.adapter : "(unknown)");
    if (S.cap > 0)
        row_kv("Present", C_TEXT, "vsync (sync interval 1), capped at %d fps", S.cap);
    else
        row_kv("Present", C_TEXT, "vsync (sync interval 1), no cap");

    row_head("Vulkan surface  (no DXVK involved)");
    LONG vs = g_vk_state;
    if (vs != 2) {
        row_kv("Probe", C_MUTED, "%s", vs == 1 ? "running..." : "not run");
    } else {
        const VkProbe &v = g_vk;
        if (v.error[0]) row_kv("Probe", C_WARN, "%s", v.error);
        else row_kv("Probe", C_TEXT, "done (%d surface formats)", v.nformats);
        if (v.device[0]) row_kv("Device", C_TEXT, "%s", v.device);
        if (v.instance) {
            row_kv("VK_EXT_swapchain_colorspace", v.ext_colorspace ? C_GOOD : C_WARN, "%s",
                   v.ext_colorspace ? "offered" : "not offered");
            row_kv("VK_EXT_hdr_metadata", v.dev_hdr_meta ? C_GOOD : C_WARN, "%s",
                   v.dev_hdr_meta ? "offered (metadata can reach the compositor)" : "not offered");
        }
        if (v.surface) {
            if (v.hdr10) row_kv("HDR10 (ST2084)", C_GOOD, "offered: %s", v.hdr10_fmt);
            else row_kv("HDR10 (ST2084)", C_WARN, "not offered");
            row_kv("scRGB (ext. sRGB linear)", v.scrgb ? C_GOOD : C_MUTED, "%s", v.scrgb ? "offered" : "not offered");
            row_text(C_MUTED, "Formats: %s", v.formats);
        }
    }

    row_head("Report");
    if (S.report_ok) row_kv("File", C_TEXT, "%s", S.report_path);
    else row_kv("File", C_WARN, "could not be written");
    if (S.mirror_ok) row_kv("Copy", C_TEXT, "Z:\\usr\\tmp\\AIO-Graphics-Test_hdr.txt");

    row_head("What to look for");
    row_text(C_TEXT,
             "Real HDR: the 400, 600, 1000 and max patches get brighter step by step, the ramp keeps brightening "
             "past the 203 mark up to about your screen's peak, and the sun glares.");
    row_text(C_TEXT,
             "Tone-mapped: the bright patches still differ but are squeezed together, the ramp flattens early and "
             "nothing glares.");
    row_text(C_TEXT,
             "Washed out (PQ shown as SDR): everything looks grey and flat, the 80-nit patch looks mid-grey and "
             "colours are pale.");
    row_text(C_TEXT,
             "Banding: in HDR10 the 8-bit strip shows steps and the 10-bit strip is smooth. Steps in both mean "
             "something in the chain is 8-bit.");
    row_text(C_TEXT,
             "SDR (A/B): everything from 203 up is the same white, the ramp stops brightening at 203, and the two "
             "colour rows match.");
    row_head("Keys");
    row_text(C_MUTED, "H next mode   V values / card   R re-check   F11 fullscreen");
}

bool write_report_to(const char *path) {
    FILE *fp = fopen(path, "w");
    if (!fp) return false;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(fp, "AIO Graphics Test %s - HDR test report\n", AIO_VERSION);
    fprintf(fp, "Written %04d-%02d-%02d %02d:%02d:%02d (rewritten on every mode switch, re-check and exit)\n",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    for (int i = 0; i < g_nrows; ++i) {
        const Row &r = g_rows[i];
        if (r.kind == R_HEAD) fprintf(fp, "\n[%s]\n", r.k);
        else if (r.kind == R_KV) fprintf(fp, "%-28s %s\n", r.k, r.v);
        else fprintf(fp, "  %s\n", r.v);
    }
    fprintf(fp, "\n[Events]\n");
    for (int i = 0; i < S.nev; ++i) fprintf(fp, "%s\n", S.ev[i]);
    fclose(fp);
    return true;
}

// Report into "AIO Results\HDR\" like the other tools, plus a copy in the
// container's shared tmp (Z:\usr\tmp, where the startup log goes) when writable.
void write_report() {
    char path[MAX_PATH];
    aio_results_path("HDR", "AIO-Graphics-Test_hdr.txt", path, sizeof(path));
    if (!GetFullPathNameA(path, MAX_PATH, S.report_path, nullptr))
        snprintf(S.report_path, sizeof(S.report_path), "%s", path);
    // The rows name the files being written, so build them as if both writes work;
    // the on-screen panel rebuilds from the real results every frame.
    S.report_ok = S.mirror_ok = true;
    build_rows();
    S.report_ok = write_report_to(path);
    S.mirror_ok = write_report_to("Z:\\usr\\tmp\\AIO-Graphics-Test_hdr.txt");
}

// ---------------------------------------------------------------------------
// Drawing helpers (UI pass)
// ---------------------------------------------------------------------------
ImFont *fnt(bool mono) {
    if (mono) return F.big_mono ? F.big_mono : F.mono;
    return F.big_ui ? F.big_ui : F.ui;
}

float text_w(bool mono, float px, const char *s) { return fnt(mono)->CalcTextSizeA(px, FLT_MAX, 0.0f, s).x; }

float text_h(bool mono, float px, const char *s, float wrap) {
    return fnt(mono)->CalcTextSizeA(px, FLT_MAX, wrap, s).y;
}

void text(ImDrawList *dl, bool mono, float px, float x, float y, ImU32 c, const char *s, float wrap = 0.0f) {
    dl->AddText(fnt(mono), px, ImVec2(x, y), c, s, nullptr, wrap);
}

// Touch-sized button. state: 0 normal, 1 selected, 2 disabled.
bool button(const char *id, float x, float y, float w, float h, const char *label, int state, float px) {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ImGui::PushID(id);
    bool clicked = false;
    if (state == 2) ImGui::Dummy(ImVec2(w, h));
    else clicked = ImGui::InvisibleButton("b", ImVec2(w, h));
    bool hov = state != 2 && ImGui::IsItemHovered();
    ImGui::PopID();
    ImVec2 p(x, y), mx(x + w, y + h);
    ImU32 bg = state == 1 ? C_ACCENT : (state == 2 ? IM_COL32(18, 24, 30, 210)
                                                    : (hov ? IM_COL32(40, 55, 70, 240) : IM_COL32(22, 30, 40, 230)));
    float rr = h * 0.22f;
    dl->AddRectFilled(p, mx, bg, rr);
    dl->AddRect(p, mx, state == 1 ? C_ACCENT : IM_COL32(255, 255, 255, state == 2 ? 20 : 48), rr, 0, 1.2f);
    ImU32 tc = state == 1 ? IM_COL32(4, 35, 31, 255) : (state == 2 ? C_FAINT : C_TEXT);
    ImVec2 ts = fnt(false)->CalcTextSizeA(px, FLT_MAX, 0.0f, label);
    dl->AddText(fnt(false), px, ImVec2(x + (w - ts.x) * 0.5f, y + (h - ts.y) * 0.5f), tc, label);
    return clicked;
}

void add_quad(float x0, float y0, float x1, float y1, int kind, float a, float b, float c, float d = 0.0f,
              float e = 0.0f, float f = 0.0f) {
    if (g_nquad >= kMaxQuads || x1 <= x0 || y1 <= y0) return;
    CardQuad &q = g_quad[g_nquad++];
    q.x0 = x0;
    q.y0 = y0;
    q.x1 = x1;
    q.y1 = y1;
    q.p0[0] = (float)kind;
    q.p0[1] = a;
    q.p0[2] = b;
    q.p0[3] = c;
    q.p1[0] = d;
    q.p1[1] = e;
    q.p1[2] = f;
    q.p1[3] = 0.0f;
}

void request_mode(int m) {
    if (m < 0 || m >= MODE_COUNT || !mode_available(m)) return;
    S.pending_mode = m;
    S.sdr_chosen = (m == MODE_SDR);
}

int next_mode() {
    for (int i = 1; i <= MODE_COUNT; ++i) {
        int m = (S.mode + i) % MODE_COUNT;
        if (mode_available(m)) return m;
    }
    return S.mode;
}

// Status band: what is on screen, the EDID verdict, the key DXGI numbers, and the
// mode / values buttons. Returns the band's bottom edge.
float draw_status_band(ImDrawList *dl, ImVec2 o, float w, float s, float fps, bool fullscreen) {
    const float M = 14.0f * s;
    const float x0 = o.x + M, x1 = o.x + w - M;
    // Keep clear of the shell's fullscreen control in the viewport's top-right.
    const float right = o.x + w - (fullscreen ? 190.0f : 58.0f);
    float y = o.y + M * 0.8f;

    char title[64], sub[256];
    ImU32 tcol = C_TEXT;
    if (S.legacy) {
        snprintf(title, sizeof(title), "SDR ONLY");
        tcol = C_WARN;
        snprintf(sub, sizeof(sub), "%s", S.legacy_why);
    } else if (S.mode == MODE_HDR10) {
        snprintf(title, sizeof(title), "HDR10 ON");
        tcol = C_GOOD;
        snprintf(sub, sizeof(sub), "PQ BT.2020 \xC2\xB7 R10G10B10A2 10-bit \xC2\xB7 flip model \xC2\xB7 vsync");
    } else if (S.mode == MODE_SCRGB) {
        snprintf(title, sizeof(title), "scRGB ON");
        tcol = C_GOOD;
        snprintf(sub, sizeof(sub), "linear BT.709 \xC2\xB7 R16G16B16A16 float \xC2\xB7 DXVK converts it to %s for the screen",
                 S.sup_hdr10 == SUP_YES ? "PQ" : "sRGB");
    } else if (mode_available(MODE_HDR10)) {
        snprintf(title, sizeof(title), "SDR");
        snprintf(sub, sizeof(sub), "sRGB \xC2\xB7 R8G8B8A8 8-bit \xC2\xB7 the A/B reference: what a non-HDR game sends");
    } else {
        snprintf(title, sizeof(title), "HDR10 NOT AVAILABLE");
        tcol = C_BAD;
        snprintf(sub, sizeof(sub), "showing SDR: %s", hdr10_reason());
    }
    if (S.cap > 0 && !S.legacy) appendf(sub, sizeof(sub), " \xC2\xB7 capped %d fps", S.cap);

    const float t_px = 26.0f * s, v_px = 15.5f * s, i_px = 14.0f * s, b_px = 15.0f * s;
    float bh = 38.0f * s;
    if (bh < 34.0f) bh = 34.0f;
    const float segw = 82.0f * s, valw = 96.0f * s, gap = 8.0f * s;
    const float btns_w = 3.0f * segw + gap * 1.5f + valw;
    const float title_w = text_w(false, t_px, title);
    const bool wrap = x0 + title_w + 2.0f * gap + btns_w > right;
    float row_h = t_px * 1.2f;
    if (!wrap && bh > row_h) row_h = bh;
    text(dl, false, t_px, x0, y + (row_h - t_px * 1.2f) * 0.5f, tcol, title);
    float by = y + (row_h - bh) * 0.5f;
    y += row_h + 2.0f * s;

    const float ww = x1 - x0;
    text(dl, false, v_px, x0, y, C_MUTED, sub, ww);
    y += text_h(false, v_px, sub, ww) + 3.0f * s;

    char vt[200];
    ImU32 vc = C_TEXT;
    verdict_text(vt, sizeof(vt), &vc);
    text(dl, false, v_px, x0, y, vc, vt, ww);
    y += text_h(false, v_px, vt, ww) + 3.0f * s;

    char info[200];
    if (S.have_desc)
        snprintf(info, sizeof(info),
                 "DXGI  max %.0f \xC2\xB7 full-frame %.0f \xC2\xB7 min %.4f nits \xC2\xB7 %u bpc \xC2\xB7 DXVK_HDR=%s \xC2\xB7 %.0f fps",
                 S.desc.MaxLuminance, S.desc.MaxFullFrameLuminance, S.desc.MinLuminance, S.desc.BitsPerColor,
                 S.dxvk_hdr, fps);
    else
        snprintf(info, sizeof(info), "DXGI output description unavailable \xC2\xB7 DXVK_HDR=%s \xC2\xB7 %.0f fps",
                 S.dxvk_hdr, fps);
    text(dl, true, i_px, x0, y, C_TEXT, info, ww);
    y += text_h(true, i_px, info, ww) + 4.0f * s;

    if (wrap) {
        by = y;
        y += bh + 6.0f * s;
    }
    float bx = wrap ? x0 : right - btns_w;
    for (int m = 0; m < MODE_COUNT; ++m) {
        int st = (S.mode == m) ? 1 : (mode_available(m) ? 0 : 2);
        char id[16];
        snprintf(id, sizeof(id), "##hdrmode%d", m);
        if (button(id, bx + m * segw, by, segw - 2.0f * s, bh, kModeLabel[m], st, b_px)) request_mode(m);
    }
    bx += 3.0f * segw + gap * 1.5f;
    if (button("##hdrvalues", bx, by, valw, bh, S.show_values ? "Card" : "Values", S.show_values ? 1 : 0, b_px))
        S.show_values = !S.show_values;

    dl->AddLine(ImVec2(x0, y + 1.0f), ImVec2(x1, y + 1.0f), C_LINE, 1.0f);
    return y + 8.0f * s;
}

// The patterns, top to bottom: luminance patches, PQ ramp, 10-bit vs 8-bit strips,
// then the sun box beside the BT.709 / BT.2020 colour rows. Records the quads for
// the render pass and draws their labels.
void draw_patterns(ImDrawList *dl, float X0, float Y0, float X1, float Y1, float s) {
    const float W = X1 - X0, H = Y1 - Y0;
    if (W < 80.0f || H < 80.0f) return;
    const float gap = 10.0f * s, cap = 13.0f * s;
    const float maxn = max_nits();
    const float hA = H * 0.26f, hB = H * 0.17f, hC = H * 0.18f;
    const float hD = H - hA - hB - hC - 3.0f * gap;

    // A: luminance patches (neutral grey) with their level underneath.
    {
        const float lab = cap * 2.6f;
        float ps = (W - 6.0f * gap) / 7.0f;
        if (ps > hA - lab) ps = hA - lab;
        if (ps < 8.0f) ps = 8.0f;
        const float rx = X0 + (W - (7.0f * ps + 6.0f * gap)) * 0.5f;
        static const char *const kSub[7] = {"", "SDR white", "", "", "", "DXGI max", "PQ max"};
        for (int i = 0; i < 7; ++i) {
            float nits = (i == 5) ? maxn : kPatchNits[i];
            float px = rx + i * (ps + gap);
            add_quad(px, Y0, px + ps, Y0 + ps, K_GREY, nits, 0.0f, 0.0f);
            char num[16];
            snprintf(num, sizeof(num), "%.0f", nits);
            float nw = text_w(true, cap * 1.1f, num);
            text(dl, true, cap * 1.1f, px + (ps - nw) * 0.5f, Y0 + ps + 3.0f * s, C_TEXT, num);
            const char *sub = (i == 5 && !have_max()) ? "assumed" : kSub[i];
            if (sub[0]) {
                float sw = text_w(false, cap * 0.9f, sub);
                text(dl, false, cap * 0.9f, px + (ps - sw) * 0.5f, Y0 + ps + 3.0f * s + cap * 1.3f, C_MUTED, sub);
            }
        }
    }

    // B: PQ ramp 0 -> 10000 nits, linear in PQ code, with nits ticks and the DXGI max.
    const float by = Y0 + hA + gap;
    {
        const char *capt = "PQ ramp  0 -> 10000 nits: it stops getting brighter where your screen clips";
        if (text_w(false, cap, capt) > W * 0.72f) capt = "PQ ramp  0 -> 10000 nits";
        text(dl, false, cap, X0, by, C_MUTED, capt);
        float rt = by + cap * 1.6f, rb = by + hB - cap * 1.7f;
        if (rb < rt + 6.0f) rb = rt + 6.0f;
        add_quad(X0, rt, X1, rb, K_RAMP, 0.0f, 0.0f, 0.0f);
        static const float kTicks[] = {0.0f, 100.0f, 203.0f, 400.0f, 1000.0f, 4000.0f, 10000.0f};
        for (float n : kTicks) {
            float x = X0 + W * pq_code(n);
            dl->AddLine(ImVec2(x, rb), ImVec2(x, rb + 5.0f * s), C_MUTED, 1.0f);
            char t[16];
            snprintf(t, sizeof(t), "%.0f", n);
            float tw = text_w(true, cap * 0.95f, t);
            float tx = x - tw * 0.5f;
            if (tx < X0) tx = X0;
            if (tx + tw > X1) tx = X1 - tw;
            text(dl, true, cap * 0.95f, tx, rb + 6.0f * s, C_MUTED, t);
        }
        float mxp = X0 + W * pq_code(maxn);
        dl->AddTriangleFilled(ImVec2(mxp - 5.0f * s, rt - 7.0f * s), ImVec2(mxp + 5.0f * s, rt - 7.0f * s),
                              ImVec2(mxp, rt - 1.0f * s), C_WARN);
        char ml[40];
        snprintf(ml, sizeof(ml), have_max() ? "DXGI max %.0f" : "assumed max %.0f", maxn);
        float mw = text_w(true, cap * 0.95f, ml);
        float mlx = mxp + 8.0f * s;
        if (mlx + mw > X1) mlx = mxp - 8.0f * s - mw;
        text(dl, true, cap * 0.95f, mlx, rt - cap * 1.45f, C_WARN, ml);
    }

    // C: banding strips, black -> 203 nits; the lower one is quantised to 8-bit steps.
    const float cy = by + hB + gap;
    {
        const char *capt = (S.mode == MODE_SDR)
                               ? "Banding  black -> SDR white: the swapchain is 8-bit now, so both strips step"
                               : "Banding  black -> 203 nits: the 8-bit strip should step, the 10-bit strip should be smooth";
        text(dl, false, cap, X0, cy, C_MUTED, capt, W);
        float capt_h = text_h(false, cap, capt, W);
        float lw = 70.0f * s;
        float gt = cy + capt_h + 4.0f * s;
        float gh = (cy + hC - gt - 4.0f * s) * 0.5f;
        if (gh < 6.0f) gh = 6.0f;
        add_quad(X0 + lw, gt, X1, gt + gh, K_GRAD, kSdrWhite, 0.0f, 0.0f);
        add_quad(X0 + lw, gt + gh + 4.0f * s, X1, gt + 2.0f * gh + 4.0f * s, K_GRAD8, kSdrWhite, 0.0f, 0.0f);
        text(dl, true, cap, X0, gt + gh * 0.5f - cap * 0.6f, C_TEXT, "10-bit");
        text(dl, true, cap, X0, gt + gh + 4.0f * s + gh * 0.5f - cap * 0.6f, C_TEXT, "8-bit");
    }

    // D: the sun (left) and the colour rows (right).
    const float dy = cy + hC + gap;
    if (hD > 30.0f) {
        const float sw = W * 0.42f;
        // A slow Lissajous path: about 26 s across, 19 s up and down.
        const double t = scene_time();
        const float sx = 0.5f + 0.30f * (float)sin(t * 6.2831853 / 26.0);
        const float sy = 0.46f + 0.22f * (float)sin(t * 6.2831853 / 19.0 + 0.8);
        add_quad(X0, dy, X0 + sw, dy + hD, K_SUN, sx, sy, 0.11f, sw / hD, maxn, 0.0f);
        dl->AddRect(ImVec2(X0, dy), ImVec2(X0 + sw, dy + hD), C_LINE, 0.0f, 0, 1.0f);
        char sl[48];
        snprintf(sl, sizeof(sl), have_max() ? "sun core = DXGI max (%.0f nits)" : "sun core = %.0f nits (assumed)",
                 maxn);
        text(dl, false, cap * 0.95f, X0 + 6.0f * s, dy + 4.0f * s, C_MUTED, sl, sw - 12.0f * s);

        const float gx0 = X0 + sw + gap * 1.5f, gw = X1 - gx0;
        const char *capt = "BT.709 (top) vs BT.2020 (bottom): the bottom row is deeper in HDR, the same in SDR";
        text(dl, false, cap * 0.95f, gx0, dy, C_MUTED, capt, gw);
        float capt_h = text_h(false, cap * 0.95f, capt, gw);
        const float lw2 = 62.0f * s, g2 = 6.0f * s;
        const float cw = (gw - lw2 - 5.0f * g2) / 6.0f;
        const float top = dy + capt_h + 5.0f * s;
        const float ch = (dy + hD - top - cap * 1.5f - g2) * 0.5f;
        if (cw > 4.0f && ch > 4.0f) {
            // Primaries and secondaries, linear light in BT.2020 at SDR white.
            static const float kBase[6][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {0, 1, 1}, {1, 0, 1}, {1, 1, 0}};
            static const char *const kName[6] = {"R", "G", "B", "C", "M", "Y"};
            // BT.709 -> BT.2020 (ITU-R BT.2087).
            static const float k709[3][3] = {{0.6274040f, 0.3292820f, 0.0433136f},
                                             {0.0690970f, 0.9195400f, 0.0113612f},
                                             {0.0163916f, 0.0880132f, 0.8955950f}};
            for (int i = 0; i < 6; ++i) {
                const float *c = kBase[i];
                float r709[3];
                for (int k = 0; k < 3; ++k)
                    r709[k] = (k709[k][0] * c[0] + k709[k][1] * c[1] + k709[k][2] * c[2]) * kSdrWhite;
                float cx = gx0 + lw2 + i * (cw + g2);
                add_quad(cx, top, cx + cw, top + ch, K_COLOUR, 0.0f, 0.0f, 0.0f, r709[0], r709[1], r709[2]);
                add_quad(cx, top + ch + g2, cx + cw, top + 2.0f * ch + g2, K_COLOUR, 0.0f, 0.0f, 0.0f,
                         c[0] * kSdrWhite, c[1] * kSdrWhite, c[2] * kSdrWhite);
                float nw = text_w(true, cap, kName[i]);
                text(dl, true, cap, cx + (cw - nw) * 0.5f, top + 2.0f * ch + g2 + 3.0f * s, C_MUTED, kName[i]);
            }
            text(dl, true, cap * 0.95f, gx0, top + ch * 0.5f - cap * 0.55f, C_TEXT, "BT.709");
            text(dl, true, cap * 0.95f, gx0, top + ch + g2 + ch * 0.5f - cap * 0.55f, C_TEXT, "BT.2020");
        }
    }
}

// Lays the rows out from y; draws them when dl is set, else only measures. Returns
// the bottom edge.
float layout_rows(ImDrawList *dl, float x0, float x1, float y, float px, float s) {
    const float kw = (x1 - x0) * 0.36f;
    const float vw = x1 - x0 - kw;
    for (int i = 0; i < g_nrows; ++i) {
        const Row &r = g_rows[i];
        if (r.kind == R_HEAD) {
            if (i > 0) y += 10.0f * s;
            if (dl) text(dl, false, px * 1.08f, x0, y, r.c, r.k);
            y += px * 1.08f * 1.35f;
            if (dl) dl->AddLine(ImVec2(x0, y - 3.0f * s), ImVec2(x1, y - 3.0f * s), C_LINE, 1.0f);
            y += 2.0f * s;
        } else if (r.kind == R_KV) {
            float kh = text_h(true, px, r.k, kw - 8.0f * s);
            float vh = text_h(true, px, r.v, vw);
            if (dl) {
                text(dl, true, px, x0, y, C_MUTED, r.k, kw - 8.0f * s);
                text(dl, true, px, x0 + kw, y, r.c, r.v, vw);
            }
            y += (kh > vh ? kh : vh) + 4.0f * s;
        } else {
            float th = text_h(false, px, r.v, x1 - x0);
            if (dl) text(dl, false, px, x0, y, r.c, r.v, x1 - x0);
            y += th + 5.0f * s;
        }
    }
    return y;
}

// Values panel: every DXGI / swapchain / Vulkan value plus the reading guide, in a
// panel over the card. Drag (or wheel) to scroll; nothing needs a keyboard.
void draw_values(ImDrawList *dl, float X0, float Y0, float X1, float Y1, float s) {
    if (X1 - X0 < 80.0f || Y1 - Y0 < 80.0f) return;
    build_rows();
    const float pad = 14.0f * s;
    dl->AddRectFilled(ImVec2(X0, Y0), ImVec2(X1, Y1), C_PANEL, 10.0f * s);
    dl->AddRect(ImVec2(X0, Y0), ImVec2(X1, Y1), C_LINE, 10.0f * s, 0, 1.0f);

    float bh = 36.0f * s;
    if (bh < 32.0f) bh = 32.0f;
    const float bw = 124.0f * s, gap = 8.0f * s, b_px = 15.0f * s;
    float hx = X1 - pad - bw;
    if (button("##hdrrecheck", hx, Y0 + pad, bw, bh, "Re-check", 0, b_px)) S.recheck = true;
    char pl[32];
    snprintf(pl, sizeof(pl), "Pace: %s", S.cap == 60 ? "60 fps" : (S.cap == 30 ? "30 fps" : "vsync"));
    float pw = bw * 1.2f;
    if (button("##hdrpace", hx - gap - pw, Y0 + pad, pw, bh, pl, 0, b_px))
        S.cap = (S.cap == 60) ? 30 : (S.cap == 30 ? 0 : 60);
    text(dl, false, 22.0f * s, X0 + pad, Y0 + pad + (bh - 22.0f * s * 1.2f) * 0.5f, C_TEXT, "Values");

    const float cx0 = X0 + pad, cx1 = X1 - pad - 8.0f * s;
    const float cy0 = Y0 + pad + bh + 10.0f * s, cy1 = Y1 - pad;
    if (cy1 - cy0 < 20.0f) return;
    float px = 15.0f * s;
    if (px < 11.0f) px = 11.0f;
    const float content_h = layout_rows(nullptr, cx0, cx1, 0.0f, px, s);
    const float view_h = cy1 - cy0;

    // Drag / wheel scrolling over the rows (touch arrives as mouse drag under Wine).
    ImGui::SetCursorScreenPos(ImVec2(cx0, cy0));
    ImGui::InvisibleButton("##hdrvalscroll", ImVec2(cx1 - cx0, view_h));
    ImGuiIO &io = ImGui::GetIO();
    if (ImGui::IsItemActive()) S.scroll -= io.MouseDelta.y;
    if (ImGui::IsItemHovered() && io.MouseWheel != 0.0f) S.scroll -= io.MouseWheel * 48.0f * s;
    float max_scroll = content_h - view_h;
    if (max_scroll < 0.0f) max_scroll = 0.0f;
    if (S.scroll > max_scroll) S.scroll = max_scroll;
    if (S.scroll < 0.0f) S.scroll = 0.0f;

    dl->PushClipRect(ImVec2(cx0, cy0), ImVec2(cx1 + 8.0f * s, cy1), true);
    layout_rows(dl, cx0, cx1, cy0 - S.scroll, px, s);
    dl->PopClipRect();

    if (max_scroll > 0.0f) {  // scroll position indicator
        float track = view_h;
        float thumb = track * (view_h / content_h);
        if (thumb < 24.0f * s) thumb = 24.0f * s;
        float ty = cy0 + (track - thumb) * (S.scroll / max_scroll);
        float tx = X1 - pad + 1.0f * s;
        dl->AddRectFilled(ImVec2(tx, cy0), ImVec2(tx + 4.0f * s, cy1), IM_COL32(255, 255, 255, 18), 2.0f * s);
        dl->AddRectFilled(ImVec2(tx, ty), ImVec2(tx + 4.0f * s, ty + thumb), IM_COL32(255, 255, 255, 90), 2.0f * s);
    }
}

// Frame cap on top of vsync: sleeps so frames start at most `cap` times a second.
void pace() {
    static LARGE_INTEGER freq, last;
    if (S.cap <= 0) {
        last.QuadPart = 0;
        return;
    }
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (last.QuadPart && freq.QuadPart) {
        double el = (double)(now.QuadPart - last.QuadPart) * 1000.0 / (double)freq.QuadPart;
        double target = 1000.0 / (double)S.cap;
        if (el >= 0.0 && el < target - 1.0) {
            Sleep((DWORD)(target - el));
            QueryPerformanceCounter(&now);
        }
    }
    last = now;
}

}  // namespace

// ===========================================================================
// Public entry points
// ===========================================================================
bool aio_hdr_is_active(void) { return S.active; }

void aio_hdr_enter(const AioHdrHost *h) {
    if (S.active || !h || !h->dev || !h->ctx || !h->swap || !h->rtv) return;
    release_pipeline();
    release_ui_layer();
    S = State();
    S.pending_mode = -1;
    S.cap = 60;
    {
        LARGE_INTEGER n;
        QueryPerformanceCounter(&n);
        S.t0 = n.QuadPart;
    }
    DWORD en = GetEnvironmentVariableA("DXVK_HDR", S.dxvk_hdr, sizeof(S.dxvk_hdr));
    S.dxvk_hdr_set = en > 0 && en < sizeof(S.dxvk_hdr);
    if (!S.dxvk_hdr_set) snprintf(S.dxvk_hdr, sizeof(S.dxvk_hdr), "(not set)");
    ev("enter: AIO %s, DXVK_HDR=%s", AIO_VERSION, S.dxvk_hdr);

    IDXGIAdapter *ad = nullptr;
    IDXGIFactory2 *f2 = nullptr;
    dxgi_objects(h->dev, &ad, nullptr, &f2);
    if (ad) {
        DXGI_ADAPTER_DESC d;
        if (SUCCEEDED(ad->GetDesc(&d)))
            WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, S.adapter, sizeof(S.adapter), nullptr, nullptr);
    }
    RECT rc;
    GetClientRect(h->hwnd, &rc);
    S.create_w = (UINT)((rc.right - rc.left) > 8 ? (rc.right - rc.left) : 8);
    S.create_h = (UINT)((rc.bottom - rc.top) > 8 ? (rc.bottom - rc.top) : 8);

    if (!f2) {
        S.legacy = true;
        snprintf(S.legacy_why, sizeof(S.legacy_why), "no IDXGIFactory2 (DXGI 1.2): a flip-model swapchain cannot be created");
        ev("%s", S.legacy_why);
    } else {
        // A window holds one flip-model swapchain, so the shell's goes first.
        bb_release(h);
        h->ctx->ClearState();
        h->ctx->Flush();
        if (*h->swap) {
            (*h->swap)->Release();
            *h->swap = nullptr;
        }
        DXGI_SWAP_CHAIN_DESC1 d1;
        ZeroMemory(&d1, sizeof(d1));
        d1.Width = S.create_w;
        d1.Height = S.create_h;
        d1.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
        d1.SampleDesc.Count = 1;
        d1.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d1.BufferCount = 2;
        d1.Scaling = DXGI_SCALING_STRETCH;
        d1.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        d1.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
        IDXGISwapChain1 *sc1 = nullptr;
        S.hr_create = f2->CreateSwapChainForHwnd(h->dev, h->hwnd, &d1, nullptr, nullptr, &sc1);
        char b[48];
        ev("CreateSwapChainForHwnd flip-discard R10G10B10A2 x2 %ux%u: %s", S.create_w, S.create_h,
           hr_str(S.hr_create, b, sizeof(b)));
        if (SUCCEEDED(S.hr_create) && sc1) {
            *h->swap = sc1;  // the reference moves into the host's slot
            S.flip = true;
        } else {
            if (sc1) sc1->Release();
            S.legacy = true;
            snprintf(S.legacy_why, sizeof(S.legacy_why), "the flip-model swapchain could not be created (%s)", b);
            create_shell_swapchain(h);
        }
    }
    if (f2) f2->Release();
    if (ad) ad->Release();
    bb_create(h);

    if (*h->swap) {
        (*h->swap)->QueryInterface(__uuidof(IDXGISwapChain3), (void **)&S.sc3);
        (*h->swap)->QueryInterface(__uuidof(IDXGISwapChain4), (void **)&S.sc4);
        IDXGIOutput *out = nullptr;
        if (FAILED((*h->swap)->GetContainingOutput(&out)) || !out) {
            IDXGIAdapter *a2 = nullptr;
            dxgi_objects(h->dev, &a2, nullptr, nullptr);
            if (a2) {
                a2->EnumOutputs(0, &out);
                a2->Release();
            }
        }
        if (out) {
            out->QueryInterface(__uuidof(IDXGIOutput6), (void **)&S.out6);
            out->Release();
        }
    }
    read_desc(true);
    if (S.have_desc)
        ev("GetDesc1: %s, %u bpc, max %.1f, full-frame %.1f, min %.4f nits", cs_str(S.desc.ColorSpace),
           S.desc.BitsPerColor, S.desc.MaxLuminance, S.desc.MaxFullFrameLuminance, S.desc.MinLuminance);
    else
        ev("GetDesc1 unavailable (IDXGIOutput6 %s)", S.out6 ? "present" : "missing");
    build_meta();
    create_pipeline(h->dev);
    probe_support(h);
    S.active = true;
    apply_mode(h, best_mode());
    vk_probe_kick();
    write_report();
}

void aio_hdr_leave(const AioHdrHost *h) {
    if (!S.active || !h) return;
    ev("leave");
    write_report();
    release_pipeline();
    release_ui_layer();
    safe_release(S.sc4);
    safe_release(S.sc3);
    safe_release(S.out6);
    if (S.flip) {
        bb_release(h);
        h->ctx->ClearState();
        h->ctx->Flush();
        if (*h->swap) {
            (*h->swap)->Release();
            *h->swap = nullptr;
        }
        create_shell_swapchain(h);
        bb_create(h);
    } else {
        h->ctx->ClearState();
    }
    S.flip = false;
    S.active = false;
    g_nquad = 0;
}

void aio_hdr_begin_frame(const AioHdrHost *h) {
    if (!S.active || !h) return;
    pace();
    if (S.recheck) {
        // Ask again: the drawer toggles can change what the surface offers.
        S.recheck = false;
        for (int m = 0; m < MODE_COUNT; ++m) S.mode_failed[m] = false;
        ev("re-check");
        probe_support(h);
        int m = (S.mode == MODE_SDR && S.sdr_chosen) ? MODE_SDR
                                                     : (mode_available(S.mode) ? S.mode : best_mode());
        apply_mode(h, m);
        vk_probe_kick();
        write_report();
    } else if (S.pending_mode >= 0) {
        int m = S.pending_mode;
        S.pending_mode = -1;
        if (m != S.mode && mode_available(m)) {
            apply_mode(h, m);
            write_report();
        }
    }
    if (g_vk_state == 2 && !g_vk_logged) {
        g_vk_logged = true;
        const VkProbe &v = g_vk;
        if (v.error[0]) ev("vulkan probe: %s", v.error);
        else
            ev("vulkan probe: colorspace ext %s, hdr_metadata %s, HDR10 %s, scRGB %s", v.ext_colorspace ? "yes" : "no",
               v.dev_hdr_meta ? "yes" : "no", v.hdr10 ? "offered" : "not offered", v.scrgb ? "offered" : "not offered");
        write_report();
    }
}

void aio_hdr_draw_ui(ImDrawList *dl, ImVec2 o, float w, float h, float fps, bool fullscreen,
                     const AioHdrFonts *fonts) {
    g_nquad = 0;
    if (!S.active || !dl || !fonts || !fonts->ui || !fonts->mono) return;
    F = *fonts;

    // Keyboard (optional; everything is also a button): H next mode, V values, R re-check.
    if (ImGui::IsKeyPressed(ImGuiKey_H, false)) request_mode(next_mode());
    if (ImGui::IsKeyPressed(ImGuiKey_V, false)) S.show_values = !S.show_values;
    if (ImGui::IsKeyPressed(ImGuiKey_R, false)) S.recheck = true;

    // Scale everything with the viewport so the card reads the same on a phone.
    float s = w / 960.0f;
    if (h / 640.0f < s) s = h / 640.0f;
    if (s < 0.75f) s = 0.75f;
    if (s > 2.6f) s = 2.6f;
    const float M = 14.0f * s;
    const float top = draw_status_band(dl, o, w, s, fps, fullscreen);
    if (S.show_values) draw_values(dl, o.x + M, top, o.x + w - M, o.y + h - M, s);
    else draw_patterns(dl, o.x + M, top, o.x + w - M, o.y + h - M, s);
}

void aio_hdr_render(const AioHdrHost *h, ImDrawData *draw_data) {
    if (!S.active || !h || !draw_data) return;
    ID3D11DeviceContext *ctx = h->ctx;
    ID3D11RenderTargetView *bb = *h->rtv;
    if (!bb) return;
    UINT bw = 0, bhh = 0;
    bb_size(*h->swap, &bw, &bhh);
    if (bw == 0 || bhh == 0) return;

    const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    ctx->OMSetRenderTargets(1, &bb, nullptr);
    ctx->ClearRenderTargetView(bb, black);
    D3D11_VIEWPORT vp;
    ZeroMemory(&vp, sizeof(vp));
    vp.Width = (float)bw;
    vp.Height = (float)bhh;
    vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &vp);

    // 1) The card, straight into the back buffer in the swapchain's encoding.
    if (S.pipe_ok && g_nquad > 0) {
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ctx->Map(S.vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            CardVtx *v = (CardVtx *)m.pData;
            int nv = 0;
            const float mode = (float)S.mode;
            for (int i = 0; i < g_nquad; ++i) {
                const CardQuad &q = g_quad[i];
                const float x0 = q.x0 / bw * 2.0f - 1.0f, x1 = q.x1 / bw * 2.0f - 1.0f;
                const float y0 = 1.0f - q.y0 / bhh * 2.0f, y1 = 1.0f - q.y1 / bhh * 2.0f;
                const float corner[4][4] = {{x0, y0, 0.0f, 0.0f}, {x1, y0, 1.0f, 0.0f},
                                            {x0, y1, 0.0f, 1.0f}, {x1, y1, 1.0f, 1.0f}};
                static const int kIdx[6] = {0, 1, 2, 2, 1, 3};
                for (int k = 0; k < 6; ++k) {
                    CardVtx &cv = v[nv++];
                    const float *c = corner[kIdx[k]];
                    cv.x = c[0];
                    cv.y = c[1];
                    cv.u = c[2];
                    cv.v = c[3];
                    memcpy(cv.p0, q.p0, sizeof(cv.p0));
                    cv.p1[0] = q.p1[0];
                    cv.p1[1] = q.p1[1];
                    cv.p1[2] = q.p1[2];
                    cv.p1[3] = mode;
                }
            }
            ctx->Unmap(S.vb, 0);
            UINT stride = sizeof(CardVtx), off = 0;
            ctx->IASetInputLayout(S.il);
            ctx->IASetVertexBuffers(0, 1, &S.vb, &stride, &off);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx->VSSetShader(S.vs, nullptr, 0);
            ctx->HSSetShader(nullptr, nullptr, 0);
            ctx->DSSetShader(nullptr, nullptr, 0);
            ctx->GSSetShader(nullptr, nullptr, 0);
            ctx->PSSetShader(S.ps, nullptr, 0);
            ctx->RSSetState(S.rs);
            ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
            ctx->OMSetDepthStencilState(nullptr, 0);
            ctx->Draw((UINT)nv, 0);
        }
    }

    // 2) ImGui. SDR: straight into the back buffer. HDR10 / scRGB: into the RGBA8
    //    layer, then composited at 203 nits in the swapchain's encoding.
    const bool layered = S.mode != MODE_SDR && S.pipe_ok && ensure_ui_layer(h->dev, bw, bhh);
    if (!layered) {
        ImGui_ImplDX11_RenderDrawData(draw_data);
    } else {
        const float clear0[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        ctx->OMSetRenderTargets(1, &S.ui_rtv, nullptr);
        ctx->ClearRenderTargetView(S.ui_rtv, clear0);
        ImGui_ImplDX11_RenderDrawData(draw_data);
        ctx->OMSetRenderTargets(1, &bb, nullptr);
        ctx->RSSetViewports(1, &vp);
        const float comp[4] = {S.mode == MODE_HDR10 ? 0.0f : 1.0f, kSdrWhite, 0.0f, 0.0f};
        ctx->UpdateSubresource(S.cb_comp, 0, nullptr, comp, 0, 0);
        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(S.vs_full, nullptr, 0);
        ctx->HSSetShader(nullptr, nullptr, 0);
        ctx->DSSetShader(nullptr, nullptr, 0);
        ctx->GSSetShader(nullptr, nullptr, 0);
        ctx->PSSetShader(S.ps_comp, nullptr, 0);
        ctx->PSSetConstantBuffers(0, 1, &S.cb_comp);
        ctx->PSSetShaderResources(0, 1, &S.ui_srv);
        ctx->RSSetState(S.rs);
        const float bf[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        ctx->OMSetBlendState(S.bs_premul, bf, 0xffffffff);
        ctx->OMSetDepthStencilState(nullptr, 0);
        ctx->Draw(3, 0);
    }

    // Leave nothing of ours bound: the shell's resize path and the DX11 scenes
    // expect a clean slate.
    ID3D11ShaderResourceView *nullsrv = nullptr;
    ctx->PSSetShaderResources(0, 1, &nullsrv);
    ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
}

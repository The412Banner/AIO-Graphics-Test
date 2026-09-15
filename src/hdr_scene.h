// AIO Graphics Test - HDR test card (Direct3D 11 / DXGI HDR10).
//
// Unlike the kScenes[] family, which render offscreen and reach the screen as an
// ImGui image through the shell's 8-bit swapchain, this test has to own the
// window's swapchain: HDR10 output is a property of the swapchain (a flip-model
// R10G10B10A2 swapchain in the PQ BT.2020 colour space), not of a texture. While the
// test is selected it releases the shell's bitblt R8G8B8A8 swapchain, creates its
// own flip-model one on the same window, and puts an identical shell swapchain back
// when the test is left. Everything else about the shell (device, ImGui, menu) is
// unchanged.
//
// Frame composition while active:
//   1. the test card (luminance patches, PQ ramp, 10-bit vs 8-bit gradients, moving
//      sun, BT.2020 vs BT.709 colours) is drawn straight into the back buffer in the
//      swapchain's own encoding (PQ, scRGB or sRGB);
//   2. ImGui (chrome + the card's labels and readout) is rendered into an RGBA8 layer
//      and composited on top at SDR reference white (203 nits), so the UI stays at a
//      normal brightness in HDR instead of being read as PQ code values.
//   In the SDR mode ImGui renders straight into the back buffer as usual.
//
// Failure never ends the test: if a flip-model swapchain or HDR10 is unavailable
// the card falls back to SDR and says why on screen.
#ifndef AIO_HDR_SCENE_H
#define AIO_HDR_SCENE_H

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include "imgui.h"

// What the shell lends the test: its device and window, plus the slots holding its
// swapchain and back-buffer RTV. The test puts its own swapchain into those slots
// while active and an identical shell swapchain back on leave, so the shell's
// resize and Present code keep working through the same pointers.
struct AioHdrHost {
    ID3D11Device *dev;
    ID3D11DeviceContext *ctx;
    HWND hwnd;
    IDXGISwapChain **swap;
    ID3D11RenderTargetView **rtv;
};

// Fonts the card draws with. The big ones are baked large so the readout stays
// sharp at phone scale.
struct AioHdrFonts {
    ImFont *ui;        // Inter, baked 14
    ImFont *mono;      // Cascadia Mono, baked 12
    ImFont *big_ui;    // Inter, baked large
    ImFont *big_mono;  // Cascadia Mono, baked large
};

// True between aio_hdr_enter and aio_hdr_leave.
bool aio_hdr_is_active(void);

// Swap the HDR swapchain in (creates it, probes colour-space support, reads the
// DXGI output description, picks HDR10 if supported else SDR, writes the report).
void aio_hdr_enter(const AioHdrHost *h);

// Restore the shell's own bitblt R8G8B8A8 swapchain and release everything.
void aio_hdr_leave(const AioHdrHost *h);

// Once per frame BEFORE ImGui::NewFrame: frame pacing, then any mode switch or
// re-check requested from the UI on the previous frame.
void aio_hdr_begin_frame(const AioHdrHost *h);

// Inside the shell's ImGui frame: lays out the card in the viewport rect (o, w, h),
// draws its labels, status band, buttons and readout panel, and records the card
// geometry for aio_hdr_render. fps = the shell's presented-frame counter.
void aio_hdr_draw_ui(ImDrawList *dl, ImVec2 o, float w, float h, float fps, bool fullscreen,
                     const AioHdrFonts *fonts);

// After ImGui::Render, instead of the shell's normal clear + ImGui draw: draws the
// card into the back buffer, then the ImGui frame (composited for HDR). The shell
// Presents afterwards with vsync.
void aio_hdr_render(const AioHdrHost *h, ImDrawData *draw_data);

#endif  // AIO_HDR_SCENE_H

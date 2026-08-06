// AIO Graphics Test - cross-API embedded backend contract.
//
// The single-window Dear ImGui shell (shell_imgui.cpp) runs on its OWN D3D11
// device and composites every test INSIDE its viewport. The DX11 scene family is
// driven directly on the shell's device (see cube_d3d11_scene.h). The other
// backends (Vulkan / OpenGL / D3D12 / D3D10 / D3D9 / D3D8 / DirectDraw) each own a
// DIFFERENT API/device, so they cannot render onto the shell's device. Instead
// each one renders ONE frame to an OFFSCREEN target on its own device (no window,
// no swapchain shown), then hands the shell the resulting pixels via a CPU
// readback (GPU -> staging -> CPU). The shell uploads those pixels into a dynamic
// D3D11 texture and draws it with ImGui::Image, exactly like the DX11 path - so
// the HUD, fullscreen and aspect-correct resize all work identically for every API.
//
// This readback baseline ALWAYS works regardless of interop support (it never
// needs a shared DXGI handle / external memory), so it is the reliability floor:
// something renders embedded for every API even where a zero-copy optimization is
// unavailable. A backend MAY later expose a shared-texture fast path, but must
// keep this readback contract as the fallback.
//
// The standalone aio_run_*_cube runners (the --cube CLI + the headless --bench
// child processes) are UNTOUCHED; these are additive entry points.
//
// Reliability rules every backend embed MUST follow:
//   - Load its API DLL(s) DYNAMICALLY (LoadLibrary/GetProcAddress); never a static
//     import (the exe must still launch on a container missing that DLL set).
//   - NEVER pop a MessageBox / modal on failure. Return non-zero from init and let
//     the shell show an inline "unavailable" notice.
//   - If a window handle is unavoidable (GL/D3D9/D3D8/DDraw need one to create a
//     device), create it HIDDEN (no WS_VISIBLE) and never ShowWindow it.
//   - Never SetWindowText a live FPS string (it bleeds through the shell chrome).

#ifndef AIO_CUBE_EMBED_H
#define AIO_CUBE_EMBED_H

#ifdef __cplusplus
extern "C" {
#endif

// Byte order of the CPU readback rows. The shell's upload texture is
// DXGI_FORMAT_R8G8B8A8_UNORM (R at byte 0), so RGBA8 uploads with a straight
// memcpy and BGRA8 needs an R/B swap per pixel.
enum { AIO_EMBED_FMT_RGBA8 = 0, AIO_EMBED_FMT_BGRA8 = 1 };

typedef struct AioEmbedFrame {
    const void *pixels;  // CPU pointer to the last rendered frame; owned by the
                         // backend, valid until the next render/get_frame/cleanup.
    int width;           // pixel dimensions of the readback
    int height;
    int pitch;           // bytes per row (may exceed width*4 - honor it)
    int fmt;             // AIO_EMBED_FMT_RGBA8 / _BGRA8
    int flip_y;          // 1 = rows are bottom-up (GL); the shell flips on upload
} AioEmbedFrame;

// Uniform per-backend embed interface (mirrors cube_d3d11_scene.h's shape). All
// entry points return 0 on success unless noted. init/resize (re)create the device
// and offscreen target at (w,h) PIXELS; render issues one frame for animation time
// t (seconds); get_frame publishes the CPU pixels; gpu_ms returns that backend's
// own last per-frame render cost in milliseconds (0 if unknown); cleanup releases
// everything. All are no-ops / return non-zero if the backend never init'd.
typedef struct AioEmbedBackend {
    int (*init)(int w, int h);
    int (*resize)(int w, int h);
    void (*render)(double t);
    int (*get_frame)(AioEmbedFrame *out);
    double (*gpu_ms)(void);
    void (*cleanup)(void);
} AioEmbedBackend;

// ---- Vulkan (vulkan-1 -> Turnip) ----
int aio_vk_embed_init(int w, int h);
int aio_vk_embed_resize(int w, int h);
void aio_vk_embed_render(double t);
int aio_vk_embed_get_frame(AioEmbedFrame *out);
double aio_vk_embed_gpu_ms(void);
void aio_vk_embed_cleanup(void);

// ---- OpenGL (opengl32 -> Zink -> Vulkan) ----
int aio_gl_embed_init(int w, int h);
int aio_gl_embed_resize(int w, int h);
void aio_gl_embed_render(double t);
int aio_gl_embed_get_frame(AioEmbedFrame *out);
double aio_gl_embed_gpu_ms(void);
void aio_gl_embed_cleanup(void);

// ---- Direct3D 12 (d3d12 -> VKD3D) ----
int aio_dx12_embed_init(int w, int h);
int aio_dx12_embed_resize(int w, int h);
void aio_dx12_embed_render(double t);
int aio_dx12_embed_get_frame(AioEmbedFrame *out);
double aio_dx12_embed_gpu_ms(void);
void aio_dx12_embed_cleanup(void);

// ---- Direct3D 10 (d3d10 -> DXVK) ----
int aio_dx10_embed_init(int w, int h);
int aio_dx10_embed_resize(int w, int h);
void aio_dx10_embed_render(double t);
int aio_dx10_embed_get_frame(AioEmbedFrame *out);
double aio_dx10_embed_gpu_ms(void);
void aio_dx10_embed_cleanup(void);

// ---- Direct3D 9 (d3d9 -> DXVK) ----
int aio_dx9_embed_init(int w, int h);
int aio_dx9_embed_resize(int w, int h);
void aio_dx9_embed_render(double t);
int aio_dx9_embed_get_frame(AioEmbedFrame *out);
double aio_dx9_embed_gpu_ms(void);
void aio_dx9_embed_cleanup(void);

// ---- Direct3D 8 (d3d8 -> d3d9 -> DXVK) ----
int aio_dx8_embed_init(int w, int h);
int aio_dx8_embed_resize(int w, int h);
void aio_dx8_embed_render(double t);
int aio_dx8_embed_get_frame(AioEmbedFrame *out);
double aio_dx8_embed_gpu_ms(void);
void aio_dx8_embed_cleanup(void);

// ---- DirectDraw / Direct3D 7 (ddraw -> wined3d) ----
int aio_ddraw_embed_init(int w, int h);
int aio_ddraw_embed_resize(int w, int h);
void aio_ddraw_embed_render(double t);
int aio_ddraw_embed_get_frame(AioEmbedFrame *out);
double aio_ddraw_embed_gpu_ms(void);
void aio_ddraw_embed_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif  // AIO_CUBE_EMBED_H

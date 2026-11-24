// AIO Graphics Test - Direct3D 11 cube backend.
// Renders a spinning cube through Direct3D 11 (tests the DXVK -> Vulkan ->
// Turnip path under Winlator). Shares the HUD overlay and benchmark modules.
#ifndef AIO_CUBE_D3D11_H
#define AIO_CUBE_D3D11_H

#include <windows.h>

// Opens a Direct3D 11 scene window and runs it (honoring an active --bench).
// scene_name selects the test ("spin", "textured", "instanced", ...); NULL or an
// unknown name falls back to the spinning cube. Returns a process exit code.
int aio_run_d3d11_cube(HINSTANCE hinst, const char *scene_name);

// Sets the draw count for the "drawstress" scene (default 1024). Call before
// aio_run_d3d11_cube. Clamped to a sane range.
void aio_d3d11_set_draws(int n);

// Window for aio_run_d3d11_cube: client size in pixels (0 = default; with both 0
// the window is the original 640x480 frame), or fullscreen = a borderless popup
// over the primary monitor (overrides the size). Call before aio_run_d3d11_cube.
void aio_d3d11_set_window(int client_w, int client_h, int fullscreen);

// 0 = no FPS overlay bar (the window title still shows the FPS).
void aio_d3d11_set_hud(int on);

// Start the "upscale" scene with its pan/zoom on (M toggles it in the scene).
void aio_d3d11_set_motion(int on);

#endif  // AIO_CUBE_D3D11_H

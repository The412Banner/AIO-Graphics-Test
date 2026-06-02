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

#endif  // AIO_CUBE_D3D11_H

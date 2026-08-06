// AIO Graphics Test - Direct3D 11 scene registry accessors.
//
// These expose the file-static kScenes[] table from cube_d3d11.c so an external
// caller (the Dear ImGui single-window shell, shell_imgui.cpp) can drive the
// DX11 scenes against ITS OWN ID3D11Device / ID3D11DeviceContext, with no window,
// swapchain, or message loop of its own. The standalone aio_run_d3d11_cube path
// is untouched.
//
// Contract mirrors the standalone runner:
//   - init: builds the scene's resources (returns 0 on success). The accessor
//     lazily loads d3dcompiler (D3DCompile) the first time, so the caller does
//     not need to.
//   - frame: issues the scene's draws for one frame. The CALLER must have bound
//     an RTV + DSV, set the viewport, set a LESS depth-stencil state, and set a
//     CULL_NONE rasterizer baseline (scenes that need other state set it inside
//     frame), then cleared both targets.
//   - cleanup: releases the scene's resources.
//
// IMPORTANT: include this AFTER <d3d11.h> (uses ID3D11Device / ID3D11DeviceContext).
#ifndef AIO_CUBE_D3D11_SCENE_H
#define AIO_CUBE_D3D11_SCENE_H

#ifdef __cplusplus
extern "C" {
#endif

// Number of scenes in kScenes[].
int aio_d3d11_scene_count(void);
// Scene selector name ("spin", ...) / HUD label ("D3D11 Cube", ...). "" if OOB.
const char *aio_d3d11_scene_name(int i);
const char *aio_d3d11_scene_label(int i);
// Update the width/height scenes read via the internal g_w/g_h (the scaling-test
// scenes bake these into a resolution constant buffer). Call on viewport resize.
void aio_d3d11_scene_set_size(int w, int h);
// Lifecycle by index. init returns 0 on success; frame/cleanup are no-ops if OOB.
int aio_d3d11_scene_init(int i, ID3D11Device *dev, ID3D11DeviceContext *ctx, int w, int h);
void aio_d3d11_scene_frame(int i, ID3D11DeviceContext *ctx, double t, float aspect);
void aio_d3d11_scene_cleanup(int i);

// Draw count for the "drawstress" scene. Call BEFORE aio_d3d11_scene_init.
// (Same setter the standalone --draws path uses.)
void aio_d3d11_set_draws(int n);

#ifdef __cplusplus
}
#endif

#endif  // AIO_CUBE_D3D11_SCENE_H

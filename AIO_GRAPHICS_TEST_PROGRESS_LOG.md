
## 2026-06-28 — DX11 SCALING TESTS scene set (compositor upscaler torture cards)
Why: A/B the Bannerlator compositor's scaling modes (None/Linear/Nearest/SGSR/FSR/
FSR-Fit/Sharpen/NIS). This app does NOT scale — it only renders crisp, native,
high-freq, pixel-exact content at the swapchain (= container render-) resolution;
the user sets a sub-native container render-res and the compositor upscales to the
panel, then flips modes in the in-game drawer. High-freq 1px detail is what makes
the upscalers diverge (a smooth scene looks identical under every mode).
- New unified DX11 scene (cube_d3d11.c, ~after band_cleanup): one PS switches on
  iCard 0..5 — 0 combo (zone TL / grid TR / checker BL / wedge BR), 1 zone plate,
  2 resolution wedge + ~64-spoke siemens star + top freq-sweep bar, 3 1px lines on
  a 4px pitch + diagonals at 4 slopes, 4 checkerboard 1/2/4px zones, 5 4x4 hard-edge
  shapes (circle/square/triangle/diamond) at many rotations for overshoot/halo test.
- Cloned the Banding structure: custom ScaleCB cbuffer {iTime,iAspect,res.xy,iCard,
  iColor,iAspectMode,pad} (32B), fullscreen-triangle VS, SV_POSITION int2 pixel math.
  Shared scale_init_card/scale_frame/scale_cleanup + 6 thin scz_*_init wrappers so
  each menu entry opens on its card. No AA / no MSAA / no smoothstep; static (iTime
  plumbed, unused). Zone-plate k = 1.5708 / center-to-corner dist -> rings ~Nyquist
  at the edges.
- Keys (edge-detected, g_keys): C cycle card (wrap 0..5), K toggle color (grayscale
  vs v*float3(1,0.15,0.5)), A toggle 4:3 letterbox (exercises FSR vs FSR-Fit). HUD:
  "Card N  color:on/off  aspect:fill/4:3  [C/K/A]".
- Registered 6 entries in kScenes[] after "banding": scaletest_combo / _zoneplate /
  _wedge / _grid / _checker / _edges. CLI e.g. dx11 --scene scaletest_combo.
- menu.c: new "Scaling Tests >" sub-page (show_dx11_scaling) mirroring "Demo Scenes",
  reached via a 2nd persistent button (g_run_all2, ID_DX11_SCALING) on the DX11
  picker; "< Back" = ID_DX11_SCALING_BACK; both wired in WM_COMMAND + destroy_content.
- Branch feat/dx11-scaling-tests, pushed. CI workflow_dispatch run 28327163981 GREEN
  both arches (64+32). C compiles clean; HLSL compiles at runtime (device-untested).
- NEXT: stage .exe on device, set sub-native container render-res, launch a card,
  flip compositor Scaling mode and compare. Verify Nyquist tuning + ps_4_0 int/%/^.

## 2026-06-28 — DX11 BANDING TEST scene (debanding test fixture)
Why: prior compositor debanding test stalled — existing AIO scenes render gradients
too cleanly (no banding in the "before" to fix). Added a deliberate banding torture card.
- New DX11 scene "banding" (cube_d3d11.c): full-screen PS, 6 gradient strips
  (full gray / dark ramp 0..16/255 worst-case / R / G / B / dark hue sweep). 8-bit
  R8G8B8A8_UNORM swapchain quantises the smooth float gradient -> visible bands.
- Toggles (g_keys in band_frame, edge-detected): D = in-app sub-LSB dither ON/OFF
  (OFF=raw "before", ON=dither "after"); M = cycle pattern IGN / white-noise / Bayer8.
- New global g_scene_hud[64]; runner appends it to the FPS HUD -> "Dither OFF (IGN) [D/M]".
- Static (no animation) for clean frozen-frame A/B captures.
- menu.c: "Banding test (dither)" button in the D3D11 feature picker (dx11 --scene banding).
- Branch feat/dx11-banding-test (881f39e), pushed. CI workflow_dispatch run 28326348018
  GREEN both arches (64+32). C compiles clean; HLSL compiles at runtime (device-untested).
- NEXT: stage .exe on device, launch scene, A/B with compositor debander OFF vs ON.

## 2026-06-28 — RELEASE v1.6.1 (banding + scaling test scenes)
- Merged feat/dx11-scaling-tests -> main (ff, brought banding + scaling + fullscreen fix).
- Bumped AIO_VERSION "v1.6.0" -> "v1.6.1" (src/menu.h:9); shown bottom-right of the shell
  via the dynamically-sized g_version label (layout_footer measures AIO_VERSION each layout,
  so the footer reads v1.6.1 with no clipping). main HEAD = 928e83a.
- CI run 28327668514 GREEN (built 928e83a, both arches).
- GitHub Release /releases/tag/v1.6.1 — tagged at 928e83a, Latest, NOT prerelease/draft.
  Assets = CI exes (64bit 2372292 B + 32bit 2344186 B) from run 28327668514.
- NOTE: the new DX11 scenes (banding + 6 scaling cards) are CI-green but the HLSL compiles
  at runtime on-device and is NOT yet device-verified. Scenes degrade gracefully (a shader
  compile failure shows a fail-box, no app crash). Device A/B test still owed.

## 2026-06-28 — FIX: DX10/DX11 cube scenes rendered inside-out (winding vs cull)
Symptom (user): the plain spinning cube + cube-grid render the INSIDE of the cube
(hollow / back walls) instead of a solid outside cube. GL and Vulkan cubes look fine.
Root cause: the cube mesh is wound CCW-front (OpenGL convention; cube_d3d11.c kFace +
kQuadIdx). The GL cube enables GL_DEPTH_TEST but NEVER GL_CULL_FACE (cube_gl.c:63) so it
draws both sides and the depth buffer sorts them -> always solid (Vulkan cube likewise).
The DX11 spin_frame + inst_frame (and the DX10 cube) never call RSSetState, so they fell
to D3D's default rasterizer = CULL_BACK + FrontCounterClockwise=FALSE (CW=front). With
CCW geometry that culls the OUTWARD faces -> only inner back walls draw -> inside-out.
Other DX11 scenes dodged it because they each force CULL_NONE (dolphin/ray/atom/planet/
banding/etc). Swept the whole DX family: d3d8/d3d9/d3d12/ddraw already set CULL_NONE;
only D3D10 + D3D11 had the gap.
Fix (match GL/Vulkan = no culling, depth decides): bind a global CULL_NONE rasterizer
once before the render loop.
- src/cube_d3d11.c: rs_default (D3D11_CULL_NONE, DepthClipEnable) after OMSetRenderTargets;
  released in cleanup.
- src/cube_d3d10.c: same (D3D10_CULL_NONE) after OMSetRenderTargets; released in cleanup.
- Pushed to main (65a98ff). CI build-windows run 28344205408 GREEN both arches
  (x86_64 49s + i686 59s). C clean. ✅DEVICE-PROVEN 2026-06-29: user staged the run
  exes to /sdcard/Download, ran them, DX cubes render SOLID now (no longer inside-out).
- DONE. Optional follow-up: re-bundle rebuilt exes into Bannerlator
  container_pattern_common.tzst like v1.6.1 (existing containers need reinstall/root push).

## 2026-08-05 — Phase 0 — ImGui/DX11 scaffold (single-window redesign de-risk)
Why: the redesign targets a single-window Dear ImGui / DX11 shell, but mingw C++
codegen has previously hit illegal-instruction (c000001d) crashes under the
FEX/arm64ec Proton-9 container. This ADDITIVE, REVERSIBLE scaffold proves the
C++ / ImGui / DX11 path builds green and (for the user to test) launches there,
before we invest in the real UI. Nothing existing changes.
- New --imgui CLI flag → aio_run_imgui_shell(hInstance), dispatched in WinMain
  BEFORE the run_cube/aio_run_shell decision (cube.c). No-args launch still runs
  the OLD shell; every other path is byte-for-byte unchanged.
- src/shell_imgui.h: tiny header, `extern "C"` under C++ so cube.c (C) can call in.
- src/shell_imgui.cpp (one file): ImGui-on-DX11 proof window sketching the target
  look — top toolbar with a List/Grid segmented control (stub), a RIGHT-docked test
  list (Vulkan/OpenGL/DX12/DX11/…) as selectable rows, a LEFT viewport clear-color
  fill with an overlay reading "Direct3D 11 — NNNN FPS" + an ImGui::PlotLines
  frametime sparkline fed by REAL QPC-measured frame intervals. ESC quits.
- RENDERER SAFETY (repo lesson: never static-link the DXVK DLL set or the exe won't
  launch on a container without DXVK): the D3D11 device is created via a DYNAMIC
  d3d11.dll load (GetProcAddress("D3D11CreateDeviceAndSwapChain")); if it fails the
  exe shows a message box and exits cleanly (no crash). d3d11/dxgi are NOT linked.
  ImGui's DX11 backend needs D3DCompile — satisfied by a lazy-loading forwarding
  SHIM in shell_imgui.cpp that LoadLibrary()s d3dcompiler_47.dll at first use, so
  d3dcompiler stays dynamic too (NO -ld3dcompiler needed — the shim resolved the
  symbol cleanly; verified: zero undefined-D3DCompile at link).
- Build wiring (.github/workflows/build-windows.yml, raw g++, NO CMake, matches the
  Vulkan-Headers clone pattern): clone Dear ImGui pinned to v1.91.5; compile
  imgui{,_draw,_tables,_widgets} + backends/imgui_impl_{dx11,win32} + shell_imgui.cpp
  with `-O2 -I imgui -I imgui/backends -I src -DWIN32_LEAN_AND_MEAN
  -D_WIN32_WINNT=0x0A00 -fcf-protection=none` (the -fcf-protection=none is deliberate
  FEX-arm64ec insurance). Link with g++ (not gcc) so libstdc++ resolves, static
  (`-static-libstdc++ -static-libgcc`) so there is NO runtime C++ DLL dep.
- Extra link libs actually needed (reported): **-limm32** and **-ldwmapi** (both
  pulled in by imgui_impl_win32; both are CORE Wine DLLs, always present, so no
  container-launch risk). Did NOT need -ld3d11 / -ldxgi / -ld3dcompiler.
- 32-bit NOT gated: the C++/ImGui TUs compile and link on BOTH i686 and x86_64
  (i686 failed identically to x86_64 only on the missing dwmapi, fixed for both).
- CI build-windows run 31055718702 GREEN both legs (build-i686 + build-x86_64) on
  branch feat/imgui-shell @ 80e154c (headSha verified == pushed SHA). Artifacts
  present: 64bit 1173275 B, 32bit 1189029 B. Iterations to green: add <cstdio> for
  snprintf, then -ldwmapi.
- STAGED (CI-green, NOT device-proven): 64-bit exe →
  /sdcard/Download/AIO-Graphics-Test-imgui-scaffold-64bit.exe,
  sha256 309c3f621319cc7f764adf8746d731c201f4ada40af7ce5d0bc0f2b539c76dfc
  (on-device sha256 verified equal). USER launches with `--imgui` and reports
  whether the ImGui window renders (proves no c000001d) or crashes.
- NOT merged to main. Branch feat/imgui-shell only.
- Phase 1 notes: if the window launches clean, build out the real single-window
  layout here (docking, real per-test render target in the left viewport, wire the
  List/Grid control, feed the HUD from live frame stats). The D3DCompile shim +
  dynamic-d3d11 pattern is the template for keeping the redesign DXVK-optional.
  If it crashes, the culprit is C++ codegen under FEX — next lever is narrowing
  compiler flags (e.g. -mno-avx / -fno-exceptions) or an even more minimal TU.

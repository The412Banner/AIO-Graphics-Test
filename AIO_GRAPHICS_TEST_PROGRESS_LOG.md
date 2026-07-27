
## 2026-07-27 — Bionic-FG validation harness (new standalone exe + analyzer)
Why: prove the Bionic-FG Vulkan layer isn't just running but is EFFECTIVE — extra
frames really on-screen, genuinely interpolated (not duplicated), spatially correct,
and smoothly paced. Fresh, ground-up (NOT derived from the WinNative "Frame-Gen-Tester"
binary we inspected — clean-room, ours).
- New standalone `src/fgtest.c` -> `AIO-FGTest-{32,64}bit.exe`. Self-contained; no
  dep on the main suite. D3D11, dynamically loaded (runs under Wine+DXVK), COBJMACROS,
  runtime HLSL — same idioms as cube_d3d11.c so it builds on the existing mingw path.
- It is a *source*, not a measurer: Bionic-FG sits BELOW the app in the swapchain, so
  the app can't see generated frames. It renders REAL frames at a precise known cap and
  bakes signals that are a pure function of the integer real-frame index `fi`:
  FLICKER block (full invert/frame -> ~50% grey = a generated frame), BARCODE (start +
  20-bit fi + parity + stop -> real/gen/duplicate), PHASE hue swatch, constant-VELOCITY
  marker (interp accuracy = marker vs true midpoint), plus ROTOR/BARS/OCCLUDER stressors.
  Scenes: verify/motion/stress/full. Writes `fgtest_results.json` (cadence stats + exact
  geometry so the analyzer can reconstruct expected positions). Args: --cap --duration
  --scene --width --height --vsync --selftest --out. Keys: [ ] cap, V, Space, 1-4, Esc.
- New host analyzer `tools/fg_analyze.py` (opencv+numpy): decodes a high-speed capture
  vs the results json -> generation ratio (~mult), real/gen counts, duplicate rate,
  interpolation px RMSE (vs half-step), throughput/authenticity PASS/FAIL -> fg_analysis.json.
- New workflow `.github/workflows/build-fgtest.yml` (32+64-bit mingw; links
  user32/gdi32/dxguid/winmm; no import libs — d3d11/d3dcompiler are LoadLibrary'd).
- Methodology in `docs/FRAMEGEN_TEST.md` (T1 throughput .. T6 cross-vendor, device
  recipe, Nyquist/latency caveats). Branch: feat/bionic-fg-tester. NOT device-run yet.

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

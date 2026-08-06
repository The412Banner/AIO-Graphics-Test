
## 2026-08-05 — Phase 2 — live DX11 render IN the ImGui viewport (+ fullscreen, resize)
Why: Phase 1 gave 1:1 chrome with a placeholder viewport. Phase 2 makes the LEFT
viewport render the DX11 family LIVE, in-place ("exactly like the preview"): the
selected scene draws to an offscreen texture that composites into the viewport rect,
HUD/telemetry overlaying on top.
- NEW src/cube_d3d11_scene.h + accessors in cube_d3d11.c (after pick_scene): expose
  the file-static kScenes[] to the shell — aio_d3d11_scene_count/name/label,
  _set_size, _init/_frame/_cleanup by index. init lazily loads d3dcompiler (mirrors
  the runner) and sets g_w/g_h; the standalone aio_run_d3d11_cube path is UNCHANGED.
  The shell drives kScenes[] against ITS OWN device — no window/swapchain/loop in
  cube_d3d11.c. All scene custom rasterizer state is set per-frame inside *_frame
  (verified: every RSSetState is in a _frame, none in _init), so the shell resets the
  runner baseline (LESS depth + CULL_NONE) each frame before frame() → clean switching.
- shell_imgui.cpp: offscreen RGBA8 color+SRV + D24S8 depth sized to the live viewport
  rect (ensure_offscreen recreates ONLY on pixel-size change). Per frame for a DX11
  selection: bind offscreen RTV+DSV, set viewport, LESS depth, CULL_NONE, clear to the
  mockup screen bg, call the scene's frame(ctx,t,aspect), unbind RTV (no RTV/SRV
  hazard), then ImDrawList::AddImage(srv) into the viewport; HUD pill + frametime
  sparkline + telemetry draw over it via draw-list as before. HUD FPS = live present
  rate; Resolution telemetry = live native offscreen px.
- Selection→scene map (name-based, no struct churn): DX11 Scenes(14), Showcase
  Demos(14), Scaling Tests(7) all map to cube kScenes[] indices; Draw-Stress rows set
  the draw count via aio_d3d11_set_draws before init. RENDER LIVE: spin, textured,
  instanced, tess, compute, gsexplode, atomics, dolphin, banding, drawstress 128–2048,
  all 6 scaling cards + banding, and the auto-cruising demos (raymarch/ocean/ocean2/
  mandelbulb/nebula/nebula2/showcase/space/desert/city/cel/matcap). The "Direct3D 11"
  BACKEND row also embeds (spin).
- DEFERRED (interactive input): Free Look + Planet Fly render in their default idle
  camera — their WASD/mouse steering comes from d3d11_wndproc/g_free_hwnd which the
  embedded path doesn't drive (guarded on g_free_hwnd==NULL, no crash). Feeding ImGui
  viewport hover/keys into these is a Phase-3 follow-up; they do NOT block the rest.
- Non-DX11 backends (Vulkan/OpenGL/D3D12/D3D10/D3D9/D3D8/DirectDraw-DX7): can't share
  the DX11 device yet (Phase 3). Selecting one launches the standalone window via
  CreateProcessA(self,"--cube <api>") (deduped; reselect to relaunch); the row shows a
  subtle "opens window" glyph and the viewport shows a centered notice instead of HUD.
- Tools (GPU Info/Benchmark/Disk Speed): unchanged Phase-1 placeholder (Phase-3
  datapanes).
- FULLSCREEN toggle (touch-first, for Winlator): translucent HUD-styled expand pill
  pinned to the viewport top-right; tap → the live render fills the WHOLE window
  (titlebar/toolbar/menu/telemetry/footer hidden, HUD pill kept as corner overlay);
  the pill flips to an always-visible "Exit Fullscreen" restore pill; ALSO F11 and ESC
  toggle (edge-triggered via WM_KEYDOWN bit-30 so key-repeat can't rapid-flip; ESC in
  windowed still closes). ESC is NOT the only exit. Exit restores the exact windowed
  layout — same selection + menu scroll (ImGui retains child state) + HUD/telemetry.
- RESIZE (explicit requirement): WM_SIZE now only records a pending size; the main
  loop applies ONE swapchain ResizeBuffers per frame (coalesced → click-drag doesn't
  thrash). The offscreen color+depth recreate to the viewport's new PIXEL size (native
  res, no blur) and aspect = vp_w/vp_h is recomputed every frame → square faces stay
  square, no stretch. Fullscreen toggle is just another viewport-size change (whole
  window) and is handled by the same path; scene keeps running (NOT re-init'd on
  resize, matching the standalone runner). 0-w/0-h guarded (clamp ≥8px, W/Hh ≥1,
  vpW/bodyH ≥8) so minimize / degenerate drag can't crash or divide-by-zero.
- No workflow change (cube_d3d11_scene.h is a header; cube_d3d11.c + shell_imgui.cpp
  already compiled with -I src). Branch feat/imgui-shell (NOT merged to main).
- NEXT (Phase 3): cross-API embedding (D3D12/D3D10/D3D9/DX8/DDraw render-to-shared-
  texture or interop into the shell surface; GL via WGL_NV_DX_interop or a readback
  blit; Vulkan via a shared image), plus GPU Info / Benchmark / Disk Speed datapanes
  in the viewport; feed ImGui viewport input into Free Look / Planet Fly.

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

## 2026-08-05 — Phase 1 — ImGui shell chrome (1:1 mockup reproduction)
Why: Phase 0 proved the ImGui/DX11 window launches under FEX/arm64ec (no c000001d).
Phase 1 builds the real single-window chrome to match the approved mockup
(scratchpad/aio-redesign.html) — same dark-instrument feel, teal accent, per-API
color chips, panel rounding, grouped menu, viewport HUD + telemetry, footer. Chrome
only; no live per-test render in the viewport yet (that is Phase 2).
- src/shell_imgui.cpp rewritten (keeps ALL Phase-0 machinery: dynamic d3d11 load,
  lazy D3DCompile shim, imgui_impl_win32+dx11, real QPC frametime feed, --imgui flag).
  Removed the stray Phase-0 top-left "Direct3D 11 — NNNN FPS" overlay + raw toolbar
  text (the garbled/overlapping draws) — replaced by the full chrome.
- Palette: exact hex from the mockup CSS custom props, both DARK (default) + LIGHT,
  mapped into an ImGuiStyle setup (Window/Child rounding 10, FrameRounding 7, Grab 6,
  borders 1px in --line, scrollbar themed) in apply_theme(bool). The VIEWPORT/screen
  region uses the theme-independent dark --scr colors in BOTH themes (like the mockup).
- Per-API chip hues (theme-independent): vk #e5484d, gl #5b8def, dx12 #41bf6d,
  dx11 #22c1ad, dx10 #3fb9a0, dx9 #e0a13a, dx8/ddraw #94a1b0, demo #a678f0, tools=accent.
- Fonts EMBEDDED (no external files): Cascadia Mono (mono role) + Inter (UI role),
  base85-compressed TTF via imgui binary_to_compressed_c -> src/font_mono.inc +
  src/font_ui.inc, loaded at 5 sizes (Inter 14/19, Cascadia 10/12/20), oversample 3x2.
- Top app bar = titlebar (multi-API app glyph + "AIO Graphics Test" + AIO_VERSION +
  decorative win controls) over a toolbar (crumb "Group > Item" + List/Grid segmented
  control [default List, accent fill + #04231f ink on active] + Theme toggle).
- Right menu panel (width 320): header "Tests / N tests", grouped scroll (List rows OR
  2-col Grid tiles) with mono uppercase group headers + divider, 9px API chips, fps
  hints (mono, right-aligned), hover=panel2, selected=panelhi + 2px accent left bar +
  faux-bold name. Grid tiles: panel2 + 3px hue left bar, selected=accent border+glow.
- Left viewport: dark scr gradient fill + HUD (rgba pill: glowing API chip + mono API
  label + 20px accent-ink FPS from LIVE frame timing + FRAMETIME caps + custom
  draw-list sparkline [area-fill gradient + endpoint dot; NOT raw PlotLines, per the
  1:1 spec] + ms/avg/1% from the real ring buffer) and a bottom telemetry strip
  (Resolution / Present / Translation-path[accent-ink] / GPU, mono caps keys + values,
  1px dividers). Tools hide the HUD/telemetry and show a centered tool placeholder.
- Footer: "Built with [drawn heart #e5484d] for the Emulation Community" + mono version.
- Selecting any menu row updates HUD API/chip, telemetry present+path, and the crumb
  (static catalog data — backends do NOT launch yet).
- CATALOG sourced from real code (no invented/dropped entries): 8 backends (menu.c
  g_items + cube.c WinMain), DX11 Scenes (14, show_dx11_scenes feature set), Showcase
  Demos (14, show_dx11_demos), Scaling Tests (7, show_dx11_scaling), Tools (3).
- Branch feat/imgui-shell (NOT merged). No workflow change needed (fonts are #included
  .inc; shell_imgui.cpp already compiled with -I src on both arches).
- NEXT (Phase 2): render each test to an offscreen DX11 texture and blit/ImGui::Image
  it into the viewport rect (HUD/telemetry composite on top); wire real backend launch
  on selection; feed HUD FPS from the test's own present loop, not the shell's.

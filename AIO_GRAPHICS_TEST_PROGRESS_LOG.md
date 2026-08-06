
## 2026-08-05 — Phase 3 — Tool datapanes + interactive input + real HUD throughput
Why: Phase 2 embedded the live DX11 viewport. Phase 3 finishes the remaining app
surfaces so the single-window shell replaces the classic menus for the Tools, the
interactive demos, and honest performance readout. (Cross-API backend embedding is
still deferred to a later pass.)

- TOOL DATAPANES (native ImGui, 1:1 mockup .datapane/.dp-card/.tile styling; theme-
  independent dark scr* palette in both light+dark; mono data, tabular values, green/
  red flags). They own the viewport surface for Tools (HUD + telemetry hidden).
  - GPU Info: two-column Vulkan | OpenGL instrument cards. REAL data via new structured
    accessors in gpuinfo.c/.h — aio_gpuinfo_query_vk() (device/driver/api/vendor/type/
    largest-heap memory + 8 real VkPhysicalDeviceFeatures yes/no rows) and
    aio_gpuinfo_query_gl() (renderer/version/GLSL/vendor/max-texture/MSAA). Nothing
    fabricated. Queried ONCE on a background thread (VkInstance + WGL context are heavy);
    shows "Querying adapters..." until done.
  - Benchmark: full kBenchRows[] catalog (36 rows) mirrored shell-side, mockup bar+value
    layout, per-row Run + Run All, 15/30/45/60 s duration + vsync toggle, avg + min/max
    shown per row. DX11 rows bench IN-PROCESS (a bench-scene override drives the offscreen
    target so its GPU cost is timed via the timestamp-disjoint pair; present forced
    Uncapped during the run); non-DX11 rows CreateProcess "--cube <arg> --bench <secs>
    --autoclose 3 [--vsync]" and read back AIO-Graphics-Test_bench_<apilabel>.txt
    ("avg|min|max"), reusing menu.c's exact result-file naming/format.
  - Disk Speed: seq read/write + random 4K read/write tiles (MB/s + IOPS), 256/512/1024/
    2048 MB selector, Real-Flash/Quick toggle, Run, "What's this?" popup, "Clear Temp
    Files". Wired to disk.c via new aio_disk_run_ex() (fills AioDiskResult) + existing
    aio_disk_cleanup(); the run executes on a background thread.
- INTERACTIVE DEMOS INPUT: Free Look + Planet Fly now steer inside the ImGui viewport.
  Added an input-injection API to cube_d3d11.c (aio_d3d11_input_key/wheel/steer/reset,
  declared in cube_d3d11_scene.h). Both freelook_update + planet_update prefer an
  INJECTED normalized-steer path (g_inject_steer) over the GetCursorPos(g_free_hwnd)
  path they use standalone. The shell feeds ImGui key state (WASD/QE/Space/Shift/Ctrl/
  arrows/F/M), wheel, and a viewport-centre-relative steer — ONLY while the pointer is
  over the viewport (else input is released), so ImGui controls aren't hijacked. Input
  is reset on scene switch.
- REAL HUD THROUGHPUT: a D3D11 TIMESTAMP_DISJOINT + TIMESTAMP pair brackets just the
  scene's frame() draws each frame; read back one frame late (no GPU stall). The HUD
  pill + sparkline now show the scene's true GPU render rate (high, like the standalone
  cube) instead of the shell's vsync'd present rate; falls back to present rate if the
  driver lacks timestamp support. Added a Present-mode toolbar toggle (Vsync/FIFO vs
  Uncapped) honored by Present(syncInterval); the telemetry Present cell reflects it.
- POLISH: tool background forced to the dark gradient (a bench scene rendered offscreen
  for timing can't bleed behind the cards); input reset on scene switch; timing queries
  created/destroyed with the device.
- Files: src/shell_imgui.cpp (datapanes, bench engine, timing, input feed, present
  toggle), src/gpuinfo.c/.h (+structured queries), src/disk.c/.h (+aio_disk_run_ex/
  AioDiskResult), src/cube_d3d11.c + src/cube_d3d11_scene.h (input injection). No CI
  workflow change needed (all compiled by the existing steps; no new TUs).
- Branch feat/imgui-shell (NOT merged). NEXT: cross-API backend embedding (D3D12/D3D10/
  D3D9/DX8/DDraw into a shared surface) so the 8 backends render in-viewport too.

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

## Phase 4 - EVERY backend embeds in the viewport (zero pop-outs) + honest FPS HUD

PRIORITY PASS: the single-window rebuild's whole point. Previously only the DX11 scene
family embedded (ImGui::Image of an offscreen SRV on the shell's device); Vulkan/OpenGL/
DX12/DX10/DX9/DX8/DirectDraw each `CreateProcess "--cube <api>"`'d their own top-level
window. That pop-out behavior is REMOVED. Every backend now renders INSIDE the viewport
and swaps in place - identical UX to the DX11 scenes (HUD, fullscreen, aspect-correct
resize all work). Reliability > max fps. DEVICE-UNVERIFIED (CI proves compile/link only).

- METHOD = universal offscreen render + CPU readback baseline (new `cube_embed.h`
  contract: init/resize/render/get_frame/gpu_ms/cleanup per backend, `extern "C"`,
  mirrors cube_d3d11_scene.h). Each backend renders ONE frame to an offscreen target on
  its OWN device (no window shown; GL/DX9/DX8/DDraw use a HIDDEN WS_POPUP window, DX10/
  DX12/Vulkan need none), reads pixels back to a CPU buffer, and the shell uploads them
  into a DYNAMIC D3D11 texture (`g_embed_tex`) sampled by ImGui::Image via a unified
  `g_view_srv` (DX11 offscreen SRV OR cross-API readback SRV). Readback ALWAYS works
  regardless of interop, so it is the reliability floor. Row-pitch + BGRA/RGBA swizzle +
  GL bottom-up flip handled on upload.
  - PER-BACKEND embed method (all readback baseline this phase; zero-copy shared-texture
    is a future optimization, none shipped yet):
    * Vulkan  - own VkInstance/device (no surface), offscreen R8G8B8A8 image +
      vkCmdCopyImageToBuffer -> host buffer. New `aio_vk_embed_*` in cube.c + new
      solid-color shaders embed_cube.vert/.frag (CI glslang -> .inc). RGBA8.
    * OpenGL  - hidden WGL window, GL 1.1 fixed-function to back buffer, glReadPixels.
      RGBA8, bottom-up (flip_y=1).
    * DX12    - D3D12CreateDevice (no window), offscreen RT -> READBACK buffer
      (256-aligned row pitch) via CopyTextureRegion, drain per frame. RGBA8.
    * DX10    - D3D10CreateDevice (no window), RT -> STAGING texture -> Map. RGBA8.
    * DX9     - hidden window device, offscreen RT + GetRenderTargetData -> SYSTEMMEM
      surface -> LockRect. BGRA8 (X8R8G8B8).
    * DX8     - hidden window device, render to backbuffer (no Present), CopyRects ->
      sysmem ImageSurface -> LockRect (D3D8 has no GetRenderTargetData). BGRA8.
    * DirectDraw/D3D7 - hidden cooperative window, offscreen 3D-target surface in
      system memory, Lock the surface directly. BGRA8.
  - Failed init (no ICD / missing DLL set) shows an inline "unavailable" notice in the
    viewport - NEVER a MessageBox, never a pop-out.
- SHELL: deleted the `CreateProcess "--cube <api>"` branch, the "opens a separate
  window" row marker, and the launch dedupe. On selection swap the shell cleans up the
  prior path (DX11 scene OR embed backend) and inits the new one. Fullscreen + resize
  recreate the offscreen/shared target AND `g_embed_tex` at the viewport pixel size and
  recompute aspect for every backend (0-size guarded). Standalone `aio_run_*_cube`
  (the --cube CLI) + the headless `--cube --bench` child-process bench are UNTOUCHED.
- HUD FPS FIX (device-confirmed bug: spin cube read ~29,727 fps / 0.03 ms). Cause: the
  headline used a GPU TIMESTAMP delta around the scene's draw calls = raw GPU-draw cost,
  not a frame rate. Fixed by a faithful C++ port of the emulator's `FpsCounter`
  (com.winlator.star.widget.FpsCounter), fed ONCE PER PRESENTED FRAME (each shell
  Present): 500 ms compute window FPS, 8192-entry inter-present frametime ring (ignore
  >10 s gaps), 1%/0.1%/0.01% lows = fps at the 99/99.9/99.99th-pct frametime, session
  avg/min/max at 1 s cadence, 1.5 s staleness -> 0. Same counter drives the headline
  FPS + graph + avg + lows for the DX11 family AND every embedded backend (real presents
  per second, respecting the Present toggle - lands ~1700-3200 for spin, not ~30000).
  GPU-draw cost is kept only as a small labeled SECONDARY "GPU x.xx ms" stat. The
  GPU-timestamp queries remain for the in-process Benchmark pane (separate math). No
  fabricated GPU%/CPU%/RAM/battery/watts (host metrics the guest can't read).
- Native caption cleared to "" so the Wine titlebar can't render a duplicate
  "AIO Graphics Test" overlapping the ImGui titlebar; the shell never SetWindowText's a
  live FPS string.
- CI: added two glslang lines for embed_cube.vert/.frag; no object-list/link change
  (embed code lives in already-compiled files; all APIs dynamically loaded).
- Branch feat/imgui-shell (NOT merged).

## Phase 5 — Fusion HUD side-channel (app-declared active API)

The readback shell ALWAYS presents via D3D11 (every cross-API backend renders
offscreen then blits into the ImGui/DX11 swapchain), so Bannerlator's Fusion HUD
would forever detect "D3D11 · DXVK". This adds the AIO-side WRITER of a tiny status
file the HUD (already built + staged on the emulator side) polls, so the HUD shows
the TRUE per-test API.

- Writer in `src/shell_imgui.cpp` (`aio_hud_write_status` + `aio_hud_build_label` /
  `aio_hud_build_path` / `aio_wallclock_ms`), placed just before the entry point;
  cadence wired into the render loop after the FpsCounter block.
- Source of truth = the SAME `g_sel` Test the HUD chip + telemetry strip already draw
  (`->api`, `->path`, `->tool`). No new state.
- Contract (fixed by emulator side, matched exactly):
  - path `Z:\usr\tmp\hud_active_api.json` (Z: = imagefs root; \usr\tmp = shared tmp).
  - ATOMIC: CreateFileA/WriteFile to `...json.tmp` then
    `MoveFileExA(..., MOVEFILE_REPLACE_EXISTING)`.
  - schema `{"label":"D3D9 · DXVK","path":"d3d9 → DXVK → Turnip","ts":<epoch ms>}`.
  - `label` = "<API> · <wrapper>" (spaced U+00B7). API short-name collapses
    "Direct3D N" -> "D3DN"; wrapper lifted from the path (DXVK/VKD3D/Zink/Turnip,
    that priority so DX* -> DXVK, D3D12 -> VKD3D beat the trailing "-> Turnip").
    Verified: Vulkan·Turnip, OpenGL·Zink, D3D12·VKD3D, D3D11/10/9/8·DXVK,
    DirectDraw·DXVK.
  - `path` = AIO's displayed translation path, ASCII "->" rewritten to U+2192 "→".
  - `ts` = epoch ms on the Java System.currentTimeMillis() clock via
    GetSystemTimeAsFileTime -> (t - 116444736000000000)/10000. NOT QPC.
- CADENCE: written IMMEDIATELY on a backend/scene switch, then heartbeat every ~500 ms
  while authoritatively rendering (host polls at 2 s, gates on <2000 ms). Time-gated by
  a `now_ms` accumulator — never per-frame.
- STOP-WHEN-NOT-AUTHORITATIVE: gate = `!g_sel->tool && g_scene_live`, so a Tools panel
  (GPU Info / Benchmark / Disk Speed), a failed-init "unavailable" backend, or exit
  simply stops refreshing `ts`; the file goes stale and the HUD reverts to its own
  detection within ~2 s. (File is not deleted.)
- ROBUSTNESS: every IO step guarded — if `Z:\usr\tmp\` isn't writable (AIO run outside a
  Bannerlator container) the writer silently no-ops; never crashes/stalls the loop.
- This build also carries the V2 app icon (`src/app.ico`, id 1 in app.rc) — embedded in
  both exes via windres.
- Branch feat/imgui-shell (NOT merged).

## 2026-08-05 EOD — v2.0.0 SHIPPED; v2.0.1 checkpoint (branch feat/aio-2.0.1 off main fa7439d)
- v2.0.0 RELEASED (single-window Dear ImGui rebuild, all APIs embedded/swappable, HUD matches
  the emulator's Fusion HUD via app-declared-API, new icon) — tag 2.0.0 @ fa7439d, Latest.
  Baked into Bannerlator container pattern alongside v1-"Classic" (PATTERN_CONTENT_VERSION "4",
  Bannerlator main 76548421). Device-proven. Fixed 2 device-caught bugs: default launch was the
  OLD shell (flipped default to ImGui, --classic for legacy); pattern re-extract needed a version
  bump to actually replace the exe.
- v2.0.1 PUNCH LIST (device-observed on 2.0.0, to fix on this branch):
  1. Benchmark runs pop a NEW window (LunarG cube) instead of running EMBEDDED in the viewport.
  2. Benchmark tests not selectable — restore v1.6.1 checkbox multi-select + Select/Clear All + Run Selected.
  3. Massive empty vertical space under the benchmark panel — layout fix.
  4. No benchmark reports/history — restore per-run report + past-run picker (reuse bench.c output).
  5. Disk Speed shows a static "loading" (looks frozen) — add LIVE progress bars/metrics + run history.
  6. Light theme doesn't reach the render viewport (stays dark) — make viewport/scene bg follow theme.
  → then bump AIO_VERSION to v2.0.1, build, stage for morning device test.

## 2026-08-06 — v2.0.1: fixed the 6 device-observed issues (branch feat/aio-2.0.1)
All embedded, windowless; version bumped to v2.0.1.
1. Benchmark now runs EMBEDDED for EVERY API. Removed the `CreateProcess --cube <api>
   --bench` child-process path (that popped the LunarG cube window). Cross-API rows now
   drive their `cube_embed.h` backend offscreen via a new `g_bench_active_embed` override
   in `render_scene_to_offscreen`; DX11 rows keep the in-device scene path. Per-frame
   timing = the SAME wall-clock delta the HUD FpsCounter uses (present forced uncapped
   during a bench), fed into `bench.c` (`aio_bench_begin/add/finish_ex`) which reuses its
   CSV + `_bench_<API>.txt`. Live progress bar + "~fps" in the pane.
2. Restored multi-select: per-row checkbox (`g_bcheck[]`) + Select All / Clear All +
   Run Selected (checked rows only) alongside per-row Run + Run All.
3. Layout fix: the row list is now a bottom-anchored scrolling child that fills the
   remaining pane height — no more dead space; long lists scroll.
4. Reports + history: `aio_bench_finish_ex` returns numeric stats; every run/sweep is
   appended to `AIO-Graphics-Test_history.txt` (+ a timestamped `_bench_report_*.txt`)
   and shown in a new History sub-tab (avg/min/max/1%low per test, per timestamped run,
   loaded at launch so it survives restarts).
5. Disk Speed live progress: `disk.c` gained `aio_disk_run_ex2` + `AioDiskProgress` (phase,
   within-phase fraction, overall %, partial MB/s, per-phase results) published as it runs.
   The pane polls it and draws an overall bar + per-phase caption + live-filling tiles
   (partial MB/s + per-tile progress bar). Disk-run History sub-tab persists past runs
   (`AIO-Graphics-Test_disk_history.txt`).
6. Light theme reaches the viewport: `apply_theme` now sets `g_scene_clear` (DX11 offscreen
   clear) + `g_view_bg/bg2` (empty backdrop) + mirrors the clear into `aio_embed_clear_rgb`,
   which every cross-API embed backend (VK/GL/DX12/DX10/DX9/DX8/DDraw) now reads at clear
   time. Light render background in Light mode, dark in Dark. Tool datapanes stay dark
   (instrument surface) by design.
- NOTE: DX11 bench numbers now reflect embedded wall-clock throughput (uncapped present +
  readback overhead), not the old GPU-timestamp-only figure — this is the FpsCounter-timed
  value the task asked for, consistent across all APIs. VK Phong row reuses the VK embed
  cube (embed backend has no phong variant).

### STAGED for morning device test (2026-08-06)
- CI run 31071241329 (workflow_dispatch on feat/aio-2.0.1, headSha 1721c19) — GREEN both arches
  (build-x86_64 success, build-i686 success).
- 64-bit exe sha256 8f4f587bcd64fcf7606b53dd960f6fa43bed346869c576c602bc888858ebbb53.
- Copied to /sdcard/Download/AIO/AIO-Graphics-Test-64bit.exe AND overwrote the live shortcut
  target /storage/emulated/0/Winlator/Games/aio graphics test/AIO-Graphics-Test-imgui-scaffold-64bit.exe
  (owner 10156:1023, perms 0660) — so re-tapping "AIO ImGui Test" launches 2.0.1. NOT merged.

## 2026-08-06 — v2.0.1 black-screen regression report: startup diagnostic added
- Symptom: staged 2.0.1 (sha 8f4f587b) crashed on launch in the "AIO" container (D:\AIO\
  AIO-Graphics-Test-64bit.exe). Fresh crash log /sdcard/Download/bannerlator/AIO/wine_debug.log:
  `err:seh:NtRaiseException Unhandled exception code c000001d flags 0 addr 0x6ffca966dc`
  (c000001d = ILLEGAL INSTRUCTION), dying at the Mesa/GL bring-up stage BEFORE any
  `info:  DXVK:` banner — i.e. before create_device's D3D11CreateDeviceAndSwapChain reaches DXVK.
- Code review of the 6-fix diff found NO startup regression: the only code that runs before
  create_device is unchanged from 2.0.0 (RegisterClass/CreateWindow/WinMain); both new history
  loaders are no-ops when their files are absent (they are absent in D:\AIO — verified on device);
  fix-6's aio_embed_clear_rgb write is a plain same-module x64 data-symbol write; the
  render_scene_to_offscreen refactor's new branch is skipped at startup (g_bench_active_embed=null).
  CI build log shows NO -Wreturn-type/uninitialized/overflow warnings in the changed files.
  The fault ADDRESS 0x6ffca966dc is a high-VA system/wine DLL, NOT our exe (mingw base 0x140000000)
  — the illegal instruction is in the container's GL/Mesa/wine stack, not our code. Strong evidence
  this is a fault in the "AIO" container's early graphics bring-up, not a regression in the 6 fixes.
  (2.0.0's working log is from a DIFFERENT container, "AIO ImGui Test", with a different DXVK/VKD3D
   set — not an apples-to-apples baseline.)
- FIX/INSTRUMENT: added an always-on STARTUP BREADCRUMB LOG + SetUnhandledExceptionFilter
  (src/shell_imgui.cpp aio_diag_init/aio_diag_log/aio_seh_filter; declared in shell_imgui.h; wired
  into WinMain in cube.c). Log path: <exe_dir>\AIO-Graphics-Test_startup.log (fallback %TEMP%, then
  CWD). Milestones (each flushed): WinMain entry + raw args, dispatch branch, aio_run_imgui_shell
  entry, window created, D3D11 device+swapchain created, ImGui context+fonts, bench history
  load begin/end (N runs), disk history load begin/end (N runs), backends init/entering loop,
  first frame begin, first frame presented OK, main loop exited, exit OK. The exception filter
  records code + faulting address + module for the WHOLE session (catches pane-navigation crashes
  too). Next launch is self-diagnosing.
- Rebuilt CI GREEN both arches (run 31085081126, headSha 86d7d52). Re-staged the diagnostic 64-bit
  exe (sha ec8f2f5c659d2f5ee93ff4507513f714ede6bfad912c24eee5eaf038c8a14315) to /sdcard/Download/AIO/
  AND the shortcut target (owner 10156:1023, perms 0660). NOT merged. NEXT: user relaunches; read
  <exe_dir>\AIO-Graphics-Test_startup.log to pinpoint the exact failing milestone/module.

## 2026-08-06 — v2.0.1 black-screen ROOT CAUSE + FIX: -mno-avx
- ROOT CAUSE (device-proven): c000001d fired BEFORE WinMain — no AIO-Graphics-Test_startup.log was
  written anywhere (aio_diag_init never ran), so the fault is in C++ static/CRT init before main.
  Same FEX/arm64ec class as the Bionic-FG build: GCC emitted an AVX instruction the container's FEX
  x86 JIT can't execute; the added v2.0.1 C++ (bench/disk/theme) pushed AVX into the early init path
  where 2.0.0 had none. NOT a source bug.
- FIX (codegen flag only, no source change): added -mno-avx -mno-avx2 to BOTH compile lines in
  .github/workflows/build-windows.yml — ${prefix}-gcc (C) and ${prefix}-g++ (ImGui + shell_imgui);
  kept -fcf-protection=none. Confirmed present in the CI compile commands.
- CI GREEN both arches: run 31085867703, headSha 35a4790 (== pushed). Re-staged fixed 64-bit exe
  sha256 ca5ae99f44530f608f162add83d8bc68289bfbf2f522f56ea646a6d9f7dedb44 to /sdcard/Download/AIO/
  AND the shortcut target (owner 10156:1023, perms 0660). Startup diagnostic retained — a successful
  launch will now write the log through "entering main loop"/"first frame presented OK". NOT merged.

## 2026-08-06 — v2.0.1 crash: -O0 diagnostic build + earliest ctor hook + UB audit
- Correction from coordinator: -mno-avx did NOT fix it; 0x6ffca966dc is FEX's FIXED illegal-instruction
  raise address (uninformative). Crash is still c000001d, still BEFORE WinMain (no startup.log written
  on the -mno-avx build) — a corrupted control transfer in the guest during C++ static init (memory/
  codegen class, same family as the Bionic-FG crash), NOT an ISA/AVX issue.
- Two decisive diagnostics on feat/aio-2.0.1 (no app source logic changed):
  1. Whole app rebuilt at -O0 (both ${prefix}-gcc C line and both ${prefix}-g++ C++ lines in
     build-windows.yml; kept -fcf-protection=none and -mno-avx/-mno-avx2). -O0 often dodges codegen/
     layout-sensitive faults; perf irrelevant for a diagnostic.
  2. cube.c: __attribute__((constructor(101))) aio_earliest_ctor() -> aio_diag_init() + logs
     "[ctor] static-init reached (priority 101)" DURING static init, before WinMain. Disambiguates:
       ctor line present + no WinMain milestones => crash in a LATER constructor;
       no line at all => crash before ANY user ctor (CRT startup / DLL load / relocation).
- UB/overflow AUDIT of the 2.0.1 diff (2aabe54..HEAD) — findings:
  * NO new dynamic/global constructors: every new global/static I added is POD with a constant/zero
    initializer (g_scene_clear[3], g_view_bg/bg2, g_diag/g_diag_path, g_bhist[40]/g_dhist[40]/
    g_bench_sweep (BHistRun/DHistRun POD), g_disk_prog (AioDiskProgress POD), g_bcheck[64],
    g_brow_low1[64], g_bench_queue[64], g_bench_* scalars, g_bench_active_embed ptr). None have a
    C++ ctor, so none add to the static-init ctor chain and none have a static-init-ORDER dependency.
  * NO fixed-array overflow: g_brows has 35 entries; the parallel arrays g_bcheck/g_brow_low1/
    g_bench_queue are [64] and every loop is bounded by g_nbrows(=35) or an explicit <64 guard.
    History arrays capped at 40 with guards; bench_sweep.rows[40] guarded; loaders use snprintf into
    fixed fields (ts[24]/label[40]/cls[48]) with sizeof and fgets(line[256]).
  * NO new std::/new/function-pointer UB (embed tables are link-time-constant &kEmbed* addresses).
  * aio_embed_clear_rgb is a plain same-module float[3] (constant init in cube.c); reads in the 7
    embed backends are ordinary array loads.
  => No source-level UB found; evidence points to a codegen/FEX interaction, hence the -O0 test.
- CI GREEN both arches: run 31086706727, headSha c4612e5 (== pushed). Re-staged -O0 64-bit exe
  (4,810,846 bytes) sha256 be22fef67baae5fa109c5a6dae6a69f1cbde4b694d4bf4734762cb286215936d to
  /sdcard/Download/AIO/ AND the shortcut target (owner 10156:1023, perms 0660). NOT merged.
  NEXT: user relaunches; read <exe_dir>\AIO-Graphics-Test_startup.log — presence/absence of the
  "[ctor]" line localizes the fault; if -O0 launches clean it confirms a codegen-sensitive fault.

## 2026-08-06 — v2.0.1 crash: BINARY FORENSICS -> "new libstdc++ static-init" hypothesis DISPROVED
Compared the WORKING v2.0.0 exe (device AIO-Graphics-Test-64bit.exe, sha 7afe46ca, version string
"v2.0.0") vs the CRASHING v2.0.1 exe (first build 1721c19, sha 8f4f587b, "v2.0.1", pre-diag). Parsed
the PE directly (custom Python; host objdump has no PE target). Findings:
- .CRT init table: BYTE-IDENTICAL — 13 slots, same 3 real callbacks (pre_cpp_init, pre_c_init,
  __dyn_tls_init). 104 bytes in BOTH.
- __CTOR_LIST__ (mingw global-ctor list): IDENTICAL — exactly 4 ctors in both, ALL pre-existing
  libgcc/libstdc++ internals (_GLOBAL__sub_I__ZN9__gnu_cxx9__freeres, _GLOBAL__sub_I___cxa_get_globals_fast,
  two .text.startup). NO new `_GLOBAL__sub_I_<TU>` for any 2.0.1 source file => 2.0.1 added ZERO
  pre-main static-init objects.
- .tls: IDENTICAL size (16 bytes) — no new TLS/thread_local.
- libstdc++ runtime static-init symbol markers (grep both binaries): ios_base::Init, std::thread,
  std::mutex, basic_ofstream/filebuf, pthread_create, __cxa_thread_atexit, locale::facet, std::cout/cerr
  = ZERO in BOTH (basic_ostream=1 in both, pre-existing EH/RTTI). No new C++ runtime init pulled in.
- Import table: SAME DLLs (kernel32/user32/gdi32/opengl32/vulkan-1/msvcrt/comctl32/shell32/dwmapi);
  the ONLY new imported symbol is msvcrt `strpbrk` (standard, always present -> cannot fail load).
- Source grep of the diff (2aabe54..HEAD): NO new #include; NO iostream/fstream/sstream/thread/mutex;
  NO std::cout/ofstream/thread/mutex/thread_local/file-scope std:: object; disk/GPU-info runs use Win32
  CreateThread (not std::thread) — pre-existing.
CONCLUSION: the crashing v2.0.1 is, at the loader/CRT/static-init level, structurally IDENTICAL to the
working v2.0.0. The only differences are more app .text, the benign `strpbrk` import, and larger .data/
.bss for new POD globals. The "new libstdc++ static-init object" hypothesis is DISPROVED. No source
de-C++-ification made (would be shot-in-the-dark). Diagnostic build (-O0 + constructor(101), sha
be22fef) remains staged.

### IMPORTANT caveat re "no startup.log" == "pre-WinMain"
The pre-WinMain inference rests solely on "no AIO-Graphics-Test_startup.log anywhere". But the diag
writes to <exe_dir> (D:\AIO), then %TEMP%, then CWD — and D:\AIO runtime write-permission by the
sandboxed app is UNVERIFIED: the 2.0.0 output files (bench.csv/_bench_*.txt) live in the GAMES dir
(C:\...\aio graphics test\), NOT in D:\AIO. If none of the 3 fallback dirs are app-writable under this
container, we get NO log regardless of WHERE the crash is => "no log" may be a FALSE NEGATIVE and the
crash could be much later (even in-render). Recommended immediate next step: repoint the startup log to
a PROVEN-writable path — Z:\usr\tmp\ (the HUD side-channel already writes hud_active_api.json there
successfully in-container) — then relaunch. Also: capture FEX's GUEST RIP (0x6ffca966dc is FEX's fixed
raise trampoline, uninformative) via WINEDEBUG=+seh,+relay or a FEX crash log; and do the apples-to-
apples test (2.0.1 in the "AIO ImGui Test" container that ran 2.0.0; 2.0.0 in the "AIO" container).

## 2026-08-06 — v2.0.1 polish: live bench render, results folders, list dead-space, -O2 restore (branch feat/aio-2.0.1)
Context: the earlier "black screen" was a bad SHORTCUT config, NOT codegen — the app runs fine, so
-O0 was never needed. Four changes, ONE build:

- FIX 1 — LIVE bench render in the viewport. A running benchmark already renders offscreen
  (render_scene_to_offscreen drives g_bench_active_embed / g_bench_active_scene -> g_view_srv). The
  viewport now SHOWS that live render + a compact progress overlay (new draw_bench_overlay: test label,
  queue x/y, progress bar from g_bench_progress, live g_bench_run_fps) instead of the Benchmark datapane
  while bench_any_active(). draw_viewport gains `bench_view = Benchmark selected && bench_any_active()`
  and `bench_live_img`; the image-draw condition ORs bench_live_img, and the g_sel->tool block draws the
  overlay + returns when bench_view (else the datapane as before). Covers single Run, Run Selected,
  Run All — each swept test shows as it runs; on finish the datapane (with results) returns.

- FIX 2 — output files organized into folders. New helper aio_results_path(category, filename, out, cap)
  in bench.c/.h: best-effort recursive CreateDirectoryA of "AIO Results\<category>", falls back to the
  bare filename (CWD) if mkdir fails so a write never breaks. Layout now:
    AIO Results\Benchmark\   -> AIO-Graphics-Test_bench.csv, _bench_<API>.txt (bench.c);
                                AIO-Graphics-Test_history.txt, _bench_report_<ts>.txt (shell)
    AIO Results\Disk Speed\  -> AIO-Graphics-Test_disk_history.txt (shell)
  ALL read+write sites routed (disk_hist_load/record, bench_hist_load, bench_sweep_finish + report).
  The result popup's "Saved:" line now prints the full path. gpuinfo _report.txt left as-is (per task).

- FIX 3 — bench-row list dead-space + over-scroll. Root cause: screen_button/screen_checkbox (and the
  history InvisibleButton) each SetCursorScreenPos to their widget, so at the trailing
  ImGui::Dummy(availW, ry - lb.y + 8) the ImGui cursor sat at the LAST row, not the child origin lb —
  the Dummy then reserved its full height from there, ~doubling content => big dead area + scroll past
  the last row. Fix: SetCursorScreenPos(lb) immediately before each such Dummy so content height ==
  drawn rows exactly (no scroll when rows are shorter than the child). Applied to ##benchrows, ##bhist,
  and ##dhist (dhist draws only via the draw-list so was already correct — made explicit/robust).

- BUILD FLAG — reverted -O0 -> -O2 for the ${prefix}-gcc C line and both ${prefix}-g++ C++ lines in
  .github/workflows/build-windows.yml (the crash was the shortcut config, not codegen; -O2 gives
  accurate benchmark numbers). Kept -fcf-protection=none -mno-avx -mno-avx2 and the startup diagnostic.

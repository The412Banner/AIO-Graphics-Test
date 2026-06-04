# AIO Graphics Test

An all-in-one graphics **diagnostic + benchmark** tool for the **Winlator/Gamehub/GameNative** Wine
graphics stack on Android (DXVK, VKD3D-Proton, Turnip, Zink). One Windows `.exe` you drop
into a container — it replaces `vkcube.exe`, `GPUInfo.exe`, and the whole `3d-tests` kit
(DX8/9/11/12 + GL demos) with a single, touch-friendly, instrumented binary that renders
the **same cube through every graphics API** so you can see exactly which translation layer
is broken or slow. On top of the diagnostic cubes it bundles a gallery of detailed
**procedural showcase scenes** (a photoreal planet, a neon-rain city, moonlit dunes, a
volumetric nebula, a "Seascape" ocean) that double as fragment-heavy benchmarks, and a full
**dark theme**.

Forked from Khronos [Vulkan-Tools `vkcube`](https://github.com/KhronosGroup/Vulkan-Tools)
(`cube.c`, tag `sdk-1.3.239.0`, Apache-2.0). The self-contained pre-codegen base links
`vulkan-1` directly (no `cube_functions.h` generation step).

> **Idea & inspiration:** [**Nick (@Xnick417x)**](https://github.com/Xnick417x) — co-author. See [Credits](#credits).
>
> **▶ Showcase video:** https://youtu.be/gKhwjwjGvWI

## Download

Grab the latest `AIO-Graphics-Test-64bit.exe` (or `-32bit.exe`) from the
[**Releases**](https://github.com/The412Banner/AIO-Graphics-Test/releases) page, drop it
into a Winlator container, and run it — it opens a menu, no install. See it in action in the
[**showcase video**](https://youtu.be/gKhwjwjGvWI). The container already
provides `vulkan-1.dll` (every Winlator container does). The exe's embedded cube icon is
used as the shortcut art when you add it to a container.

## What it does

A single binary, driven by an **in-app menu** (touch-friendly — Winlator is a touchscreen),
with optional CLI shortcuts:

- **App shell** — a persistent left-sidebar menu + content pane, **dark-themed throughout**
  (window, push buttons, checkboxes, panes, and report scrollbars are all custom dark-painted).
  Pick a test; GPU Info opens in-frame, cube/scene tests open in their own window (so the menu
  stays usable).
- **GPU / driver report** — Vulkan and OpenGL shown as **two stacked, independently-scrollable
  panes** on one page (device, driver + API version, memory heaps, queue families, features,
  extensions). Replaces `GPUInfo.exe`.
- **Multi-API renderer** — the same cube through **every** graphics API in the Winlator
  stack, so 5/6/7/8/9/10/11/12 are all covered:

  | Backend | Path it exercises |
  |---|---|
  | **Vulkan**     | native Vulkan → **Turnip** (no translation — the baseline) |
  | **OpenGL**     | OpenGL → **Zink / wined3d** → Vulkan |
  | **DirectDraw (DX5/6/7)** | **Wine `ddraw`** → wined3d → OpenGL — the legacy path **DXVK does not implement**, a genuinely different stack from everything below. A Direct3D 7 immediate-mode cube + a pure-2D blit test. |
  | **Direct3D 8** | **DXVK** d3d8 → d3d9 → Vulkan |
  | **Direct3D 9** | **DXVK** d3d9 → Vulkan |
  | **Direct3D 10**| **DXVK** d3d10 → d3d11 → Vulkan |
  | **Direct3D 11**| **DXVK** d3d11 → Vulkan |
  | **Direct3D 12**| **VKD3D-Proton** d3d12 → Vulkan |

  > **Two builds, two bitnesses.** Each release ships `AIO-Graphics-Test-64bit.exe` (**64-bit**)
  > and `AIO-Graphics-Test-32bit.exe` (**32-bit**). A 64-bit process can only load the
  > container's *x64* DXVK / VKD3D / ddraw DLLs; a 32-bit process loads the separate *x32*
  > set. Since almost every legacy DirectX game (and all DX5/6/7 titles) runs as 32-bit, the
  > x86 build tests DLLs the x64 build physically can't reach. Use whichever matches the game
  > you're debugging — or run both to compare the two DLL sets.

- **DX11 test suite** — Direct3D 11 has the deepest coverage: a suite of scenes, each
  stressing a different part of the pipeline / DXVK:

  | Scene | Exercises |
  |---|---|
  | Spinning cube | baseline pipeline |
  | Textured cube | texture upload + SRV + sampler |
  | Instanced (512 cubes) | instanced draw throughput |
  | Tessellation | hull/domain shaders (feature level 11) |
  | Compute particles | the D3D11 compute path (UAV/SRV) |
  | **Dolphin** | the classic **DolphinVS** underwater scene — the real mesh tweened across 3 keyframes, seafloor + 32-frame animated caustics (see [Credits](#credits)) |
  | Raymarch SDF | a signed-distance field marched in the pixel shader over a fullscreen triangle — a heavy **fragment-ALU** workload (a different bottleneck from the geometry scenes) |
  | GS exploder | the **geometry-shader** stage — each triangle pushed out along its face normal (the only scene that exercises GS; emulated on Turnip) |
  | Cel shading | quantised toon bands + a silhouette outline |
  | Matcap | a procedural matcap (no texture) — view-space normal → chrome-ball lighting |
  | Atomics | a histogram built with **`InterlockedAdd`** atomics + clear-UAV + a structured-buffer SRV-in-VS (a correctness probe for the translation layer) |
  | Draw stress (128–2048) | thousands of **individual draw calls** (not instanced) → **CPU/submit-bound**, a different axis from every GPU-bound scene; pick the draw count to map the scaling curve |

- **Demo Scenes** — a gallery of procedural **showcase** scenes (all ray-marched in the D3D11
  pixel shader, so they double as heavy fragment-ALU stress tests), reached via **Demo Scenes ▸**:

  | Scene | What it renders |
  |---|---|
  | **Space** | a photoreal planet from orbit — multi-octave terrain, biomes + ice caps, drifting clouds with shadowing, ocean sun-glint, atmospheric limb scattering, night-side city lights, cratered asteroids, ACES tonemap |
  | **Cityscape** | a rainy neon night — hashed buildings with a real skyline, lit windows (frames/curtains/occupant silhouettes), neon signs, taxis, rooftop water tanks + AC units, a wet reflective street, animated rain, cinematic grade |
  | **Desert** | a moonlit dune sea — rolling heightfield dunes, cool moonlight + soft terrain shadows, wind ripples, a craters moon and a star field |
  | **Detailed Nebula** | volumetric emission/absorption — glowing gas (red → magenta → reflection-blue temperature palette) + dark dust lanes, three embedded stars lighting the gas from within, dithered front-to-back march |
  | **Ocean v2** | a "Seascape" sea — choppy summed wave octaves, binary-search heightmap trace, Fresnel sky-reflection vs deep-water refraction, subsurface crest glow, sun specular + sun disk |
  | Showcase · Raymarch SDF · Mandelbulb · Volumetric nebula · Ocean · Cel · Matcap | the original lighter procedural scenes (reflections + soft shadows, a raw SDF march, a fractal, simple volumetrics, toon, chrome matcap) |

- **Native-Vulkan scenes** — beyond the textured cube, the Vulkan backend has a **Phong-lit**
  cube (full ambient + diffuse + specular on the *native* VK path, no DXVK) and a
  **mesh-shader probe** that reports whether the device/driver exposes `VK_EXT_mesh_shader`
  (Turnip on Adreno currently does not — the probe is the diagnostic).

- **Benchmark** — a view (or `--bench <sec>`) that times a run and reports **Avg / Min / Max**
  + **1%-low** FPS (with a per-frame **CSV**). Min/Max use the 1st/99th percentile so a stray
  sub-frame timing glitch can't produce an absurd reading; a short **warm-up window** is
  excluded so first-frame shader compilation doesn't skew the result.
  - **Tick what to run** — every test is a checkbox (with **Select All / Clear All**); the many
    D3D11 scenes collapse under one **Direct3D 11** group (and the draw-stress counts under one
    **Draw Stress** sub-row).
  - **Run Selected** sweeps the ticked tests **sequentially and hands-free** (no GPU
    contention between tests).
  - **Selectable length** — 15 / 30 / 45 / 60 s — and a **Vsync toggle**.
  - **Benchmark Results** screen — the full Avg/Min/Max for every test, with a **run picker**:
    each sweep is saved to disk with a **timestamp**, so you can review previous runs (they
    survive app restarts).
- **Semaphore Probe** — benchmarks the instanced D3D11 cube with **timeline vs binary**
  semaphores to measure the Turnip-kgsl timeline-semaphore regression, and prints a plain
  verdict (e.g. *"binary is 1.7× faster"*). (The binary path only differs on a DXVK build
  that honors `DXVK_DISABLE_TIMELINE_SEMAPHORES`.)
- **Disk Speed** — a non-graphics view that measures **sequential write**, **sequential read**,
  and **random 4 KB read** throughput (MB/s + IOPS) against a temp file in `%TEMP%` (created and
  deleted each run), i.e. the *in-container* storage speed a game actually gets. Pick 128 / 256 /
  512 MB, then:
  - **Run (quick)** — fast, but Wine usually serves the read from the OS page cache (RAM), so the
    read/random numbers are RAM-fast, not real flash. Good for seeing the cache benefit.
  - **Real-Flash Read** — before reading, writes a RAM-sized **cache-buster** file to evict the
    test file from the page cache, so the read happens *cold* and reflects true storage speed
    (slower; writes several extra GB, deleted afterwards). The **write** figure is always real
    (flushed straight to storage).
  - **What's this?** — an in-app explainer of all of the above.
- **Live HUD** — every cube window shows the active API + live FPS as an on-screen overlay.
- **Hang watchdog** — if a backend deadlocks (e.g. a broken GL stack blocking in
  `SwapBuffers`), it self-closes after ~12 s instead of locking up the container.
- **Robust by design** — DXVK / VKD3D / d3dcompiler are all loaded **dynamically**, so the
  exe launches and shows a graceful notice even on a container that lacks them. All output is
  written to disk too, so results survive a PRoot/Termux OOM-kill mid-test.

## CLI shortcuts

The menu is the primary interface, but every mode has a flag too:

| Flag | What it does |
|------|--------------|
| *(default)* | Opens the app shell (menu) |
| `--gpuinfo` / `--report` | Dump GL + VK adapter info to console + `AIO-Graphics-Test_report.txt`, then exit |
| `--cube vk\|gl\|dx7\|dx8\|dx9\|dx10\|dx11\|dx12` | Launch a backend directly in its own window |
| `--cube ddraw2d` | Launch the pure-2D DirectDraw blit test (`dx7` = the DirectDraw 3D cube) |
| `--cube vk --scene phong\|meshshader` | Native-VK Phong-lit cube, or the mesh-shader probe |
| `--cube dx11 --scene <name>` | Pick a DX11 scene. **Pipeline tests:** `spin` `textured` `instanced` `tess` `compute` `dolphin` `gsexplode` `atomics` `drawstress`. **Showcases:** `space` `city` `desert` `nebula2` `ocean2` `showcase` `raymarch` `ocean` `mandelbulb` `nebula` `cel` `matcap` |
| `--cube dx11 --scene drawstress --draws <n>` | Draw-stress with N draws (128 / 256 / 512 / 1024 / 2048) |
| `--bench <sec>` | Run the launched cube as a timed benchmark (avg/min/max/1%-low + CSV) |
| `--vsync` | Present with vsync (default is uncapped) |
| `--autoclose <sec>` | Auto-dismiss the benchmark result popup after N s (Run All uses 3) |
| `--semaphore timeline\|binary` | Force the DXVK semaphore path (the probe uses this) |
| `--no-menu` | Skip the shell and launch the cube directly |

## Build

CI only (no local builds). Cross-compiled Linux → Windows PE on GitHub Actions
(`.github/workflows/build-windows.yml`) as a **two-arch matrix**: mingw-w64 + Vulkan-Headers
+ a cross-built Vulkan-Loader import lib + glslang + `windres` (icon) → `AIO-Graphics-Test-64bit.exe`
(**x86_64**) and `AIO-Graphics-Test-32bit.exe` (**i686**). Releases are the exact CI-built
artifacts. Only `vulkan-1` is statically imported (always present in a container);
`ddraw`/d3d8/9/10/11/12, `dxgi`, and `d3dcompiler` are loaded at runtime.

## Layout

```
src/cube.c            forked vkcube + WinMain dispatch (also the Vulkan backend)
src/menu.c            app shell (dark theme): sidebar, stacked GPU Info panes, scene/benchmark/probe views
src/cube_gl.c         OpenGL backend (WGL)
src/cube_ddraw.c      DirectDraw / legacy Direct3D backend (DX7 cube + 2D blit)
src/cube_d3d8.c       Direct3D 8 backend (DXVK d3d8 wrapper)
src/cube_d3d9.c       Direct3D 9 backend (DXVK)
src/cube_d3d10.c      Direct3D 10 backend (DXVK)
src/cube_d3d11.c      Direct3D 11 scene framework + 21 scenes (pipeline tests + procedural showcases)
src/cube_phong.frag   native-Vulkan Phong fragment shader (--scene phong)
src/cube_d3d12.c      Direct3D 12 backend (VKD3D-Proton)
src/gpuinfo.c         GL + VK adapter report
src/bench.c           benchmark instrumentation (avg/min/max/1%-low + CSV, vsync flag)
src/hud.c             in-window FPS/API overlay
src/watchdog.c        render-loop hang watchdog
src/dolphin_assets.h  embedded DolphinVS assets (generated)
src/app.rc, app.ico   app icon (PE resource; also the Winlator shortcut art)
tools/gen_dolphin_assets.py   one-time .x/.bmp/.tga → dolphin_assets.h
tools/gen_icon.sh             regenerate app.ico (ImageMagick)
cmake/                mingw-w64 cross toolchain file
```

## Credits

**AIO Graphics Test** is built by [**The412Banner**](https://github.com/The412Banner) with
co-author [**Nick (@Xnick417x)**](https://github.com/Xnick417x) — the **idea, inspiration,
and ongoing help** behind the project. Thank you, Nick. 🙏

It stands on open-source graphics work and was informed by the `3d-tests` reference kit:

- **Khronos `vkcube` / Vulkan-Tools** — `src/cube.c` is a fork of `cube/cube.c`
  (tag `sdk-1.3.239.0`), Apache-2.0. © The Khronos Group, Valve, LunarG.
- **Vulkan-Headers / Vulkan-Loader / glslang** — Khronos, Apache-2.0 (fetched in CI).
- **DXVK** and **VKD3D-Proton** — the translation layers this tool exercises (not bundled).

**DolphinVS assets** — the Dolphin scene embeds the original Microsoft *DolphinVS* DirectX
SDK sample data (dolphin + seafloor meshes, textures, and the 32-frame caustics), parsed
into `src/dolphin_assets.h`. Those assets are **© Microsoft Corporation**, included for the
homage scene; all trademarks belong to their owners. This project is independent and not
affiliated with or endorsed by Microsoft.

Full attribution is in [`CREDITS.md`](CREDITS.md). Everything else is reimplemented natively
— no third-party `.exe` binaries are bundled.

## License

Apache-2.0 (inherited from Vulkan-Tools) — see [`LICENSE`](LICENSE).

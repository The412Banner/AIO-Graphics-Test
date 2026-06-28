
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

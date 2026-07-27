# Bionic-FG validation harness

A deterministic Direct3D 11 **frame source** (`src/fgtest.c` → `AIO-FGTest-{32,64}bit.exe`)
plus a host-side **capture analyzer** (`tools/fg_analyze.py`) that together prove the
Bannerlator Bionic-FG Vulkan frame-generation layer is not merely *running* but
**effective, authentic, spatially correct, and smoothly paced.**

## Why a source + a capture (and not just "the FPS went up")

Bionic-FG is a Vulkan implicit layer that sits **below** the game in the swapchain.
A D3D11 app under DXVK therefore **cannot see or count** the frames the layer
inserts — with FG on, the app still honestly reports its own real cadence (e.g. 30).
So "measured FPS rose" cannot come from the app; it has to be read off the **actual
on-screen output**. This harness splits the job cleanly:

- **The exe is the ground truth.** It renders *real* frames at a precise, known cap
  and bakes machine-readable markers into every frame. Every on-screen element is a
  pure function of the integer real-frame index `fi` (wall-clock paces presents only,
  never places geometry), so the analyzer can reconstruct the exact expected image
  for any real frame and any interpolation phase between two of them.
- **The analyzer is the measurement.** It decodes a high-speed capture of the output
  against the `fgtest_results.json` the exe wrote, and counts/classifies what the
  layer actually put on the panel.

## Baked signals

| Signal | What it proves |
|---|---|
| **Flicker block** — fully inverts every real frame | A frame that reads ~50% grey is a blend of two reals → **it is a generated (interpolated) frame.** Primary real/gen discriminator and a live ghosting indicator. |
| **Barcode strip** — start + 20-bit `fi` + parity + stop | Crisp & valid → **real** frame (and we know *which* one); blurred/parity-fail → **generated**; crisp but a repeat of the previous index → **duplicate** (a broken FG that repeats instead of interpolating). |
| **Phase swatch** — hue cycles per real frame | Independent phase estimator; intermediate hue confirms interpolation. |
| **Velocity marker** — square sweeping at a fixed px/frame | A *correct* generated frame lands the marker at the true motion **midpoint**; a duplicate leaves it on a real position. → **interpolation accuracy in pixels.** |
| **Rotor / bars / occluder** | Motion-quality stressors (rotation, fast thin edges, dis-occlusion) for the visual/artifact pass — including the fast-lateral + occlusion case that separates FSR3 models 3 vs 4. |

## The tests

- **T1 Throughput** — cap the source at 30; the captured output rate must be
  ~60/90/120 at 2×/3×/4×. Negative controls: FG off → 30; `BIONIC_FG_DISABLE=1` → 30.
- **T2 Authenticity** — generated frames must sit at intermediate marker positions
  (low RMSE), **not** duplicate real frames (duplicate rate ~0).
- **T3 Latency** *(external)* — true FG adds ~1 frame of latency (it holds a frame to
  interpolate). Measure present→photon off vs on; no rise ⇒ not really interpolating.
- **T4 Pacing** — captured inter-frame interval variance low (no 33/0/33/0 judder).
- **T5 Quality** — `--scene stress`/`full`: inspect ghosting/warping/dis-occlusion;
  compare `BIONIC_FG_MODEL` 0/1 vs 3/4.
- **T6 Cross-vendor** — run T1–T5 on Adreno (proven) **and** Mali/Xclipse (our gap).

## Running it

Build via CI (**Actions → "Build Bionic-FG Tester (Windows PE)"**) → download
`AIO-FGTest-64bit.exe` (or 32-bit for legacy titles).

Interactive keys: `[` `]` cap ∓5 · `V` vsync · `Space` pause · `1`–`4` scene · `Esc`.

Headless / scripted:

```
AIO-FGTest-64bit.exe --cap 30 --scene verify --duration 12 --out fgtest_results.json
```

On device (root bridge sketch):

```sh
# 1. push the exe into a container's drive_c and the results path somewhere readable
bridge 'cp /sdcard/Download/AIO-FGTest-64bit.exe <prefix>/drive_c/'
# 2. launch it through the container with Bionic-FG 2x enabled for that shortcut,
#    while screen-recording the actual output:
bridge 'screenrecord --time-limit 15 /sdcard/Download/fg_cap.mp4'
# 3. pull the capture + the results file the exe wrote, then analyze on host:
python3 tools/fg_analyze.py fg_cap.mp4 fgtest_results.json --expect-mult 2
```

## Reading the result

`fg_analyze.py` prints a summary and writes `fg_analysis.json`:

- **generation ratio** ≈ multiplier → **T1 pass** (extra frames are really on screen).
- **duplicate rate** ~0 **and** **interp px RMSE** < ½·step → **T2 pass** (they are
  genuine interpolations, not repeats). RMSE ≈ half-step ⇒ the "FG" is duplicating.

## Limitations (be honest about these)

- **Capture Nyquist.** The capture's own fps must exceed the FG output rate or
  generated frames alias away; `screenrecord` caps at panel refresh, so use a 240 fps
  external camera to validate output above the panel rate.
- **Latency (T3)** needs external timing gear; it is not measured by this pair.
- A future companion (`BIONIC_FG_STATS`) exporting the layer's own real/generated
  counts would let the app read the output side directly and remove the capture step
  for T1/T4.

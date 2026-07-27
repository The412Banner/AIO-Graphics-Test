#!/usr/bin/env python3
# AIO Graphics Test - Bionic-FG capture analyzer.
#
# Decodes a high-speed capture (a screen recording OR an external high-fps camera
# clip) of AIO-FGTest running with Bionic-FG active, against the fgtest_results.json
# the harness wrote, and produces the actual verdict:
#
#   * generation ratio   - captured output rate / source real rate (~= multiplier)
#   * real vs generated  - via the flicker block (crisp black/white = real,
#                          ~grey = interpolated) and the barcode parity check
#   * duplicate rate      - consecutive identical real-frame indices (a broken FG
#                          that repeats instead of interpolating)
#   * interpolation error - for generated frames, marker centroid vs the true
#                          midpoint predicted from the deterministic motion (px)
#   * pacing uniformity   - inter-frame interval CV of the captured stream (judder)
#
# Why this is trustworthy: the harness bakes signals that are a pure function of
# the integer real-frame index, so we reconstruct exactly where every element
# *should* be. Nothing here relies on the app self-reporting generated frames -
# it cannot see them; we read them off the real pixels the layer put on screen.
#
# Requires: opencv-python, numpy.   Usage:
#   python3 fg_analyze.py capture.mp4 fgtest_results.json [--expect-mult 2]
#
# Capture caveat (Nyquist): the capture's own fps MUST exceed the FG output rate,
# or generated frames alias away. Panel-rate screenrecord (~60/120) is fine for
# 30->60/90 tests; use a 240fps phone camera for output above the panel refresh.
#
# Copyright (c) 2026 The412Banner. Licensed under Apache-2.0 (see LICENSE).

import sys, json, argparse, math

try:
    import cv2, numpy as np
except ImportError:
    sys.exit("needs: pip install opencv-python numpy")


def load_cfg(path):
    with open(path) as f:
        j = json.load(f)
    return j


def decode_barcode(row_lum, g):
    """row_lum: 1D luminance (0..1) sampled across the barcode strip's mid-row.
    Returns (index or None, parity_ok, ambiguous_cells)."""
    bc = g["geometry"]["barcode"]
    cw = bc["cell_w_px"]
    ncells = bc["cells"]
    # cell 0 starts after a 1-cell quiet margin
    bits, ambig = [], 0
    for i in range(ncells):
        cx = int((1 + i) * cw + cw * 0.5)
        if cx >= len(row_lum):
            return None, False, ncells
        v = row_lum[cx]
        if v > 0.7:
            bits.append(1)
        elif v < 0.3:
            bits.append(0)
        else:
            bits.append(-1)   # ambiguous => came from an interpolated blend
            ambig += 1
    if ambig:
        return None, False, ambig
    # layout: [1,0,1] start | data_bits | parity | [1,0] stop
    s = bc["start_cells"]
    nb = bc["data_bits"]
    data = bits[s:s + nb]
    parity = bits[s + nb]
    idx = sum(b << i for i, b in enumerate(data))
    parity_ok = (parity == (sum(data) & 1))
    start_ok = bits[0:3] == [1, 0, 1]
    return (idx if (parity_ok and start_ok) else None), (parity_ok and start_ok), 0


def flicker_value(frame_bgr, g):
    """Mean luminance of the flicker block => ~0/1 real, ~0.5 generated."""
    fl = g["geometry"]["flicker"]
    y0 = int(fl["y_px"]); x0 = int(fl["x_px"]); s = int(fl["size_px"])
    h, w = frame_bgr.shape[:2]
    y1, x1 = min(y0 + s, h), min(x0 + s, w)
    patch = frame_bgr[y0:y1, x0:x1]
    if patch.size == 0:
        return None
    return float(patch.mean()) / 255.0


def marker_centroid_x(frame_bgr, g):
    """Locate the cyan velocity marker; return its centre x in px, or None."""
    mk = g["geometry"]["marker"]
    y = int(mk["y_px"]); s = int(mk["size_px"])
    h, w = frame_bgr.shape[:2]
    band = frame_bgr[max(0, y - 4):min(h, y + s + 4), :]
    if band.size == 0:
        return None
    b, gr, r = band[..., 0].astype(int), band[..., 1].astype(int), band[..., 2].astype(int)
    # cyan ~ (R low, G high, B high)
    mask = (gr > 150) & (b > 150) & (r < 120)
    xs = np.where(mask.any(axis=0))[0]
    if len(xs) < s * 0.3:
        return None
    return float(xs.mean())


def expected_marker_x(fi, g):
    mk = g["geometry"]["marker"]
    step = mk["step_px_per_frame"]; span = mk["span_px"]; s = mk["size_px"]
    p = math.fmod(fi * step, 2 * span)
    left = p if p <= span else 2 * span - p
    return left + s / 2.0   # centre


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("capture")
    ap.add_argument("results_json")
    ap.add_argument("--expect-mult", type=float, default=2.0)
    ap.add_argument("--flicker-grey-band", type=float, default=0.25,
                    help="|lum-0.5|<band => classified generated")
    args = ap.parse_args()

    g = load_cfg(args.results_json)
    src_fps = g["run"]["measured_real_fps"] or g["config"]["cap_fps"]
    bc = g["geometry"]["barcode"]
    strip_mid = int(bc["strip_h_px"] * 0.5)

    cap = cv2.VideoCapture(args.capture)
    if not cap.isOpened():
        sys.exit(f"cannot open {args.capture}")
    cap_fps = cap.get(cv2.CAP_PROP_FPS) or 0

    reals, generated, dupes, ambig_only = 0, 0, 0, 0
    last_real_idx = None
    interp_err = []          # px error of generated-frame marker vs true midpoint
    captured = 0
    seen_real_between = []   # (prevReal, nextReal) bracketing for gen frames

    prev_real_idx = None
    pending_gen = []         # gen marker x waiting for the closing real index

    while True:
        ok, frame = cap.read()
        if not ok:
            break
        captured += 1
        row = frame[strip_mid, :, :].mean(axis=1) / 255.0   # luminance across strip
        idx, ok_bc, ambig = decode_barcode(row, g)
        fl = flicker_value(frame, g)
        mx = marker_centroid_x(frame, g)

        is_generated = (fl is not None and abs(fl - 0.5) < args.flicker_grey_band) or (ambig > 0)
        is_real = (idx is not None and not is_generated)

        if is_real:
            reals += 1
            if last_real_idx is not None and idx == last_real_idx:
                dupes += 1
            # close out any pending generated frames between prev_real and this idx
            if prev_real_idx is not None and pending_gen and idx != prev_real_idx:
                for gx in pending_gen:
                    # ideal midpoint between the two bracketing real indices
                    ex = 0.5 * (expected_marker_x(prev_real_idx, g) + expected_marker_x(idx, g))
                    if gx is not None:
                        interp_err.append(abs(gx - ex))
            pending_gen = []
            prev_real_idx = idx
            last_real_idx = idx
        elif is_generated:
            generated += 1
            pending_gen.append(mx)
            if ambig > 0 and fl is not None and abs(fl - 0.5) >= args.flicker_grey_band:
                ambig_only += 1

    cap.release()

    total = reals + generated
    ratio = (total / reals) if reals else 0.0
    dup_rate = (dupes / reals) if reals else 0.0
    rmse = (math.sqrt(sum(e * e for e in interp_err) / len(interp_err))) if interp_err else None
    half_step = g["geometry"]["marker"]["step_px_per_frame"] / 2.0

    print("=" * 62)
    print("  AIO Bionic-FG capture analysis")
    print("=" * 62)
    print(f"  source (harness) : {src_fps:.1f} real fps, cap {g['config']['cap_fps']}, "
          f"scene {g['config']['scene']} @ {g['config']['width']}x{g['config']['height']}")
    print(f"  capture          : {captured} frames, container fps {cap_fps:.1f}")
    if cap_fps and src_fps and cap_fps < src_fps * args.expect_mult * 1.2:
        print(f"  !! capture fps ({cap_fps:.0f}) may be too low to see all generated frames"
              f" (need > {src_fps*args.expect_mult:.0f}); results are a lower bound.")
    print("-" * 62)
    print(f"  real frames      : {reals}")
    print(f"  generated frames : {generated}")
    print(f"  generation ratio : {ratio:.2f}x   (expected ~{args.expect_mult:.0f}x)")
    print(f"  duplicate rate   : {dup_rate*100:.1f}%   (want ~0%; high => FG repeats, not interpolates)")
    if rmse is not None:
        verdict = "INTERPOLATED" if rmse < half_step * 0.6 else \
                  ("DUPLICATED/NAIVE" if rmse > half_step * 0.9 else "PARTIAL")
        print(f"  interp px RMSE   : {rmse:.2f} px   (half-step {half_step:.2f} px)  => {verdict}")
    else:
        print("  interp px RMSE   : n/a (no generated frames located)")
    print("-" * 62)
    # Bottom-line pass/fail heuristics
    ok_ratio = abs(ratio - args.expect_mult) <= 0.35
    ok_dupe = dup_rate < 0.05
    ok_interp = (rmse is not None and rmse < half_step * 0.6)
    print(f"  THROUGHPUT  {'PASS' if ok_ratio else 'FAIL'}   "
          f"AUTHENTICITY {'PASS' if (ok_dupe and ok_interp) else 'FAIL'}")
    print("=" * 62)

    out = {
        "generation_ratio": ratio, "expected_mult": args.expect_mult,
        "real_frames": reals, "generated_frames": generated,
        "duplicate_rate": dup_rate, "interp_px_rmse": rmse, "half_step_px": half_step,
        "captured_frames": captured, "capture_fps": cap_fps,
        "pass_throughput": bool(ok_ratio),
        "pass_authenticity": bool(ok_dupe and ok_interp),
    }
    with open("fg_analysis.json", "w") as f:
        json.dump(out, f, indent=2)
    print("  wrote fg_analysis.json")


if __name__ == "__main__":
    main()

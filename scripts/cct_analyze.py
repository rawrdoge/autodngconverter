#!/usr/bin/env python3
"""
cct_analyze.py -- Pixel-level CCT estimation from camera RAW files.

Illuminant estimators are faithful ports of the working implementations in
DeepColorBalancing (illuminant_estimation_clean.py), applied here to
camera-native linear RGB from rawpy instead of sRGB JPEGs:
  grayworld          - channel mean
  shadesofgrey       - Minkowski norm p=6 (Finlayson & Trezzi 2004)
  whitepatch         - per-channel 99th percentile
  maxrgb             - per-channel 95th percentile
  generalgrayworld   - Gaussian center-weighted mean, sigma=0.3*min(h,w)
  bayesian           - gray-world blended 30% toward D65 prior + nearest label

Usage: cct_analyze.py <raw_path> <area_spec> [algorithm]
Outputs a single JSON line; on failure {"error": "<msg>"} and exit code 1.
"""
import sys
import json
import math
import rawpy
import numpy as np
from colour import xy_to_CCT_Robertson1968, XYZ_to_xy, sRGB_to_XYZ

D50_X, D50_Y = 0.34570, 0.35850   # D50 chromaticity
SOG_P = 6.0                       # Shades-of-Grey Minkowski norm (F&T 2004)

def parse_area(area_spec, h, w):
    """Parse area spec into (y1, y2, x1, x2).

    Normalizes the strings the Lua panel sends ("center20"/"center10") as well
    as "full" and an explicit JSON rectangle. Unknown specs fall back to the
    center-20% window.
    """
    if area_spec == "full":
        return 0, h, 0, w
    if area_spec in ("center20", "center 20"):
        cy, cx = h // 2, w // 2
        sh, sw = h // 5, w // 5
        return cy - sh // 2, cy + sh // 2, cx - sw // 2, cx + sw // 2
    if area_spec in ("center10", "center 10"):
        cy, cx = h // 2, w // 2
        sh, sw = h // 10, w // 10
        return cy - sh // 2, cy + sh // 2, cx - sw // 2, cx + sw // 2
    try:
        d = json.loads(area_spec)
        y1 = max(0, min(int(d["y"]), h))
        x1 = max(0, min(int(d["x"]), w))
        return y1, min(y1 + int(d["h"]), h), x1, min(x1 + int(d["w"]), w)
    except (json.JSONDecodeError, KeyError, TypeError, ValueError):
        # Default: center 20%
        cy, cx = h // 2, w // 2
        sh, sw = h // 5, w // 5
        return cy - sh // 2, cy + sh // 2, cx - sw // 2, cx + sw // 2


COMMON_ILLUMINANTS = {
    "D55":  np.array([0.956, 1.000, 0.921]),
    "D65":  np.array([0.950, 1.000, 1.089]),
    "D75":  np.array([0.949, 1.000, 1.226]),
    "A":    np.array([1.098, 1.000, 0.356]),
    "TL84": np.array([0.982, 1.000, 0.826]),
    "F2":   np.array([0.991, 1.000, 0.673]),
    "F11":  np.array([1.009, 1.000, 0.644]),
}

BAYESIAN_PRIOR_WEIGHT = 0.3

def gray_world(img):
    illum = np.mean(img, axis=(0, 1), dtype=np.float64)
    m = np.max(illum)
    return illum / m if m > 0 else np.ones(3)


def shades_of_grey(img, p=SOG_P):
    eps = 1e-12
    illum = np.zeros(3, dtype=np.float64)
    for c in range(3):
        channel = img[:, :, c].flatten().astype(np.float64)
        if p == "inf":
            norm = np.max(channel) + eps
        else:
            norm = np.power(np.mean(np.power(channel + eps, p), dtype=np.float64),
                            1.0 / p)
        illum[c] = norm
    m = np.max(illum)
    return illum / m if m > 0 else np.ones(3)


def white_patch(img, percentile=99):
    illum = np.array([
        np.percentile(img[:, :, c].flatten(), percentile, interpolation="linear")
        for c in range(3)
    ], dtype=np.float64)
    m = np.max(illum)
    return illum / m if m > 0 else np.ones(3)


def max_rgb(img, percentile=95):
    illum = np.array([
        np.percentile(img[:, :, c].flatten(), percentile, interpolation="linear")
        for c in range(3)
    ], dtype=np.float64)
    m = np.max(illum)
    return illum / m if m > 0 else np.ones(3)


def general_gray_world(img, sigma=0.3):
    h, w = img.shape[:2]
    y, x = np.mgrid[0:h, 0:w].astype(np.float64)
    sigma_px = sigma * min(h, w)
    weight = np.exp(-((x - w / 2.0) ** 2 + (y - h / 2.0) ** 2)
                    / (2 * sigma_px ** 2))
    illum = np.array([
        np.mean(img[:, :, c] * weight, dtype=np.float64) for c in range(3)
    ], dtype=np.float64)
    m = np.max(illum)
    return illum / m if m > 0 else np.ones(3)


def bayesian(img):
    """Gray-world blended toward a daylight prior; labels nearest known illuminant."""
    gw = gray_world(img)
    dists = {name: float(np.linalg.norm(gw - vec))
             for name, vec in COMMON_ILLUMINANTS.items()}
    nearest = min(dists, key=dists.get)
    blended = ((1 - BAYESIAN_PRIOR_WEIGHT) * gw
               + BAYESIAN_PRIOR_WEIGHT * COMMON_ILLUMINANTS["D65"])
    m = np.max(blended)
    return (blended / m if m > 0 else np.ones(3)), nearest


ESTIMATORS = {
    "grayworld":        lambda img: (gray_world(img), None),
    "shadesofgrey":     lambda img: (shades_of_grey(img), None),
    "whitepatch":       lambda img: (white_patch(img), None),
    "maxrgb":           lambda img: (max_rgb(img), None),
    "generalgrayworld": lambda img: (general_gray_world(img), None),
    "bayesian":         bayesian,
}


def analyze(path, area_spec, algorithm="grayworld"):
    estimator = ESTIMATORS.get(algorithm)
    if estimator is None:
        raise ValueError("unsupported algorithm: %s" % algorithm)

    with rawpy.imread(path) as raw:
        # Post-process to linear RGB, no WB applied, camera-native space.
        rgb = raw.postprocess(
            gamma=(1, 1),
            no_auto_bright=True,
            output_bps=16,
            use_camera_wb=False,
            use_auto_wb=False,
            output_color=rawpy.ColorSpace.raw,
        )

    h, w = rgb.shape[:2]
    y1, y2, x1, x2 = parse_area(area_spec, h, w)
    sample = rgb[y1:y2, x1:x2].astype(np.float64)

    # Illuminant estimate via the requested method (normalized RGB vector +
    # optional nearest-known-illuminant label from the Bayesian variant).
    illum, nearest = estimator(sample)

    # Chromaticity is scale-invariant; convert normalized RGB -> xy.
    xyz = sRGB_to_XYZ(illum)
    xy = XYZ_to_xy(xyz)

    # CCT + tint via Robertson 1968 interpolation.
    cct, tint = xy_to_CCT_Robertson1968(xy)

    # D50-adapted hue/chroma for darktable's Color Calibration module.
    dx = float(xy[0]) - D50_X
    dy = float(xy[1]) - D50_Y
    chroma = math.sqrt(dx * dx + dy * dy)
    hue = math.atan2(dy, dx)

    # Confidence: inverse of spatial variance in the sample (flat scene = high).
    var = np.var(sample, axis=(0, 1)).mean()
    confidence = max(0.0, min(1.0, 1.0 - (var / 65535.0)))

    result = {
        "cct": float(cct),
        "tint": float(tint),
        "xy": [float(xy[0]), float(xy[1])],
        "hue": float(hue),
        "chroma": float(chroma),
        "confidence": float(confidence),
        "algorithm": algorithm,
    }
    if nearest:
        result["nearest_illuminant"] = nearest
    return result


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print(json.dumps({"error": "usage: cct_analyze.py <raw_path> <area_spec> [algorithm]"}))
        sys.exit(1)
    algo = sys.argv[3].lower().replace("rawpy_", "") if len(sys.argv) > 3 else "grayworld"
    try:
        result = analyze(sys.argv[1], sys.argv[2], algo)
        print(json.dumps(result))
    except Exception as e:
        print(json.dumps({"error": str(e)}))
        sys.exit(1)

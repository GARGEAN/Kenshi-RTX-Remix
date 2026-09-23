#!/usr/bin/env python3
"""Decode and analyze the DDS images exported by RTX Remix's AssetExporter."""

from __future__ import annotations

import argparse
import json
import math
import struct
from pathlib import Path

import numpy as np
from PIL import Image


DDS_MAGIC = b"DDS "
DDS_DX10 = b"DX10"
DDS_GLI1 = b"GLI1"
DXGI_R8G8B8A8_UNORM = 28
DXGI_R32_UINT = 42
D3DFMT_A16B16G16R16F = 113
D3DFMT_R32F = 114


def _read_header(path: Path) -> tuple[bytes, int, int, int, int | None]:
    blob = path.read_bytes()
    if len(blob) < 128 or blob[:4] != DDS_MAGIC:
        raise ValueError(f"{path}: not a DDS file")

    height, width = struct.unpack_from("<II", blob, 12)
    fourcc = blob[84:88]
    header_size = 128
    extended_format: int | None = None
    if fourcc in (DDS_DX10, DDS_GLI1):
        if len(blob) < 148:
            raise ValueError(f"{path}: truncated extended DDS header")
        extended_format = struct.unpack_from("<I", blob, 128)[0]
        header_size = 148
    return blob, width, height, header_size, extended_format


def _decode_rgb10a2(payload: bytes, width: int, height: int) -> np.ndarray:
    packed = np.frombuffer(payload, dtype="<u4", count=width * height).reshape(height, width)
    rgba = np.empty((height, width, 4), dtype=np.float32)
    rgba[..., 0] = (packed & 0x3FF).astype(np.float32) / 1023.0
    rgba[..., 1] = ((packed >> 10) & 0x3FF).astype(np.float32) / 1023.0
    rgba[..., 2] = ((packed >> 20) & 0x3FF).astype(np.float32) / 1023.0
    rgba[..., 3] = ((packed >> 30) & 0x3).astype(np.float32) / 3.0
    return rgba


def _decode_octahedral_normal(payload: bytes, width: int, height: int) -> tuple[np.ndarray, np.ndarray]:
    packed = np.frombuffer(payload, dtype="<u4", count=width * height).reshape(height, width)
    signed = packed.view("<i2").reshape(height, width, 2).astype(np.float32)
    oct_xy = np.maximum(signed / 32767.0, -1.0)
    normal = np.empty((height, width, 3), dtype=np.float32)
    normal[..., :2] = oct_xy
    normal[..., 2] = 1.0 - np.abs(oct_xy[..., 0]) - np.abs(oct_xy[..., 1])
    t = np.maximum(-normal[..., 2], 0.0)
    normal[..., 0] += np.where(normal[..., 0] >= 0.0, -t, t)
    normal[..., 1] += np.where(normal[..., 1] >= 0.0, -t, t)
    lengths = np.linalg.norm(normal, axis=2, keepdims=True)
    normal = np.divide(normal, lengths, out=np.zeros_like(normal), where=lengths > 0.0)
    return normal, packed


def decode_dds(path: Path) -> tuple[np.ndarray, dict[str, object]]:
    blob, width, height, offset, extended_format = _read_header(path)
    fourcc = blob[84:88]
    payload = memoryview(blob)[offset:]
    pixel_count = width * height
    name = path.stem.lower()

    metadata: dict[str, object] = {
        "width": width,
        "height": height,
        "fourcc": fourcc.decode("latin-1").rstrip("\x00"),
        "extendedFormat": extended_format,
        "payloadBytes": len(payload),
    }

    if fourcc == DDS_DX10 and extended_format == DXGI_R8G8B8A8_UNORM:
        expected = pixel_count * 4
        if len(payload) < expected:
            raise ValueError(f"{path}: truncated RGBA8 payload")
        array = np.frombuffer(payload, dtype=np.uint8, count=expected).reshape(height, width, 4)
        metadata["encoding"] = "RGBA8_UNORM"
        return array, metadata

    if fourcc == DDS_DX10 and extended_format == DXGI_R32_UINT:
        expected = pixel_count * 4
        if len(payload) < expected:
            raise ValueError(f"{path}: truncated R32_UINT payload")
        if "normal" in name:
            array, packed = _decode_octahedral_normal(payload, width, height)
            metadata["encoding"] = "R32_UINT_SNORM2X16_OCTAHEDRAL"
            metadata["packedZeroFraction"] = float(np.mean(packed == 0))
            metadata["packedUniqueCount"] = int(np.unique(packed).size)
            return array, metadata
        array = np.frombuffer(payload, dtype="<u4", count=pixel_count).reshape(height, width)
        metadata["encoding"] = "R32_UINT"
        return array, metadata

    if fourcc == DDS_GLI1:
        expected = pixel_count * 4
        if len(payload) < expected:
            raise ValueError(f"{path}: truncated packed GLI payload")
        array = _decode_rgb10a2(payload, width, height)
        metadata["encoding"] = "A2B10G10R10_UNORM_PACK32"
        return array, metadata

    legacy_format = struct.unpack("<I", fourcc)[0]
    if legacy_format == D3DFMT_A16B16G16R16F:
        expected = pixel_count * 8
        if len(payload) < expected:
            raise ValueError(f"{path}: truncated RGBA16F payload")
        array = np.frombuffer(payload, dtype="<f2", count=pixel_count * 4).reshape(height, width, 4).astype(np.float32)
        metadata["encoding"] = "RGBA16_SFLOAT"
        return array, metadata

    if legacy_format == D3DFMT_R32F:
        expected = pixel_count * 4
        if len(payload) < expected:
            raise ValueError(f"{path}: truncated R32F payload")
        array = np.frombuffer(payload, dtype="<f4", count=pixel_count).reshape(height, width)
        metadata["encoding"] = "R32_SFLOAT"
        return array, metadata

    raise ValueError(
        f"{path}: unsupported DDS encoding fourcc={fourcc!r}, extendedFormat={extended_format}"
    )


def _numeric_summary(array: np.ndarray) -> dict[str, object]:
    values = array.astype(np.float64, copy=False)
    finite = np.isfinite(values)
    finite_values = values[finite]
    result: dict[str, object] = {
        "finiteFraction": float(np.mean(finite)),
        "nanCount": int(np.count_nonzero(np.isnan(values))),
        "positiveInfinityCount": int(np.count_nonzero(np.isposinf(values))),
        "negativeInfinityCount": int(np.count_nonzero(np.isneginf(values))),
    }
    if finite_values.size:
        result.update({
            "min": float(np.min(finite_values)),
            "p01": float(np.percentile(finite_values, 1)),
            "p50": float(np.percentile(finite_values, 50)),
            "p99": float(np.percentile(finite_values, 99)),
            "max": float(np.max(finite_values)),
            "mean": float(np.mean(finite_values)),
            "zeroFraction": float(np.mean(finite_values == 0.0)),
            "negativeFraction": float(np.mean(finite_values < 0.0)),
        })
    return result


def summarize(array: np.ndarray, metadata: dict[str, object]) -> dict[str, object]:
    result = dict(metadata)
    result["values"] = _numeric_summary(array)

    if array.ndim == 3 and array.shape[2] >= 3:
        rgb = array[..., :3].astype(np.float64)
        finite_rgb = np.all(np.isfinite(rgb), axis=2)
        max_rgb = np.max(np.where(np.isfinite(rgb), rgb, -np.inf), axis=2)
        result["rgb"] = {
            "fullyFinitePixelFraction": float(np.mean(finite_rgb)),
            "blackPixelFractionLe1e-4": float(np.mean(max_rgb <= 1.0e-4)),
            "darkPixelFractionLe1e-2": float(np.mean(max_rgb <= 1.0e-2)),
            "fireflyPixelFractionGt10": float(np.mean(max_rgb > 10.0)),
            "extremePixelFractionGt1000": float(np.mean(max_rgb > 1000.0)),
        }
        if metadata.get("encoding") == "R32_UINT_SNORM2X16_OCTAHEDRAL":
            lengths = np.linalg.norm(array, axis=2)
            result["normal"] = {
                "meanLength": float(np.mean(lengths)),
                "invalidLengthFraction": float(np.mean(np.abs(lengths - 1.0) > 1.0e-3)),
                "meanXYZ": [float(v) for v in np.mean(array, axis=(0, 1))],
            }
    return result


def _linear_to_srgb(linear: np.ndarray) -> np.ndarray:
    linear = np.clip(linear, 0.0, 1.0)
    return np.where(linear <= 0.0031308, linear * 12.92, 1.055 * np.power(linear, 1.0 / 2.4) - 0.055)


def make_visualization(array: np.ndarray, metadata: dict[str, object]) -> Image.Image:
    encoding = metadata["encoding"]
    if encoding == "RGBA8_UNORM":
        return Image.fromarray(array[..., :3], mode="RGB")

    if encoding == "R32_UINT_SNORM2X16_OCTAHEDRAL":
        rgb = array * 0.5 + 0.5
    elif array.ndim == 2:
        values = array.astype(np.float64)
        finite_positive = values[np.isfinite(values) & (values > 0.0)]
        if finite_positive.size:
            lo = max(float(np.percentile(finite_positive, 1)), np.finfo(np.float32).tiny)
            hi = max(float(np.percentile(finite_positive, 99)), lo * 1.0001)
            normalized = (np.log(np.maximum(values, lo)) - math.log(lo)) / (math.log(hi) - math.log(lo))
            rgb = np.repeat(np.clip(normalized[..., None], 0.0, 1.0), 3, axis=2)
        else:
            rgb = np.zeros((*values.shape, 3), dtype=np.float64)
    else:
        rgb = np.nan_to_num(array[..., :3].astype(np.float64), nan=0.0, posinf=0.0, neginf=0.0)
        rgb = np.maximum(rgb, 0.0)
        luminance = rgb[..., 0] * 0.2126 + rgb[..., 1] * 0.7152 + rgb[..., 2] * 0.0722
        positive = luminance[luminance > 1.0e-6]
        if positive.size:
            exposure = 0.8 / max(float(np.percentile(positive, 90)), 1.0e-6)
            exposure = min(exposure, 256.0)
        else:
            exposure = 1.0
        rgb = (rgb * exposure) / (1.0 + rgb * exposure)
        rgb = _linear_to_srgb(rgb)

    pixels = np.rint(np.clip(rgb, 0.0, 1.0) * 255.0).astype(np.uint8)
    return Image.fromarray(pixels, mode="RGB")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="Directory containing RTX Remix DDS screenshots")
    parser.add_argument("--output", type=Path, help="PNG/statistics output directory")
    args = parser.parse_args()

    input_dir = args.input.resolve()
    output_dir = (args.output or input_dir / "decoded").resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    report: dict[str, object] = {}
    for path in sorted(input_dir.glob("*.dds")):
        array, metadata = decode_dds(path)
        report[path.name] = summarize(array, metadata)
        make_visualization(array, metadata).save(output_dir / f"{path.stem}.png")

    report_path = output_dir / "remix_screenshot_analysis.json"
    report_path.write_text(json.dumps(report, indent=2, sort_keys=True), encoding="utf-8")
    print(report_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

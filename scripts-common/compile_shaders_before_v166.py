#!/usr/bin/env python3
# DX11_V165_REAL_SHADER_INCLUDE_TREE
# Real shader compiler wrapper for Remix 1.5 Slang module layout.
# No placeholder shaders and no fake output. This wrapper preserves all real
# include/header/data files and skips only shared .slang modules as standalone
# compile entries.

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
ORIG = SCRIPT_DIR / "compile_shaders_orig_v165.py"
if not ORIG.is_file():
    fallback = SCRIPT_DIR / "compile_shaders_orig_v165.py"
    if fallback.is_file():
        ORIG = fallback

STAGE_EXTS = {
    ".vert", ".frag", ".comp", ".geom", ".tesc", ".tese",
    ".rgen", ".rchit", ".rahit", ".rmiss", ".rcall",
    ".mesh", ".task"
}

STAGE_NAME_TOKENS = (
    ".vert.", ".frag.", ".comp.", ".geom.", ".tesc.", ".tese.",
    ".rgen.", ".rchit.", ".rahit.", ".rmiss.", ".rcall.",
    ".mesh.", ".task.",
    "_vs", "_ps", "_fs", "_cs", "_gs", "_ms", "_ts",
    "vertex", "pixel", "fragment", "compute",
    "raygen", "closesthit", "anyhit", "miss", "callable",
)

REAL_INCLUDE_SUFFIXES = {
    ".h", ".hh", ".hpp", ".hlsli", ".inc", ".ush", ".glslh",
    ".slangh", ".json", ".txt"
}

def _find_arg(args: list[str], name: str) -> int:
    try:
        return args.index(name)
    except ValueError:
        return -1

def _is_entry_shader(path: Path) -> bool:
    lower_name = path.name.lower()
    lower_stem = path.stem.lower()
    suffix = path.suffix.lower()

    if suffix in STAGE_EXTS:
        return True

    if suffix == ".slang":
        joined = lower_name
        return any(tok in joined or tok in lower_stem for tok in STAGE_NAME_TOKENS)

    if suffix in {".hlsl", ".glsl"}:
        return True

    return False

def _should_copy_non_entry(path: Path) -> bool:
    suffix = path.suffix.lower()
    if suffix in REAL_INCLUDE_SUFFIXES:
        return True

    # Keep extensionless include/data files too. Shader trees sometimes include
    # generated binding files or tables without a conventional extension.
    if suffix == "":
        return True

    return False

def _copy_filtered_tree(src: Path, dst: Path) -> tuple[int, int, list[str]]:
    entries = 0
    includes = 0
    skipped_slang: list[str] = []

    for f in src.rglob("*"):
        if not f.is_file():
            continue

        rel = f.relative_to(src)
        out = dst / rel
        suffix = f.suffix.lower()

        if _is_entry_shader(f):
            out.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(f, out)
            entries += 1
            continue

        if suffix == ".slang":
            skipped_slang.append(str(rel).replace("\\", "/"))
            continue

        if _should_copy_non_entry(f):
            out.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(f, out)
            includes += 1

    return entries, includes, skipped_slang

def main() -> int:
    if not ORIG.is_file():
        print(f"DX11_V165_REAL_SHADER_INCLUDE_TREE: original compiler missing: {ORIG}", file=sys.stderr)
        return 2

    args = sys.argv[1:]
    input_i = _find_arg(args, "-input")
    output_i = _find_arg(args, "-output")

    if input_i < 0 or input_i + 1 >= len(args):
        return subprocess.call([sys.executable, str(ORIG)] + args)

    input_dir = Path(args[input_i + 1]).resolve()
    if not input_dir.is_dir():
        return subprocess.call([sys.executable, str(ORIG)] + args)

    output_dir = None
    if output_i >= 0 and output_i + 1 < len(args):
        output_dir = Path(args[output_i + 1]).resolve()

    with tempfile.TemporaryDirectory(prefix="dx11_v165_shader_entries_") as td:
        filtered = Path(td) / "input"
        filtered.mkdir(parents=True, exist_ok=True)

        entries, includes, skipped = _copy_filtered_tree(input_dir, filtered)

        if output_dir is not None:
            output_dir.mkdir(parents=True, exist_ok=True)
            report = output_dir / "dx11_v165_skipped_slang_modules_and_copied_headers.txt"
            report.write_text(
                "DX11_V165_REAL_SHADER_INCLUDE_TREE\n"
                f"OriginalInput={input_dir}\n"
                f"FilteredInput={filtered}\n"
                f"EntryFilesCopied={entries}\n"
                f"RealIncludeFilesCopied={includes}\n"
                f"SharedSlangModulesSkipped={len(skipped)}\n\n"
                + "\n".join(skipped)
                + ("\n" if skipped else ""),
                encoding="utf-8"
            )

        if entries == 0:
            return subprocess.call([sys.executable, str(ORIG)] + args)

        new_args = list(args)
        new_args[input_i + 1] = str(filtered)

        # Keep both temp tree and original tree available to include/import lookup.
        existing_includes = {new_args[i + 1] for i, a in enumerate(new_args[:-1]) if a == "-include"}
        for inc in (filtered, input_dir, input_dir.parent):
            s = str(inc)
            if s not in existing_includes:
                new_args.extend(["-include", s])
                existing_includes.add(s)

        print(
            "DX11_V165_REAL_SHADER_INCLUDE_TREE: compiling filtered shader entry tree "
            f"entries={entries} real_includes={includes} skipped_slang_modules={len(skipped)} "
            f"original={input_dir}",
            flush=True
        )

        return subprocess.call([sys.executable, str(ORIG)] + new_args)

if __name__ == "__main__":
    raise SystemExit(main())
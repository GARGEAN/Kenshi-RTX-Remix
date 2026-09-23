#!/usr/bin/env python3
# DX11_V160_SLANG_MODULE_FILTER
# Real shader compiler wrapper for Remix 1.5 Slang module layout.
# This does not generate fake shader output. It filters shared .slang modules
# out of the standalone compile list, then invokes the original compiler.

from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
ORIG = SCRIPT_DIR / "compile_shaders_orig_v160.py"

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
        if any(tok in joined or tok in lower_stem for tok in STAGE_NAME_TOKENS):
            return True
        return False

    if suffix in {".hlsl", ".glsl"}:
        return True

    return False

def _copy_filtered_tree(src: Path, dst: Path) -> tuple[int, list[str]]:
    copied = 0
    skipped: list[str] = []
    for f in src.rglob("*"):
        if not f.is_file():
            continue
        rel = f.relative_to(src)
        if _is_entry_shader(f):
            out = dst / rel
            out.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(f, out)
            copied += 1
        elif f.suffix.lower() == ".slang":
            skipped.append(str(rel).replace("\\", "/"))
    return copied, skipped

def main() -> int:
    if not ORIG.is_file():
        print(f"DX11_V160_SLANG_MODULE_FILTER: original compiler missing: {ORIG}", file=sys.stderr)
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

    with tempfile.TemporaryDirectory(prefix="dx11_v160_shader_entries_") as td:
        filtered = Path(td) / "input"
        filtered.mkdir(parents=True, exist_ok=True)

        copied, skipped = _copy_filtered_tree(input_dir, filtered)

        if output_dir is not None:
            output_dir.mkdir(parents=True, exist_ok=True)
            report = output_dir / "dx11_v160_skipped_slang_modules.txt"
            report.write_text(
                "DX11_V160_SLANG_MODULE_FILTER\n"
                f"OriginalInput={input_dir}\n"
                f"FilteredInput={filtered}\n"
                f"EntryFilesCopied={copied}\n"
                f"SharedSlangModulesSkipped={len(skipped)}\n\n"
                + "\n".join(skipped)
                + ("\n" if skipped else ""),
                encoding="utf-8"
            )

        if copied == 0:
            return subprocess.call([sys.executable, str(ORIG)] + args)

        new_args = list(args)
        new_args[input_i + 1] = str(filtered)

        if "-include" not in new_args or str(input_dir) not in new_args:
            new_args.extend(["-include", str(input_dir)])

        print(
            "DX11_V160_SLANG_MODULE_FILTER: compiling filtered shader entry tree "
            f"entries={copied} skipped_slang_modules={len(skipped)} original={input_dir}",
            flush=True
        )

        return subprocess.call([sys.executable, str(ORIG)] + new_args)

if __name__ == "__main__":
    raise SystemExit(main())
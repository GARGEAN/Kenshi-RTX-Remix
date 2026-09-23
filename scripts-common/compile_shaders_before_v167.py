# DX11_V167_SHADER_TEMP_CLEANUP_SAFE
#!/usr/bin/env python3
# DX11_V166_FULL_SHADER_TREE_HIDDEN_MODULES
# Real shader compiler wrapper for Remix 1.5.
# No placeholder shaders and no fake output.
# Copies the full real shader tree so relative includes/imports work, copies
# real src/dxvk/rtx_render headers next to the temp input root, and hides only
# shared .slang modules from the Python compile-script enumeration.

from __future__ import annotations

import glob as _glob
import os
import runpy
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
ORIG = SCRIPT_DIR / "compile_shaders_orig_v166.py"
for fallback_name in ("compile_shaders_orig_v166.py", "compile_shaders_orig_v166.py", "compile_shaders_orig_v166.py"):
    if ORIG.is_file():
        break
    fb = SCRIPT_DIR / fallback_name
    if fb.is_file():
        ORIG = fb

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

HEADER_SUFFIXES = {".h", ".hh", ".hpp", ".hlsli", ".inc", ".ush", ".glslh", ".slangh"}

HIDDEN_SHARED_MODULES: set[str] = set()


def _norm(p: os.PathLike[str] | str) -> str:
    try:
        return str(Path(p).resolve()).lower()
    except Exception:
        return os.path.abspath(os.fspath(p)).lower()


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
        return any(tok in lower_name or tok in lower_stem for tok in STAGE_NAME_TOKENS)
    if suffix in {".hlsl", ".glsl"}:
        return True
    return False


def _copy_full_tree_and_find_modules(src: Path, dst: Path) -> tuple[int, int, list[str]]:
    entries = 0
    headers = 0
    modules: list[str] = []
    shutil.copytree(src, dst, dirs_exist_ok=True)
    for f in dst.rglob("*"):
        if not f.is_file():
            continue
        rel = f.relative_to(dst)
        if _is_entry_shader(f):
            entries += 1
        elif f.suffix.lower() == ".slang":
            modules.append(str(rel).replace("\\", "/"))
            HIDDEN_SHARED_MODULES.add(_norm(f))
        elif f.suffix.lower() in HEADER_SUFFIXES:
            headers += 1
    return entries, headers, modules


def _copy_rtx_render_headers(original_input: Path, temp_root: Path) -> tuple[int, Path | None]:
    # Original input is src/dxvk/shaders/rtx. Some shader headers include:
    #   ../../../../rtx_render/...
    # From pass/terrain_baking that resolves to src/dxvk/rtx_render. In the
    # temp tree the same relative include resolves to temp_root/rtx_render.
    src_dxvk = original_input.parent.parent
    src_rtx_render = src_dxvk / "rtx_render"
    if not src_rtx_render.is_dir():
        return 0, None
    dst = temp_root / "rtx_render"
    copied = 0
    for f in src_rtx_render.rglob("*"):
        if not f.is_file():
            continue
        if f.suffix.lower() not in HEADER_SUFFIXES and f.suffix.lower() not in {".txt", ".json", ""}:
            continue
        rel = f.relative_to(src_rtx_render)
        out = dst / rel
        out.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(f, out)
        copied += 1
    return copied, dst


def _install_hidden_module_hooks() -> None:
    real_os_walk = os.walk
    real_glob = _glob.glob
    real_iglob = _glob.iglob
    real_path_glob = Path.glob
    real_path_rglob = Path.rglob

    def visible(path: os.PathLike[str] | str) -> bool:
        return _norm(path) not in HIDDEN_SHARED_MODULES

    def walk(top, *args, **kwargs):
        for root, dirs, files in real_os_walk(top, *args, **kwargs):
            files[:] = [f for f in files if visible(Path(root) / f)]
            yield root, dirs, files

    def glob_func(pathname, *args, **kwargs):
        return [p for p in real_glob(pathname, *args, **kwargs) if visible(p)]

    def iglob_func(pathname, *args, **kwargs):
        for p in real_iglob(pathname, *args, **kwargs):
            if visible(p):
                yield p

    def path_glob(self, pattern):
        for p in real_path_glob(self, pattern):
            if visible(p):
                yield p

    def path_rglob(self, pattern):
        for p in real_path_rglob(self, pattern):
            if visible(p):
                yield p

    os.walk = walk  # type: ignore[assignment]
    _glob.glob = glob_func  # type: ignore[assignment]
    _glob.iglob = iglob_func  # type: ignore[assignment]
    Path.glob = path_glob  # type: ignore[assignment]
    Path.rglob = path_rglob  # type: ignore[assignment]


def _run_original_in_process(args: list[str]) -> int:
    old_argv = sys.argv[:]
    try:
        sys.argv = [str(ORIG)] + args
        try:
            runpy.run_path(str(ORIG), run_name="__main__")
            return 0
        except SystemExit as e:
            code = e.code
            if code is None:
                return 0
            if isinstance(code, int):
                return code
            return 1
    finally:
        sys.argv = old_argv


def main() -> int:
    if not ORIG.is_file():
        print(f"DX11_V166_FULL_SHADER_TREE_HIDDEN_MODULES: original compiler missing: {ORIG}", file=sys.stderr)
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

    with tempfile.TemporaryDirectory(prefix="dx11_v167_shader_fulltree_", ignore_cleanup_errors=True) as td:
        temp_root = Path(td)
        temp_input = temp_root / "input"
        temp_input.mkdir(parents=True, exist_ok=True)

        entries, headers, modules = _copy_full_tree_and_find_modules(input_dir, temp_input)
        rtx_headers, rtx_dst = _copy_rtx_render_headers(input_dir, temp_root)

        if output_dir is not None:
            output_dir.mkdir(parents=True, exist_ok=True)
            report = output_dir / "dx11_v166_full_shader_tree_hidden_modules.txt"
            report.write_text(
                "DX11_V166_FULL_SHADER_TREE_HIDDEN_MODULES\n"
                f"OriginalInput={input_dir}\n"
                f"TempInput={temp_input}\n"
                f"EntryFilesVisibleToCompiler={entries}\n"
                f"RealShaderHeadersCopied={headers}\n"
                f"RealRtxRenderHeadersCopied={rtx_headers}\n"
                f"RtxRenderHeaderTempRoot={rtx_dst}\n"
                f"SharedSlangModulesHiddenFromEnumeration={len(modules)}\n\n"
                + "\n".join(modules)
                + ("\n" if modules else ""),
                encoding="utf-8"
            )

        if entries == 0:
            return subprocess.call([sys.executable, str(ORIG)] + args)

        new_args = list(args)
        new_args[input_i + 1] = str(temp_input)

        existing_includes = {new_args[i + 1] for i, a in enumerate(new_args[:-1]) if a == "-include"}
        for inc in (temp_input, input_dir, input_dir.parent, temp_root, temp_root / "rtx_render"):
            s = str(inc)
            if s not in existing_includes:
                new_args.extend(["-include", s])
                existing_includes.add(s)

        print(
            "DX11_V166_FULL_SHADER_TREE_HIDDEN_MODULES: compiling full shader tree with shared modules hidden "
            f"entries={entries} shader_headers={headers} rtx_render_headers={rtx_headers} "
            f"hidden_slang_modules={len(modules)} original={input_dir}",
            flush=True
        )

        _install_hidden_module_hooks()
        return _run_original_in_process(new_args)

if __name__ == "__main__":
    raise SystemExit(main())
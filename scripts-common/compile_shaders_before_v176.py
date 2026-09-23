#!/usr/bin/env python3
# DX11_V174_PY314_SAFE_SHADER_WRAPPER
# Real shader compiler wrapper for the DX11 fork.
# No placeholder shaders and no fake output.

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

HEADER_SUFFIXES = {".h", ".hh", ".hpp", ".hlsli", ".inc", ".ush", ".glslh", ".slangh", ".json", ".txt", ""}
HIDDEN_SHARED_MODULES: set[str] = set()

DX11_DEFINES = [
    "-DDXVK_REMIX_DX11_SHADER_MODE=1",
    "-DRTX_REMIX_DX11=1",
]

def _read_start(path: Path, limit: int = 65536) -> str:
    try:
        with path.open("r", encoding="utf-8", errors="ignore") as f:
            return f.read(limit)
    except Exception:
        return ""

def _is_clean_original(path: Path) -> bool:
    if not path.is_file():
        return False
    text = _read_start(path)
    if "DX11_V" in text:
        return False
    if "HIDDEN_SHARED_MODULES" in text:
        return False
    if "_install_hidden_module_hooks" in text:
        return False
    if "compile_shaders_orig_v" in text:
        return False
    return "argparse" in text or "slangc" in text or "glslang" in text

def _select_original() -> Path | None:
    candidates = [
        SCRIPT_DIR / "compile_shaders_real_original_v174.py",
        SCRIPT_DIR / "compile_shaders_orig_clean_v174.py",
        SCRIPT_DIR / "compile_shaders_orig_v160.py",
        SCRIPT_DIR / "compile_shaders_orig_v161.py",
        SCRIPT_DIR / "compile_shaders_before_v160.py",
        SCRIPT_DIR / "compile_shaders_before_v161.py",
        SCRIPT_DIR.parent / "_nvidia_dxvk_remix_for_dx11_bridge" / "scripts-common" / "compile_shaders.py",
        SCRIPT_DIR.parent / "_upstream_dxvk_remix_1_5_full_runtime_dx11" / "scripts-common" / "compile_shaders.py",
        SCRIPT_DIR.parent / "_upstream_dxvk_remix_1_5_runtime_ui" / "scripts-common" / "compile_shaders.py",
    ]
    for c in candidates:
        if _is_clean_original(c):
            return c
    return None

def _norm(p: os.PathLike[str] | str) -> str:
    try:
        return str(Path(p).resolve()).lower()
    except Exception:
        return os.path.abspath(os.fspath(p)).lower()

def _is_old_api_path(path: Path) -> bool:
    s = str(path).replace("\\", "/").lower()
    name = path.name.lower()
    return (
        "/d3d9/" in s
        or "/dx9/" in s
        or "d3d9_" in name
        or "_d3d9" in name
        or "dx9_" in name
        or "_dx9" in name
    )

def _find_arg(args: list[str], name: str) -> int:
    try:
        return args.index(name)
    except ValueError:
        return -1

def _is_entry_shader(path: Path) -> bool:
    if _is_old_api_path(path):
        return False
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

def _copy_full_tree_and_find_modules(src: Path, dst: Path) -> tuple[int, int, int, list[str]]:
    entries = 0
    headers = 0
    old_api_skipped = 0
    modules: list[str] = []

    for f in src.rglob("*"):
        if not f.is_file():
            continue
        rel = f.relative_to(src)
        if _is_old_api_path(f):
            old_api_skipped += 1
            continue
        out = dst / rel
        out.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(f, out)

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

    return entries, headers, old_api_skipped, modules

def _copy_rtx_render_headers(original_input: Path, temp_root: Path) -> tuple[int, list[Path]]:
    src_dxvk = original_input.parent.parent
    src_rtx_render = src_dxvk / "rtx_render"
    if not src_rtx_render.is_dir():
        return 0, []

    destinations = [
        temp_root / "rtx_render",
        temp_root.parent / "rtx_render",
        temp_root / "input" / "rtx_render",
    ]

    copied = 0
    used: list[Path] = []
    for dst in destinations:
        dst.mkdir(parents=True, exist_ok=True)
        used.append(dst)
        for f in src_rtx_render.rglob("*"):
            if not f.is_file():
                continue
            if _is_old_api_path(f):
                continue
            if f.suffix.lower() not in HEADER_SUFFIXES:
                continue
            rel = f.relative_to(src_rtx_render)
            out = dst / rel
            out.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(f, out)
            copied += 1
    return copied, used

def _install_hidden_module_hooks() -> None:
    real_os_walk = os.walk
    real_glob = _glob.glob
    real_iglob = _glob.iglob
    real_path_glob = Path.glob
    real_path_rglob = Path.rglob
    real_subprocess_call = subprocess.call
    real_subprocess_run = subprocess.run
    real_subprocess_check_call = subprocess.check_call
    real_popen = subprocess.Popen

    def visible(path: os.PathLike[str] | str) -> bool:
        p = Path(path)
        return _norm(path) not in HIDDEN_SHARED_MODULES and not _is_old_api_path(p)

    def add_dx11_defines(cmd):
        try:
            if isinstance(cmd, (list, tuple)) and cmd:
                exe = str(cmd[0]).lower()
                if "slangc" in exe:
                    out = list(cmd)
                    for d in DX11_DEFINES:
                        if d not in out:
                            out.append(d)
                    return out
            elif isinstance(cmd, str) and "slangc" in cmd.lower():
                extra = " " + " ".join(DX11_DEFINES)
                if "DXVK_REMIX_DX11_SHADER_MODE" not in cmd:
                    return cmd + extra
        except Exception:
            pass
        return cmd

    def walk(top, *args, **kwargs):
        for root, dirs, files in real_os_walk(top, *args, **kwargs):
            dirs[:] = [d for d in dirs if visible(Path(root) / d)]
            files[:] = [f for f in files if visible(Path(root) / f)]
            yield root, dirs, files

    def glob_func(pathname, *args, **kwargs):
        return [p for p in real_glob(pathname, *args, **kwargs) if visible(p)]

    def iglob_func(pathname, *args, **kwargs):
        for p in real_iglob(pathname, *args, **kwargs):
            if visible(p):
                yield p

    def path_glob(self, pattern, *args, **kwargs):
        for p in real_path_glob(self, pattern, *args, **kwargs):
            if visible(p):
                yield p

    def path_rglob(self, pattern, *args, **kwargs):
        for p in real_path_rglob(self, pattern, *args, **kwargs):
            if visible(p):
                yield p

    def call(cmd, *args, **kwargs):
        return real_subprocess_call(add_dx11_defines(cmd), *args, **kwargs)

    def run(cmd, *args, **kwargs):
        return real_subprocess_run(add_dx11_defines(cmd), *args, **kwargs)

    def check_call(cmd, *args, **kwargs):
        return real_subprocess_check_call(add_dx11_defines(cmd), *args, **kwargs)

    def popen(cmd, *args, **kwargs):
        return real_popen(add_dx11_defines(cmd), *args, **kwargs)

    os.walk = walk  # type: ignore[assignment]
    _glob.glob = glob_func  # type: ignore[assignment]
    _glob.iglob = iglob_func  # type: ignore[assignment]
    Path.glob = path_glob  # type: ignore[assignment]
    Path.rglob = path_rglob  # type: ignore[assignment]
    subprocess.call = call  # type: ignore[assignment]
    subprocess.run = run  # type: ignore[assignment]
    subprocess.check_call = check_call  # type: ignore[assignment]
    subprocess.Popen = popen  # type: ignore[assignment]

def _run_original_in_process(orig: Path, args: list[str]) -> int:
    old_argv = sys.argv[:]
    old_env = dict(os.environ)
    try:
        os.environ["DXVK_REMIX_DX11_SHADER_MODE"] = "1"
        os.environ["RTX_REMIX_DX11"] = "1"
        sys.argv = [str(orig)] + args
        try:
            runpy.run_path(str(orig), run_name="__main__")
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
        os.environ.clear()
        os.environ.update(old_env)

def main() -> int:
    orig = _select_original()
    if orig is None:
        print("DX11_V174_PY314_SAFE_SHADER_WRAPPER: no clean original compile_shaders.py found. No fake shader output was generated.", file=sys.stderr)
        return 2

    args = sys.argv[1:]
    input_i = _find_arg(args, "-input")
    output_i = _find_arg(args, "-output")

    if input_i < 0 or input_i + 1 >= len(args):
        return subprocess.call([sys.executable, str(orig)] + args)

    input_dir = Path(args[input_i + 1]).resolve()
    if not input_dir.is_dir():
        return subprocess.call([sys.executable, str(orig)] + args)

    output_dir = None
    if output_i >= 0 and output_i + 1 < len(args):
        output_dir = Path(args[output_i + 1]).resolve()

    with tempfile.TemporaryDirectory(prefix="dx11_v174_shader_fulltree_", ignore_cleanup_errors=True) as td:
        temp_root = Path(td)
        temp_input = temp_root / "input"
        temp_input.mkdir(parents=True, exist_ok=True)

        entries, headers, old_api_skipped, modules = _copy_full_tree_and_find_modules(input_dir, temp_input)
        rtx_headers, rtx_dsts = _copy_rtx_render_headers(input_dir, temp_root)

        if output_dir is not None:
            output_dir.mkdir(parents=True, exist_ok=True)
            report = output_dir / "dx11_v174_py314_safe_shader_wrapper_report.txt"
            report.write_text(
                "DX11_V174_PY314_SAFE_SHADER_WRAPPER\n"
                "No placeholder shaders. No fake output.\n"
                f"OriginalCompiler={orig}\n"
                f"OriginalInput={input_dir}\n"
                f"TempInput={temp_input}\n"
                f"EntryFilesVisibleToCompiler={entries}\n"
                f"RealShaderHeadersCopied={headers}\n"
                f"RealRtxRenderHeadersCopied={rtx_headers}\n"
                f"OldApiPathsSkipped={old_api_skipped}\n"
                f"SharedSlangModulesHiddenFromEnumeration={len(modules)}\n\n"
                + "\n".join(modules)
                + ("\n" if modules else ""),
                encoding="utf-8"
            )

        if entries == 0:
            return subprocess.call([sys.executable, str(orig)] + args)

        new_args = list(args)
        new_args[input_i + 1] = str(temp_input)

        existing_includes = {new_args[i + 1] for i, a in enumerate(new_args[:-1]) if a == "-include"}
        include_roots = [temp_input, input_dir, input_dir.parent, temp_root, temp_root.parent]
        include_roots.extend(rtx_dsts)
        for inc in include_roots:
            s = str(inc)
            if s not in existing_includes:
                new_args.extend(["-include", s])
                existing_includes.add(s)

        print(
            "DX11_V174_PY314_SAFE_SHADER_WRAPPER: compiling real Remix runtime shaders "
            f"entries={entries} headers={headers} rtx_render_headers={rtx_headers} "
            f"old_api_paths_skipped={old_api_skipped} hidden_slang_modules={len(modules)} "
            f"original_compiler={orig}",
            flush=True
        )

        _install_hidden_module_hooks()
        return _run_original_in_process(orig, new_args)

if __name__ == "__main__":
    raise SystemExit(main())
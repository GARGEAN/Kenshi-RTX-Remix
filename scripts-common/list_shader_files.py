#!/usr/bin/env python3
"""List every shader source file under a directory, one absolute path per line.

Meson has no native globbing, and `custom_target` needs an explicit `depend_files`
list to know when to re-run. Without it the shader build declares only its output
stamp, so ninja treats that stamp as up to date forever and never regenerates -
adding or editing a shader silently produces no new SPIR-V, and the C++ side then
fails on a missing `rtx_shaders/<name>.h`.

This is invoked from meson at configure time to produce that dependency list.
"""

import os
import sys

# Everything the shader compiler reads: entry points, modules, and the headers
# they include. A change to any of them can change the generated SPIR-V.
SHADER_SUFFIXES = (
    '.slang',
    '.slangh',
    '.h',
    '.hlsli',
    '.glslh',
    '.comp',
    '.frag',
    '.vert',
    '.geom',
    '.rgen',
    '.rmiss',
    '.rchit',
    '.rahit',
)


def main() -> int:
    if len(sys.argv) < 2:
        print('usage: list_shader_files.py <shader-root> [<shader-root> ...]',
              file=sys.stderr)
        return 1

    seen = set()
    files = []

    for root in sys.argv[1:]:
        if not os.path.isdir(root):
            continue

        for dirpath, dirnames, filenames in os.walk(root):
            # Keep traversal deterministic so the dependency list is stable
            # across configures and does not churn the build graph.
            dirnames.sort()

            for name in sorted(filenames):
                if not name.endswith(SHADER_SUFFIXES):
                    continue

                path = os.path.normpath(os.path.join(dirpath, name))

                if path not in seen:
                    seen.add(path)
                    files.append(path)

    # Meson splits this on newlines; emit nothing rather than a blank entry when
    # no shaders were found, since a stray empty string is a configure error.
    if files:
        print('\n'.join(files))

    return 0


if __name__ == '__main__':
    sys.exit(main())

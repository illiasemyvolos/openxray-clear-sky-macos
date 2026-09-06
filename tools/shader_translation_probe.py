#!/usr/bin/env python3
"""Translate the OpenGL shader tree to Metal Shading Language, offline.

Answers one question for the Vulkan/Metal spike: can the existing hand-written
GLSL survive a SPIR-V round trip, uber-shader #define scheme included, without
editing the shader sources?

It reproduces what src/Layers/xrRenderPC_GL/rgl_shaders.cpp does at runtime -
shader_sources_manager::load_includes for the textual #include walker, and
CRender::shader_compile for the #define preamble - then runs:

    GLSL 410 --(glslang -G)--> OpenGL SPIR-V --(spirv-cross --msl)--> MSL

Note the -G. Vulkan SPIR-V (-V) is rejected by this tree, because the shaders
declare uniforms outside a block (see gl/common_cbuffers.h, which carries an
upstream "TODO: OGL: Use constant buffers"). Moving those into uniform blocks is
the prerequisite for a Vulkan backend; a native Metal backend does not need it.

Usage:
    tools/shader_translation_probe.py [--root res/gamedata/shaders/gl]
                                      [--glslang glslang] [--spirv-cross spirv-cross]
                                      [--out /tmp/shader-probe] [--only NAME]
"""

import argparse
import collections
import os
import re
import subprocess
import sys

STAGES = {".vs": "vert", ".ps": "frag", ".gs": "geom"}

# One plausible variant of the uber-shader options, in the order
# CRender::shader_compile emits them. The engine picks a different combination
# per material, so a single set here cannot compile every shader: see --variant.
VARIANTS = {
    "static_sun": [
        ("SMAP_size", "2048"), ("USE_HWSMAP", "1"), ("USE_HWSMAP_PCF", "1"),
        ("USE_BRANCHING", "1"), ("USE_VTF", "1"), ("USE_R2_STATIC_SUN", "1"),
        ("SKIN_NONE", "1"), ("USE_SOFT_WATER", "1"), ("USE_SOFT_PARTICLES", "1"),
        ("USE_DOF", "1"), ("SUN_SHAFTS_QUALITY", "3"), ("SSAO_QUALITY", "3"),
        ("SUN_QUALITY", "3"), ("GBUFFER_OPTIMIZATION", "1"),
    ],
    "no_static_sun": [
        ("SMAP_size", "2048"), ("USE_HWSMAP", "1"), ("USE_HWSMAP_PCF", "1"),
        ("USE_BRANCHING", "1"), ("USE_VTF", "1"),
        ("SKIN_NONE", "1"), ("USE_SOFT_WATER", "1"), ("USE_SOFT_PARTICLES", "1"),
        ("USE_DOF", "1"), ("SUN_SHAFTS_QUALITY", "3"), ("SSAO_QUALITY", "3"),
        ("SUN_QUALITY", "3"), ("GBUFFER_OPTIMIZATION", "1"),
    ],
    "skinned": [
        ("SMAP_size", "2048"), ("USE_HWSMAP", "1"), ("USE_HWSMAP_PCF", "1"),
        ("USE_BRANCHING", "1"), ("USE_VTF", "1"), ("USE_R2_STATIC_SUN", "1"),
        ("SKIN_2", "1"), ("USE_SOFT_WATER", "1"), ("USE_SOFT_PARTICLES", "1"),
        ("USE_DOF", "1"), ("SUN_SHAFTS_QUALITY", "3"), ("SSAO_QUALITY", "3"),
        ("SUN_QUALITY", "3"), ("GBUFFER_OPTIMIZATION", "1"),
    ],
}


def resolve_ci(path):
    """Case-insensitive lookup, the way APFS resolves it.

    Several shaders include "iostructs\\v_TL.h" while the file on disk is
    v_tl.h. That is invisible on the case-insensitive volume the engine is
    developed on and breaks on any case-sensitive filesystem.
    """
    if os.path.exists(path):
        return path
    directory, base = os.path.split(path)
    if not os.path.isdir(directory):
        return path
    for entry in os.listdir(directory):
        if entry.lower() == base.lower():
            return os.path.join(directory, entry)
    return path


def inline(root, path, depth=0):
    """Splice #include "..." textually, as load_includes does."""
    if depth > 64:
        raise RuntimeError("include depth exceeded at " + path)
    with open(resolve_ci(path), encoding="utf-8", errors="replace") as handle:
        data = handle.read()
    parts, pos = [], 0
    for match in re.finditer(r'#include\s*"([^"]+)"', data):
        parts.append(data[pos:match.start()])
        included = match.group(1).replace("\\", "/")
        parts.append(inline(root, os.path.join(root, included), depth + 1))
        pos = match.end()
    parts.append(data[pos:])
    return "".join(parts)


def preamble(name, defines):
    lines = ["#version 410",
             "#extension GL_ARB_separate_shader_objects : enable",
             "// %s" % name]
    lines += ["#define %s\t%s" % (key, value) for key, value in defines]
    return "\n".join(lines) + "\n"


def translate(args, name, defines):
    stage = STAGES[os.path.splitext(name)[1]]
    source = preamble(name, defines) + inline(args.root, os.path.join(args.root, name))

    glsl_path = os.path.join(args.out, name + ".glsl")
    spv_path = os.path.join(args.out, name + ".spv")
    with open(glsl_path, "w", encoding="utf-8") as handle:
        handle.write(source)
    if os.path.exists(spv_path):
        os.remove(spv_path)

    result = subprocess.run(
        [args.glslang, "-S", stage, "-G", "--auto-map-bindings",
         "--auto-map-locations", "-o", spv_path, glsl_path],
        capture_output=True, text=True)
    if not os.path.exists(spv_path):
        errors = [line for line in (result.stdout + result.stderr).splitlines()
                  if "ERROR" in line]
        return "glslang", errors[0].strip() if errors else "unknown failure"

    result = subprocess.run(
        [args.spirv_cross, "--msl", "--msl-version", "20300", spv_path],
        capture_output=True, text=True)
    if result.returncode != 0:
        errors = result.stderr.strip().splitlines()
        return "spirv-cross", errors[-1] if errors else "unknown failure"

    with open(os.path.join(args.out, name + ".metal"), "w", encoding="utf-8") as handle:
        handle.write(result.stdout)
    return "ok", str(len(result.stdout.splitlines()))


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", default="res/gamedata/shaders/gl")
    parser.add_argument("--glslang", default="glslang")
    parser.add_argument("--spirv-cross", dest="spirv_cross", default="spirv-cross")
    parser.add_argument("--out", default="/tmp/shader-probe")
    parser.add_argument("--variant", default="static_sun", choices=sorted(VARIANTS))
    parser.add_argument("--only", help="translate a single shader by file name")
    args = parser.parse_args()

    os.makedirs(args.out, exist_ok=True)
    defines = VARIANTS[args.variant]

    names = ([args.only] if args.only else
             sorted(n for n in os.listdir(args.root)
                    if os.path.splitext(n)[1] in STAGES))

    outcomes = collections.Counter()
    failures = []
    for name in names:
        try:
            kind, detail = translate(args, name, defines)
        except Exception as error:  # noqa: BLE001 - report, do not abort the sweep
            kind, detail = "harness", str(error)
        outcomes[kind] += 1
        if kind != "ok":
            failures.append((name, kind, detail))

    print("variant: %s   shaders: %d" % (args.variant, len(names)))
    for kind, count in outcomes.most_common():
        print("  %-12s %4d" % (kind, count))
    if failures:
        print()
        for name, kind, detail in failures:
            print("  %-38s %-12s %s" % (name, kind, detail[:100]))
    return 0 if outcomes["ok"] == len(names) else 1


if __name__ == "__main__":
    sys.exit(main())

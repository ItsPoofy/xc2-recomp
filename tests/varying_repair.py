"""Compile synthetic shader pairs, run the actual C pass, and validate its SPIR-V.

Needs a C compiler, glslangValidator and spirv-val. No game files. CC may name an already-built test driver
through --driver instead (useful with MSVC). Outputs stay in ignored build/varying-test/.
"""
import argparse
import os
import struct
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build" / "varying-test"


def run(*args):
    result = subprocess.run([str(a) for a in args], capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(f"{args}: exit {result.returncode}\n{result.stdout}\n{result.stderr}")
    return result


def words(path):
    data = path.read_bytes()
    return list(struct.unpack(f"<{len(data) // 4}I", data))


def instructions(w):
    at = 5
    while at < len(w):
        n, op = w[at] >> 16, w[at] & 65535
        assert n and at + n <= len(w)
        yield op, w[at + 1:at + n]
        at += n


def f32(x):
    return struct.unpack("<f", struct.pack("<f", x))[0]


def evaluate(path, viewport, positions, attributes):
    """Execute the generated SPIR-V arithmetic, not a reimplementation of the repair formula."""
    ops = list(instructions(words(path)))
    decorations = {}
    for op, a in ops:
        if op == 71:
            decorations.setdefault(a[0], {})[a[1]] = a[2:]
    memory, value, types, private = {}, {}, {}, set()
    for op, a in ops:
        if op in (21, 22):
            types[a[0]] = op
        elif op == 43:
            value[a[1]] = struct.unpack("<f", struct.pack("<I", a[2]))[0] if types.get(a[0]) == 22 else a[2]
        elif op == 59 and a[2] != 7:
            var, storage = a[1:3]
            d = decorations.get(var, {})
            if storage == 6:
                private.add(var)
            elif storage == 2 and d.get(33) == [0]:
                memory[var] = [viewport]
            elif storage == 1 and d.get(11) == [15]:
                memory[var] = [viewport[0] + viewport[2] * .6, viewport[1] + viewport[3] * .4, .5, 1.]
            elif storage == 1 and 5285 in d:
                location = d[30][0]
                memory[var] = attributes.get(location, positions)
            value[var] = (var, ())
    generated_stores = 0
    for op, a in ops:
        if op in (65, 66):
            base, indexes = value[a[2]]
            value[a[1]] = base, indexes + tuple(int(value[i]) for i in a[3:])
        elif op == 61:
            var, indexes = value[a[2]]
            if var not in memory:
                continue
            x = memory[var]
            for i in indexes:
                x = x[i]
            value[a[1]] = x
        elif op == 81:
            if a[2] in value:
                x = value[a[2]]
                for i in a[3:]:
                    x = x[i]
                value[a[1]] = x
        elif op in (129, 131, 133, 136):
            x, y = value[a[2]], value[a[3]]
            value[a[1]] = f32({129: lambda: x + y, 131: lambda: x - y, 133: lambda: x * y, 136: lambda: x / y}[op]())
        elif op == 12 and a[3] == 50:
            x, y, z = (value[i] for i in a[4:7])
            value[a[1]] = f32(x * y + z)
        elif op == 80:
            if all(i in value for i in a[2:]):
                value[a[1]] = [value[i] for i in a[2:]]
        elif op == 62 and a[0] in private:
            memory[a[0]] = value[a[1]]
            generated_stores += 1
            if generated_stores == len(private):
                return list(memory[i] for i in sorted(private))
    raise AssertionError("generated interpolation did not initialize its Private inputs")


def oracle(positions, q, attribute):
    # Solve homogeneous barycentric equations with Gaussian elimination, independently of the emitted cross products.
    matrix = [[p[0] - q[0] * p[3] for p in positions] + [0.],
              [p[1] - q[1] * p[3] for p in positions] + [0.], [1., 1., 1., 1.]]
    for col in range(3):
        pivot = max(range(col, 3), key=lambda r: abs(matrix[r][col]))
        matrix[col], matrix[pivot] = matrix[pivot], matrix[col]
        scale = matrix[col][col]
        matrix[col] = [x / scale for x in matrix[col]]
        for row in range(3):
            if row != col:
                scale = matrix[row][col]
                matrix[row] = [x - scale * y for x, y in zip(matrix[row], matrix[col])]
    weights = [row[3] for row in matrix]
    return [sum(weights[i] * attribute[i][c] for i in range(3)) for c in range(4)]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--driver", type=Path)
    parser.add_argument("--glslang", default="glslangValidator")
    parser.add_argument("--validator", default="spirv-val")
    a = parser.parse_args()
    BUILD.mkdir(parents=True, exist_ok=True)
    driver = a.driver or BUILD / ("varying_repair.exe" if os.name == "nt" else "varying_repair")
    if not a.driver:
        run(os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2", "-g",
            ROOT / "tests/varying_repair.c", ROOT / "runtime/vk_varying.c", "-o", driver)
    vertex = """#version 450
layout(location=0) in vec4 position;
layout(location=0) out vec4 colour;
layout(location=1) flat out int material;
void main() { gl_Position = position; colour = position * 0.25; material = gl_VertexIndex; }
"""
    fragment = """#version 450
layout(location=0) in vec4 colour;
layout(location=1) flat in int material;
layout(location=0) out vec4 target;
void main() { target = colour + vec4(float(material) * 0.0); }
"""
    for env in ("vulkan1.1", "vulkan1.3"):
        for stage, source in (("vert", vertex), ("frag", fragment)):
            src = BUILD / f"test.{stage}"
            src.write_text(source)
            run(a.glslang, "-V", "--target-env", env, src, "-o", BUILD / f"{stage}.spv")
        vo, fo = BUILD / "out.vert.spv", BUILD / "out.frag.spv"
        print(run(driver, BUILD / "vert.spv", BUILD / "frag.spv", vo, fo).stdout.strip())
        for out in (vo, fo):
            run(a.validator, "--target-env", env, out)
        f = words(fo)
        assert sum(op == 71 and x[1] == 5285 for op, x in instructions(f)) == 2
        # Offset and negative viewport height, all-positive W, a vertex behind the camera, and a vertex at W=0.
        for viewport in ([0., 720., 1280., -720.], [17., 971., 1920., -1080.], [0., 0., 640., 360.]):
            for positions in ([[-2., -2., 0., 1.], [2., -2., 0., 1.], [0., 2., 0., 2.]],
                              [[-10000., -10000., 0., 1.], [10000., -10000., 0., 1.], [0., 10000., 0., -.5]],
                              [[-2., -2., 0., 1.], [2., -2., 0., 1.], [0., 2., 0., 0.]]):
                values = [[.25, .5, .75, 1.], [.5, 1., 1.5, 2.], [1., 2., 3., 4.]]
                actual = evaluate(fo, viewport, positions, {0: values})[0]
                expected = oracle(positions, [.2, -.2], values)
                assert max(abs(x - y) for x, y in zip(actual, expected)) < 2e-5, (actual, expected)
                constant = [[.375, .375, .375, .375]] * 3
                assert evaluate(fo, viewport, positions, {0: constant})[0] == constant[0]
        # No spare interface slot: the pass must decline without returning partial modules.
        skipped = subprocess.run([str(driver), str(BUILD / "vert.spv"), str(BUILD / "frag.spv"), str(vo), str(fo), "2"], capture_output=True)
        assert skipped.returncode == 3
        print(env, "SPIR-V validation, numerical interpolation and constant preservation passed")


if __name__ == "__main__":
    main()

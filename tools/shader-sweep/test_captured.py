#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later

import argparse
import json
from pathlib import Path
import struct
import subprocess
import tempfile


def serialize_environment(env):
    code = b"".join(struct.pack("<Q", int(word, 16)) for word in env["code"])
    fields = ("texture_types", "texture_formats", "cbuf_values", "storage_pointers", "replacements")
    data = struct.pack("<6Q7I", len(code), *(len(env[field]) for field in fields), *env["parameters"])
    data += code
    for field, fmt in zip(fields, ("<II", "<II", "<QI", "<QB", "<QI")):
        data += b"".join(struct.pack(fmt, *entry) for entry in env[field])
    return data + bytes.fromhex(env["tail"])


def compute_environment(x, y, z, shared):
    """A v50 compute record with a Maxwell EXIT as code and no constant or texture data."""
    code = bytes(80) + struct.pack("<Q", 0xE30000000007000F)
    record = struct.pack("<6Q", len(code), 0, 0, 0, 0, 0)
    record += struct.pack("<7I", 0, 2, 80, 0, len(code) - 8, 1, 5) + code
    return record + struct.pack("<4I", x, y, z, shared)


def main():
    parser = argparse.ArgumentParser(description="Compile and validate captured shader regression fixtures")
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--fixture", default="*", help="fixture filename stem or glob")
    parser.add_argument("--spirv-val", default="spirv-val")
    args = parser.parse_args()
    fixtures = sorted((Path(__file__).parent / "fixtures").glob(args.fixture + ".json"))
    if not fixtures:
        parser.error("no matching captured fixtures")
    for path in fixtures:
        fixture = json.loads(path.read_text())
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            env = root / "shader.env"
            key = root / "pipeline.key"
            output = root / "shader.spv"
            if "native_header" in fixture:
                # A raw dump (native header + code), for programs without a cache record (task
                # and mesh programs aren't serialized)
                raw = root / "shader.bin"
                raw.write_bytes(bytes.fromhex(fixture["native_header"]))
                command = [str(args.compiler.resolve()), str(raw), "--format", "native-header",
                           "--stage", fixture["stage"], *fixture.get("flags", []),
                           "--output", str(output)]
            elif "native_code" in fixture:
                # An instruction-only compute dump (rejected programs never reach the cache):
                # compiled with a synthetic environment holding only the workgroup and shared
                # memory sizes (compute_environment: [x, y, z, shared bytes]).
                raw = root / "shader.bin"
                raw.write_bytes(bytes.fromhex(fixture["native_code"]))
                env.write_bytes(compute_environment(*fixture["compute_environment"]))
                command = [str(args.compiler.resolve()), str(raw), "--environment", str(env),
                           "--cache-version", "50", *fixture.get("flags", []),
                           "--output", str(output)]
            else:
                env.write_bytes(serialize_environment(fixture["environment"]))
                command = [str(args.compiler.resolve()), "--environment", str(env),
                           "--cache-version", str(fixture["cache_version"]), "--output",
                           str(output)]
            if "pipeline_key" in fixture:
                key.write_bytes(bytes.fromhex(fixture["pipeline_key"]))
                command += ["--pipeline-key", str(key)]
            compiled = subprocess.run(command, capture_output=True, text=True, timeout=30)
            if compiled.returncode:
                raise RuntimeError(f"{path.stem}: compiler exit {compiled.returncode}\n"
                                   f"{compiled.stdout}{compiled.stderr}")
            result = json.loads(compiled.stdout)
            if result["status"] != "ok":
                raise RuntimeError(f"{path.stem}: {result}")
            native_global = result.get("native_global", {})
            for field, expected in fixture.get("expect_native_global", {}).items():
                if native_global.get(field) != expected:
                    raise RuntimeError(f"{path.stem}: native_global {field} is "
                                       f"{native_global.get(field)}, expected {expected}")
            target = "vulkan1.3" if fixture.get("stage") in ("task", "mesh") else "vulkan1.2"
            validated = subprocess.run([args.spirv_val, "--target-env", target, str(output)],
                                       capture_output=True, text=True, timeout=30)
            if validated.returncode:
                raise RuntimeError(f"{path.stem}: {validated.stdout}{validated.stderr}")
            print(f"PASS {path.stem}: {result['spirv_words']} SPIR-V words")


if __name__ == "__main__":
    main()

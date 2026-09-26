#!/usr/bin/env python3
"""Build and run Tachyon without CMake.

Uses the system C++ compiler if one is on PATH (g++ / clang++), otherwise Zig's bundled
clang (`pip install ziglang`). CI and Linux/macOS users can use CMake instead.

    python build.py            # build everything
    python build.py test       # build + run unit tests
    python build.py fuzz       # build + run the planted-bug campaign
    python build.py bench      # build + run the benchmark
    python build.py wasm       # compile the engine to web/tachyon.wasm (needs ziglang)
    python build.py all        # tests, fuzz, bench, wasm, then copy results into web/data
"""

import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
OUT = ROOT / "build"
EXE = ".exe" if os.name == "nt" else ""

FLAGS = ["-std=c++20", "-O3", "-DNDEBUG", "-march=native", "-Wall", "-Wextra",
         "-Wno-nullability-completeness", "-I", str(ROOT / "include")]

TARGETS = {
    "tachyon_tests": (["tests/tests.cpp"], []),
    "tachyon_fuzz": (["fuzz/fuzz.cpp"], ["-DTACHYON_PLANTED_BUGS"]),
    "tachyon_bench": (["bench/bench.cpp"], []),
}


def compiler():
    for cxx in ("clang++", "g++"):
        if shutil.which(cxx):
            return [cxx]
    return [sys.executable, "-m", "ziglang", "c++"]


def build():
    OUT.mkdir(exist_ok=True)
    cxx = compiler()
    for name, (srcs, extra) in TARGETS.items():
        exe = OUT / (name + EXE)
        print(f"[build] {name}", flush=True)
        subprocess.run(cxx + FLAGS + extra + [str(ROOT / s) for s in srcs] + ["-o", str(exe)], check=True)


def run(name, *args):
    subprocess.run([str(OUT / (name + EXE)), *args], check=True, cwd=ROOT)


def build_wasm():
    print("[build] web/tachyon.wasm", flush=True)
    subprocess.run([sys.executable, "-m", "ziglang", "c++", "-target", "wasm32-wasi", "-std=c++20", "-O3",
                    "-DNDEBUG", "-fno-exceptions", "-fvisibility=hidden", "-mexec-model=reactor",
                    "-I", str(ROOT / "include"), str(ROOT / "wasm" / "tachyon_wasm.cpp"),
                    "-o", str(ROOT / "web" / "tachyon.wasm"),
                    "-Wl,--export-dynamic", "-Wl,--gc-sections", "-s"], check=True)


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "build"
    build()
    if cmd in ("test", "all"):
        run("tachyon_tests")
    if cmd in ("fuzz", "all"):
        run("tachyon_fuzz", "--out", "results/fuzz.json")
    if cmd in ("bench", "all"):
        run("tachyon_bench", "--out", "results/bench.json")
    if cmd in ("wasm", "all"):
        build_wasm()
    if cmd == "all":
        data = ROOT / "web" / "data"
        data.mkdir(parents=True, exist_ok=True)
        for f in ("fuzz.json", "bench.json"):
            shutil.copy(ROOT / "results" / f, data / f)
        print("[done] results copied to web/data")


if __name__ == "__main__":
    main()

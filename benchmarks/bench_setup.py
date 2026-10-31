#!/usr/bin/env python3
"""What the CMake bench targets need besides the C++ binaries: Python virtualenvs (uv), the load generator `oha`, and a check of what
disturbs the numbers. Called by benchmarks/CMakeLists.txt (targets bench_env and bench_check); it can be run by hand too.

    python bench_setup.py env --root .. --venvs build-bench/venvs --pys "3.14 3.14t" --oha build-bench/oha
    python bench_setup.py check
"""
import argparse
import glob
import os
import shutil
import stat
import subprocess
import sys
import urllib.request

OHA_URL = "https://github.com/hatoo/oha/releases/latest/download/oha-linux-amd64"


def run(cmd, **kw):
    print("+", " ".join(cmd), flush=True)
    return subprocess.run(cmd, check=True, **kw)


def find_uv():
    uv = shutil.which("uv") or os.path.expanduser("~/.local/bin/uv")
    if os.path.exists(uv):
        return uv
    print("installing uv (a single binary, in ~/.local/bin)", flush=True)
    script = urllib.request.urlopen("https://astral.sh/uv/install.sh").read()
    subprocess.run(["sh"], input=script, check=True)
    return os.path.expanduser("~/.local/bin/uv")


def venv_dir(venvs, version):
    return os.path.join(venvs, "py" + version.replace(".", ""))  # 3.14t -> py314t


def make_venv(uv, root, venvs, version):
    d = venv_dir(venvs, version)
    want_nogil = version.endswith("t")
    py = os.path.join(d, "bin", "python")
    if os.path.exists(py):
        have_nogil = subprocess.run([py, "-c", "import sys;print(not getattr(sys,'_is_gil_enabled',lambda:True)())"],
                                    capture_output=True, text=True).stdout.strip() == "True"
        if have_nogil != want_nogil:
            print(f"== {d} has the wrong build: recreating", flush=True)
            shutil.rmtree(d)
    if not os.path.exists(py):
        print(f"== venv {version}", flush=True)
        run([uv, "venv", "--python", version if want_nogil else version + "+gil", d])
    print(f"== installing httpp (built from this tree) and the servers to compare into {d}", flush=True)
    env = dict(os.environ, UV_LINK_MODE="copy")  # the project may sit on another filesystem than the uv cache
    run([uv, "pip", "install", "--python", py, "--reinstall", root, "uvicorn", "httptools", "fastapi", "websockets"], env=env)
    if subprocess.run([uv, "pip", "install", "--python", py, "uvloop"], env=env).returncode != 0:
        print(f"(no uvloop for {version}: uvicorn and websockets use asyncio there)")


def get_oha(path):
    if os.path.exists(path):
        return
    found = shutil.which("oha")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    if found:
        os.symlink(found, path)
        return
    print("downloading oha", flush=True)
    try:
        urllib.request.urlretrieve(OHA_URL, path)
    except Exception as e:  # noqa: BLE001
        sys.exit(f"could not download oha ({e}): install it (cargo install oha) and rerun")
    os.chmod(path, os.stat(path).st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)


def check():
    print("== things that disturb the numbers")
    try:
        gov = open("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor").read().strip()
    except OSError:
        gov = "unknown"
    print(f"governor: {gov}")
    if gov != "performance":
        print("  -> not performance. For steadier numbers:  sudo cpupower frequency-set -g performance  (or run it as is and tell me)")
    for p in glob.glob("/sys/class/power_supply/A*/online"):
        try:
            print("power: plugged in" if open(p).read().strip() == "1" else "power: ON BATTERY -> plug in the charger first")
        except OSError:
            pass
    print("close browsers and other heavy programs while it runs.")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="what", required=True)
    e = sub.add_parser("env")
    e.add_argument("--root", required=True, help="the httpp source tree")
    e.add_argument("--venvs", required=True)
    e.add_argument("--pys", default="3.14 3.14t")
    e.add_argument("--oha", required=True, help="where the load generator goes")
    sub.add_parser("check")
    args = ap.parse_args()
    if args.what == "check":
        return check()
    uv = find_uv()
    root = os.path.abspath(args.root)
    for v in args.pys.split():
        make_venv(uv, root, os.path.abspath(args.venvs), v)
    get_oha(os.path.abspath(args.oha))
    print(subprocess.run([os.path.abspath(args.oha), "--version"], capture_output=True, text=True).stdout.strip())


if __name__ == "__main__":
    main()

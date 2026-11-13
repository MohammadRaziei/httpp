#!/usr/bin/env python3
"""Build and run every snippet of the docs site and compare what it does with what its page claims.

    python3 scripts/test_snippets.py            Python and C++
    python3 scripts/test_snippets.py python     Python only
    python3 scripts/test_snippets.py cpp        C++ only

Needs `pip install .` of this repository first (the snippets import httpp), and CMake plus a C++17 compiler for the C++ side.
Standard library only. The C++ build uses the same CMake recipe as docs/install.mdx, so a broken recipe fails here too.

Each snippet listens on the literal port 8000, which is what a reader copies. Here the port is replaced by a free one in a
private copy of the file; the snippet files themselves are not touched.
"""

import json
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LANGS = json.loads((ROOT / "languages.json").read_text(encoding="utf-8"))
EXT = {lang["id"]: lang["ext"] for lang in LANGS}

CMAKE = """\
cmake_minimum_required(VERSION 3.15)
project(snippets LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(
    COMMAND ${Python3_EXECUTABLE} -m httpp py path --cmake-dir
    OUTPUT_VARIABLE httpp_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY
)
find_package(httpp CONFIG REQUIRED)

foreach(name IN ITEMS @NAMES@)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE httpp::httpp_core)
endforeach()
"""


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def http(method, url, headers=None, data=None):
    req = urllib.request.Request(url, data=data, method=method, headers=headers or {})
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status, r.headers, r.read().decode()
    except urllib.error.HTTPError as e:  # a 4xx/5xx is an answer here, not a failure
        return e.code, e.headers, e.read().decode()


def expect(cond, what):
    if not cond:
        raise AssertionError(what)


# ---- what each page claims -------------------------------------------------------------------------------------------

INSPECT = "POST /inspect\npage=2\ntoken=abc\nbody=hello"


def check_get_route(base):
    status, _, body = http("GET", base + "/hello/world")
    expect((status, body) == (200, "hello world"), f"GET /hello/world gave {status} {body!r}")


def check_request_info(base):
    status, _, body = http("POST", base + "/inspect?page=2", {"X-Token": "abc"}, b"hello")
    expect(status == 200, f"status {status}")
    expect(body == INSPECT, f"body {body!r}, expected {INSPECT!r}")


def check_responses(base):
    status, h, body = http("POST", base + "/items", data=b"")
    expect(status == 201, f"POST /items status {status}")
    expect(h.get("Location") == "/items/1", f"Location {h.get('Location')!r}")
    expect(json.loads(body) == {"id": 1}, f"POST /items body {body!r}")
    status, _, body = http("GET", base + "/items/1")
    expect(status == 200 and json.loads(body) == {"id": 1}, f"GET /items/1 gave {status} {body!r}")
    status, _, _ = http("GET", base + "/items/2")
    expect(status == 404, f"GET /items/2 status {status}")


def check_hooks(base):
    status, _, _ = http("GET", base + "/")
    expect(status == 403, f"without the key: {status}")
    status, h, body = http("GET", base + "/", {"X-Key": "secret"})
    expect((status, body) == (200, "hello"), f"with the key: {status} {body!r}")
    expect(h.get("X-Served-By") == "httpp", f"X-Served-By {h.get('X-Served-By')!r}")


def check_client_get(out, cwd):
    expect(out.splitlines() == ["200 hello world", "200"], f"output {out!r}")


def check_client_post(out, cwd):
    lines = out.splitlines()
    expect(lines[:1] == ["200"], f"output {out!r}")
    expect("\n".join(lines[1:]) == 'POST /inspect\npage=2\ntoken=abc\nbody={"a": 1}', f"output {out!r}")


HELLO_SHA = "b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9"  # sha256 of b"hello world"


def saved_hello(cwd):
    expect((cwd / "hello.txt").read_text() == "hello world", f"hello.txt holds {(cwd / 'hello.txt').read_text()!r}")


def check_download_file(out, cwd):
    expect(out.strip() == "200", f"output {out!r}")
    saved_hello(cwd)


def check_download_verified(out, cwd):
    expect(out.strip() == HELLO_SHA, f"output {out!r}")
    saved_hello(cwd)


def check_download_async(out, cwd):
    expect(out.strip() == "200", f"output {out!r}")
    saved_hello(cwd)


# files a server snippet expects in its working folder
FIXTURES = {
    "send_file": {"report.txt": "quarterly numbers\n"},
    "serve_folder": {"public/index.html": "<h1>home</h1>", "public/notes.txt": "notes", "public/.hidden": "dotfile"},
    "file_browser": {"shared/a.txt": "alpha", "shared/sub/b.txt": "beta", "secret.txt": "outside the shared folder"},
}


def check_send_file(base):
    status, h, body = http("GET", base + "/report")
    expect(status == 200 and body == "quarterly numbers\n", f"GET /report gave {status} {body!r}")
    expect((h.get("Content-Type") or "").startswith("text/plain"), f"Content-Type {h.get('Content-Type')!r}")
    expect((h.get("Content-Disposition") or "").startswith("attachment"), f"Content-Disposition {h.get('Content-Disposition')!r}")


def check_serve_folder(base):
    status, _, body = http("GET", base + "/static/notes.txt")
    expect((status, body) == (200, "notes"), f"/static/notes.txt gave {status} {body!r}")
    status, _, body = http("GET", base + "/static/")
    expect(status == 200 and "<h1>home</h1>" in body, f"/static/ gave {status} {body!r}")
    status, _, body = http("GET", base + "/static/.hidden")
    expect((status, body) == (200, "dotfile"), f"a dotfile is served by default, got {status} {body!r}")
    status, _, _ = http("GET", base + "/static/nope.txt")
    expect(status == 404, f"a missing file gave {status}")


def check_file_browser(base):
    status, h, body = http("GET", base + "/files/")
    expect(status == 200 and "html" in (h.get("Content-Type") or ""), f"/files/ gave {status} {h.get('Content-Type')!r}")
    status, _, body = http("GET", base + "/files/api/list?path=/")
    expect(status == 200, f"list status {status}: {body[:200]!r}")
    entries = {e["name"]: e["type"] for e in json.loads(body)["entries"]}
    expect(entries == {"a.txt": "file", "sub": "dir"}, f"root entries {entries}")
    status, _, body = http("GET", base + "/files/api/list?path=/sub")
    expect(status == 200 and [e["name"] for e in json.loads(body)["entries"]] == ["b.txt"], f"sub list {status} {body[:200]!r}")
    status, _, body = http("GET", base + "/files/raw/a.txt")
    expect((status, body) == (200, "alpha"), f"raw/a.txt gave {status} {body!r}")
    status, _, body = http("GET", base + "/files/raw/..%2Fsecret.txt")
    expect(status == 404 and "outside" not in body, f"a path above the root gave {status} {body!r}")


SERVERS = {
    "get_route": check_get_route,
    "request_info": check_request_info,
    "responses": check_responses,
    "hooks": check_hooks,
    "send_file": check_send_file,
    "serve_folder": check_serve_folder,
    "file_browser": check_file_browser,
}
# client snippet: (the server snippet it talks to, its check)
CLIENTS = {
    "client_get": ("get_route", check_client_get),
    "client_post": ("request_info", check_client_post),
    "download_file": ("get_route", check_download_file),
    "download_verified": ("get_route", check_download_verified),
    "download_async": ("get_route", check_download_async),
}


# ---- running ---------------------------------------------------------------------------------------------------------

def patched(lang, name, port, dest):
    src = (ROOT / "snippets" / lang / f"{name}.{EXT[lang]}").read_text(encoding="utf-8")
    assert "8000" in src, f"{name}: no port 8000 to replace"
    dest.write_text(src.replace("8000", str(port)), encoding="utf-8")


def wait_listening(port, proc, log):
    end = time.time() + 20
    while time.time() < end:
        if proc.poll() is not None:
            raise AssertionError(f"exited with {proc.returncode} before listening:\n{log.read_text()}")
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.5).close()
            return
        except OSError:
            time.sleep(0.1)
    raise AssertionError(f"nothing listening on {port} after 20 s:\n{log.read_text()}")


def run_case(lang, name, command, port, work):
    """Start the server snippet, run the checks that talk to it, stop it."""
    log = work / f"{name}.log"
    home = work / lang / f"{name}.srv"
    home.mkdir(parents=True, exist_ok=True)
    for rel, text in FIXTURES.get(name, {}).items():
        (home / rel).parent.mkdir(parents=True, exist_ok=True)
        (home / rel).write_text(text, encoding="utf-8")
    with open(log, "w") as out:
        proc = subprocess.Popen(command, stdout=out, stderr=subprocess.STDOUT, cwd=home)
        try:
            wait_listening(port, proc, log)
            base = f"http://127.0.0.1:{port}"
            SERVERS[name](base)
            for client, (server, check) in CLIENTS.items():
                if server == name:
                    run_client(lang, client, port, check, work)
        finally:
            proc.terminate()
            try:
                proc.wait(5)
            except subprocess.TimeoutExpired:
                proc.kill()


def run_client(lang, name, port, check, work):
    if lang == "python":
        # a client snippet needs the port of the server it talks to; the C++ ones were patched before the build
        dest = work / "python" / f"{name}.py"
        patched("python", name, port, dest)
        command = [sys.executable, str(dest)]
    else:
        command = [str(work / "cpp" / "build" / name)]
    cwd = work / lang / f"{name}.cwd"  # a snippet that saves a file saves it here
    cwd.mkdir(exist_ok=True)
    r = subprocess.run(command, capture_output=True, text=True, timeout=60, cwd=cwd)
    try:
        expect(r.returncode == 0, f"exited with {r.returncode}:\n{r.stdout}{r.stderr}")
        check(r.stdout, cwd)
    except AssertionError as e:
        raise AssertionError(f"client snippet {name}: {e}") from None
    print(f"  ok  {lang:6} {name}")


def build_cpp(work, ports):
    names = list(SERVERS) + list(CLIENTS)
    folder = work / "cpp"
    folder.mkdir(exist_ok=True)  # main() already made it
    for name in SERVERS:
        patched("cpp", name, ports[name], folder / f"{name}.cpp")
    for name, (server, _) in CLIENTS.items():
        patched("cpp", name, ports[server], folder / f"{name}.cpp")
    (folder / "CMakeLists.txt").write_text(CMAKE.replace("@NAMES@", " ".join(names)), encoding="utf-8")
    for step in (
        ["cmake", "-S", str(folder), "-B", str(folder / "build"), f"-DPython3_EXECUTABLE={sys.executable}",
         "-DCMAKE_BUILD_TYPE=Release"],
        ["cmake", "--build", str(folder / "build"), "--parallel"],
    ):
        r = subprocess.run(step, capture_output=True, text=True, timeout=900)
        if r.returncode != 0:
            raise SystemExit(f"C++ snippets did not build ({' '.join(step[:2])}):\n{r.stdout[-4000:]}\n{r.stderr[-4000:]}")
    print("  ok  cpp    all snippets compiled")


def main(argv):
    want = argv[1:] or ["python", "cpp"]
    bad = [w for w in want if w not in EXT]
    if bad:
        print(__doc__, file=sys.stderr)
        return 2

    work = Path(tempfile.mkdtemp(prefix="httpp-snippets-"))
    failures = []
    try:
        ports = {name: free_port() for name in SERVERS}
        for lang in want:
            (work / lang).mkdir(exist_ok=True)
            if lang == "cpp":
                build_cpp(work, ports)
            for name in SERVERS:
                if lang == "python":
                    dest = work / "python" / f"{name}.py"
                    patched("python", name, ports[name], dest)
                    command = [sys.executable, str(dest)]
                else:
                    command = [str(work / "cpp" / "build" / name)]
                try:
                    run_case(lang, name, command, ports[name], work)
                    print(f"  ok  {lang:6} {name}")
                except (AssertionError, subprocess.TimeoutExpired) as e:
                    failures.append(f"{lang} {name}: {e}")
                    print(f"FAIL  {lang:6} {name}", file=sys.stderr)
    finally:
        shutil.rmtree(work, ignore_errors=True)

    if failures:
        print("\n" + "\n\n".join(failures), file=sys.stderr)
        return 1
    print("all snippets behave as their pages say")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

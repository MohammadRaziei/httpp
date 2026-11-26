"""Build and run every example in examples/ and compare what it does with what its docs page claims (ADR-0060).

    pytest tests/python/test_examples.py

Needs `pip install .` of this repository first (the examples import httpp). The C++ side also needs CMake and a C++17 compiler and
is skipped when CMake is not there. The C++ build uses the same CMake recipe as docs/site/docs/install.mdx, so a broken recipe fails
here too.

A topic is examples/<topic>/<lang>/<file>.<ext>: `main` is the example of its page, any other file is an extra that the page shows
below it, and an extra may exist in one language only. A unit is named `topic` for the main file and `topic/file` for an extra.

Each example listens on the literal port 8000, which is what a reader copies. Here the port is replaced by a free one in a private
copy of the file; the files in examples/ are not touched. A unit needs a check below: a server in SERVERS, a program that talks to
a server in CLIENTS (`test_every_example_has_a_check` fails otherwise).
"""

import json
import shutil
import ssl
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

import pytest

EXAMPLES = Path(__file__).resolve().parents[2] / "examples"
LANGS = {"python": "py", "cpp": "cpp"}

CMAKE = """\
cmake_minimum_required(VERSION 3.15)
project(examples LANGUAGES CXX)
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
    if(WIN32 AND NOT CMAKE_VERSION VERSION_LESS 3.21)
        # Windows finds a DLL next to the program or on PATH, and the DLLs of httpp sit in the installed package, which is on neither
        add_custom_command(TARGET ${name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:${name}> $<TARGET_FILE_DIR:${name}>
            COMMAND_EXPAND_LISTS)
    endif()
endforeach()
"""


def http(method, url, headers=None, data=None):
    req = urllib.request.Request(url, data=data, method=method, headers=headers or {})
    context = ssl._create_unverified_context() if url.startswith("https://") else None  # the test certificate is self-signed
    try:
        with urllib.request.urlopen(req, timeout=10, context=context) as r:
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


def check_request_data(base):
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


def check_get_request(out, cwd):
    expect(out.splitlines() == ["200 hello world", "200"], f"output {out!r}")


def check_upload(out, cwd):
    lines = out.splitlines()
    expect(lines[:1] == ["200"], f"output {out!r}")
    expect("\n".join(lines[1:]) == "title=shopping\nname=notes.txt\ntype=text/plain\nsize=18", f"output {out!r}")


def check_upload_progress(out, cwd):
    lines = out.splitlines()
    expect(lines[-1] == "200", f"output {out!r}")
    steps = [tuple(int(n) for n in line.split("/")) for line in lines[:-1]]
    expect(len(steps) >= 4, f"only {len(steps)} progress lines: {out!r}")
    expect(steps[0][0] == 0 and steps[-1][0] == steps[-1][1] and steps[-1][1] > 200000, f"progress {steps[0]} ... {steps[-1]}")
    expect([s for s, _ in steps] == sorted(s for s, _ in steps), "progress went backwards")


def check_post_request(out, cwd):
    lines = out.splitlines()
    expect(lines[:1] == ["200"], f"output {out!r}")
    expect("\n".join(lines[1:]) == 'POST /inspect\npage=2\ntoken=abc\nbody={"a": 1}', f"output {out!r}")


HELLO_SHA = "b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9"  # sha256 of b"hello world"


def saved_hello(cwd):
    expect((cwd / "hello.txt").read_text() == "hello world", f"hello.txt holds {(cwd / 'hello.txt').read_text()!r}")


def check_download(out, cwd):
    expect(out.strip() == "200", f"output {out!r}")
    saved_hello(cwd)


def check_download_verified(out, cwd):
    expect(out.strip() == HELLO_SHA, f"output {out!r}")
    saved_hello(cwd)


def check_download_async(out, cwd):
    expect(out.strip() == "200", f"output {out!r}")
    saved_hello(cwd)


def check_send_file(base):
    status, h, body = http("GET", base + "/report")
    expect(status == 200 and body == "quarterly numbers\n", f"GET /report gave {status} {body!r}")
    expect((h.get("Content-Type") or "").startswith("text/plain"), f"Content-Type {h.get('Content-Type')!r}")
    expect((h.get("Content-Disposition") or "").startswith("attachment"), f"Content-Disposition {h.get('Content-Disposition')!r}")


def check_serve_directory(base):
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



def check_swagger(base):
    status, _, body = http("GET", base + "/openapi.json")
    doc = json.loads(body)
    expect(status == 200 and doc["info"]["title"] == "Hello API", f"/openapi.json gave {status} {body[:120]!r}")
    expect(set(doc["paths"]) == {"/hello/{name}", "/health"}, f"paths {sorted(doc['paths'])}")
    expect(doc["paths"]["/health"]["get"]["security"] == [], "/health is not public")
    expect(doc["security"] == [{"jwt": []}], f"default security {doc.get('security')}")
    status, h, body = http("GET", base + "/docs")
    expect(status == 200 and "swagger" in body.lower(), f"/docs gave {status} {body[:120]!r}")
    status, _, body = http("GET", base + "/hello/ann")
    expect(status == 200 and json.loads(body) == {"hello": "ann"}, f"/hello/ann gave {status} {body!r}")


def check_swagger_auto_document(base):
    status, _, body = http("GET", base + "/openapi.json")
    doc = json.loads(body)
    expect(status == 200 and doc["info"]["title"] == "Library API", f"/openapi.json gave {status} {body[:120]!r}")
    expect({"/books", "/books/{book_id}", "/health"} <= set(doc["paths"]), f"paths {sorted(doc['paths'])}")
    expect(doc["paths"]["/books/by-author/{author}"]["get"].get("deprecated") is True, "by-author is not deprecated")
    status, _, body = http("GET", base + "/docs")
    expect(status == 200 and "swagger" in body.lower(), f"/docs gave {status} {body[:120]!r}")
    status, _, body = http("GET", base + "/books?limit=2")
    expect(status == 200 and len(json.loads(body)) == 2, f"/books?limit=2 gave {status} {body[:120]!r}")
    status, _, _ = http("GET", base + "/books/99")
    expect(status == 404, f"/books/99 gave {status}")


def check_server_handle(base):
    status, _, body = http("GET", base + "/a.txt")
    expect(status == 200 and body == "alpha", f"/a.txt over TLS gave {status} {body!r}")
    status, _, _ = http("GET", base + "/.hidden")
    expect(status == 404, f"a dotfile gave {status}")


def check_server_handle_mount_into(base):
    status, _, body = http("GET", base + "/static/notes.txt")
    expect(status == 200 and body == "notes", f"/static/notes.txt gave {status} {body!r}")
    status, _, body = http("GET", base + "/ping")
    expect(status == 200 and body == "pong", f"/ping gave {status} {body!r}")


def check_server_handle_stop(base):
    status, _, body = http("GET", base + "/quit")
    expect(status == 200 and body == "bye", f"/quit gave {status} {body!r}")


def check_errors(base):
    status, _, body = http("GET", base + "/users/1")
    expect(status == 200 and json.loads(body) == {"id": 1, "name": "ada"}, f"/users/1 gave {status} {body!r}")
    status, _, body = http("GET", base + "/users/9")
    expect(status == 404 and json.loads(body) == {"error": "no such user"}, f"/users/9 gave {status} {body!r}")
    status, _, body = http("GET", base + "/parse")
    expect(status == 422 and json.loads(body) == {"error": "not a number"}, f"/parse gave {status} {body!r}")
    status, _, body = http("GET", base + "/boom")
    expect(status == 500 and "secret internals" not in body, f"/boom gave {status} {body!r}")  # the message stays in the server
    status, h, body = http("GET", base + "/nope")
    expect(status == 404 and json.loads(body) == {"error": "not found"}, f"/nope gave {status} {body!r}")
    expect((h.get("Content-Type") or "").startswith("application/json"), f"Content-Type {h.get('Content-Type')!r}")


def check_errors_http_exception(base):
    status, _, body = http("GET", base + "/users/1")
    expect(status == 200 and json.loads(body) == {"id": 1, "name": "ada"}, f"/users/1 gave {status} {body!r}")
    status, _, body = http("GET", base + "/users/9")
    expect(status == 404 and json.loads(body) == {"detail": "no such user"}, f"/users/9 gave {status} {body!r}")
    status, h, body = http("GET", base + "/admin")
    expect(status == 401 and h.get("WWW-Authenticate") == "Bearer", f"/admin gave {status} {h.get('WWW-Authenticate')!r}")


# files a server example expects in its working folder (the key is the unit)
FIXTURES = {
    "send_file": {"report.txt": "quarterly numbers\n"},
    "serve_directory": {"public/index.html": "<h1>home</h1>", "public/notes.txt": "notes", "public/.hidden": "dotfile"},
    "file_browser": {"shared/a.txt": "alpha", "shared/sub/b.txt": "beta", "secret.txt": "outside the shared folder"},
    "server_handle": {"public/a.txt": "alpha", "public/.hidden": "dotfile"},
    "server_handle/mount_into": {"public/notes.txt": "notes"},
}
HTTPS = {"server_handle"}                 # these listen with TLS: they need cert.pem and key.pem, and the checks use https://
EXITS = {"server_handle/stop"}            # these end themselves once the check has run

SERVERS = {
    "get_route": check_get_route,
    "request_data": check_request_data,
    "responses": check_responses,
    "errors": check_errors,
    "errors/http_exception": check_errors_http_exception,
    "hooks": check_hooks,
    "send_file": check_send_file,
    "serve_directory": check_serve_directory,
    "file_browser": check_file_browser,
    "swagger": check_swagger,
    "swagger/auto_document": check_swagger_auto_document,
    "server_handle": check_server_handle,
    "server_handle/mount_into": check_server_handle_mount_into,
    "server_handle/stop": check_server_handle_stop,
}
# a program that talks to a server: (the unit of that server, its check)
CLIENTS = {
    "get_request": ("get_route", check_get_request),
    "post_request": ("request_data", check_post_request),
    "upload": ("request_data", check_upload),
    "upload/progress": ("request_data", check_upload_progress),
    "download": ("get_route", check_download),
    "download/verified": ("get_route", check_download_verified),
    "download/async": ("get_route", check_download_async),
}


def source(lang, unit):
    topic, _, file = unit.partition("/")
    return EXAMPLES / topic / lang / f"{file or 'main'}.{LANGS[lang]}"


def target(unit):
    return unit.replace("/", "__")


def cpp_program(work, unit):
    """The built example. A single-configuration generator puts it in build/, Visual Studio in build/<config>/, and Windows adds .exe."""
    build = work / "cpp" / "build"
    suffixes = ("", ".exe") if sys.platform == "win32" else ("",)
    tried = [build / folder / f"{target(unit)}{suffix}" for folder in ("", "Release") for suffix in suffixes]
    for candidate in tried:
        if candidate.is_file():
            return str(candidate)
    found = sorted(str(p.relative_to(build)) for p in build.rglob(f"{target(unit)}*") if p.is_file())
    pytest.fail(f"{target(unit)} was built nowhere I looked: {[str(p.relative_to(build)) for p in tried]}; files with that name: {found}")


def units_in_the_folder():
    """{unit: [languages]} found in examples/."""
    found = {}
    for lang, ext in LANGS.items():
        for f in sorted(EXAMPLES.glob(f"*/{lang}/*.{ext}")):
            topic = f.parent.parent.name
            found.setdefault(topic if f.stem == "main" else f"{topic}/{f.stem}", []).append(lang)
    return found


# ---- running ---------------------------------------------------------------------------------------------------------

def patched(lang, unit, port, dest):
    src = source(lang, unit).read_text(encoding="utf-8")
    assert "8000" in src, f"{unit}: no port 8000 to replace"
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


def make_certificate(folder):
    if shutil.which("openssl") is None:
        pytest.skip("openssl is needed to make a certificate")
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", str(folder / "key.pem"), "-out",
                    str(folder / "cert.pem"), "-days", "1", "-subj", "/CN=localhost"], check=True, capture_output=True)


def run_client(lang, unit, port, check, work):
    if lang == "python":
        dest = work / "python" / f"{target(unit)}.py"
        patched("python", unit, port, dest)
        command = [sys.executable, str(dest)]
    else:  # the C++ ones were patched before the build
        command = [cpp_program(work, unit)]
    cwd = work / lang / f"{target(unit)}.cwd"  # a program that saves a file saves it here
    cwd.mkdir(exist_ok=True)
    r = subprocess.run(command, capture_output=True, text=True, timeout=60, cwd=cwd)
    try:
        expect(r.returncode == 0, f"exited with {r.returncode}:\n{r.stdout}{r.stderr}")
        check(r.stdout, cwd)
    except AssertionError as e:
        raise AssertionError(f"client example {unit} ({lang}): {e}") from None


@pytest.fixture(scope="session")
def work(tmp_path_factory):
    return tmp_path_factory.mktemp("httpp-examples")


@pytest.fixture(scope="session")
def ports():
    found = {}
    for unit in SERVERS:
        with socket.socket() as s:
            s.bind(("127.0.0.1", 0))
            found[unit] = s.getsockname()[1]
    return found


@pytest.fixture(scope="session")
def cpp_build(work, ports):
    """Every C++ file of every unit in one CMake project, ports already replaced."""
    if shutil.which("cmake") is None:
        pytest.skip("CMake is needed to build the C++ examples")
    folder = work / "cpp"
    folder.mkdir(exist_ok=True)
    names = []
    for unit in list(SERVERS) + list(CLIENTS):
        if not source("cpp", unit).is_file():
            continue
        server = unit if unit in SERVERS else CLIENTS[unit][0]
        patched("cpp", unit, ports[server], folder / f"{target(unit)}.cpp")
        names.append(target(unit))
    (folder / "CMakeLists.txt").write_text(CMAKE.replace("@NAMES@", " ".join(names)), encoding="utf-8")
    for step in (
        ["cmake", "-S", str(folder), "-B", str(folder / "build"), f"-DPython3_EXECUTABLE={sys.executable}", "-DCMAKE_BUILD_TYPE=Release"],
        ["cmake", "--build", str(folder / "build"), "--parallel", "--config", "Release"],  # the config is for Visual Studio, which ignores CMAKE_BUILD_TYPE
    ):
        r = subprocess.run(step, capture_output=True, text=True, timeout=900)
        if r.returncode != 0:
            pytest.fail(f"C++ examples did not build ({' '.join(step[:2])}):\n{r.stdout[-4000:]}\n{r.stderr[-4000:]}")


# The C++ examples are built against the SDK of the package, with CMake, in a staged tree: that is what the CMake target
# httpp_test_python (after build_python) provides, and it runs them with `-m cpp`. A plain `pytest` (a pip-installed wheel,
# cibuildwheel) skips them: see pytest_collection_modifyitems in conftest.py.
CASES = [
    pytest.param(lang, unit, marks=pytest.mark.cpp if lang == "cpp" else (), id=f"{lang}-{unit.replace('/', '-')}")
    for unit in SERVERS
    for lang in LANGS
    if source(lang, unit).is_file()
]


@pytest.mark.parametrize("lang,unit", CASES)
def test_server_example_does_what_its_page_says(lang, unit, work, ports, request):
    """Start the example, run the check that talks to it and the clients that belong to it, stop it."""
    if lang == "cpp":
        request.getfixturevalue("cpp_build")
        command = [cpp_program(work, unit)]
    else:
        dest = work / "python" / f"{target(unit)}.py"
        dest.parent.mkdir(exist_ok=True)
        patched("python", unit, ports[unit], dest)
        command = [sys.executable, str(dest)]
    port = ports[unit]
    log = work / f"{lang}-{target(unit)}.log"
    home = work / lang / f"{target(unit)}.srv"
    home.mkdir(parents=True, exist_ok=True)
    for rel, text in FIXTURES.get(unit, {}).items():
        (home / rel).parent.mkdir(parents=True, exist_ok=True)
        (home / rel).write_bytes(text.encode("utf-8"))  # bytes: write_text would turn each \n into \r\n on Windows and the body would differ
    if unit in HTTPS:
        make_certificate(home)
    scheme = "https" if unit in HTTPS else "http"
    with open(log, "w") as out:
        proc = subprocess.Popen(command, stdout=out, stderr=subprocess.STDOUT, cwd=home)
        try:
            wait_listening(port, proc, log)
            SERVERS[unit](f"{scheme}://127.0.0.1:{port}")
            for client, (server, check) in CLIENTS.items():
                if server == unit and source(lang, client).is_file():
                    run_client(lang, client, port, check, work)
            if unit in EXITS:
                try:
                    proc.wait(10)
                except subprocess.TimeoutExpired:
                    raise AssertionError(f"{unit} did not end by itself:\n{log.read_text()}") from None
        finally:
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(5)
                except subprocess.TimeoutExpired:
                    proc.kill()


def test_every_example_has_a_check():
    """A file in examples/ without a check here would be an example that nothing runs."""
    found = set(units_in_the_folder())
    unchecked = sorted(found - set(SERVERS) - set(CLIENTS))
    assert not unchecked, f"examples with no check in tests/python/test_examples.py: {unchecked}"
    assert not sorted((set(SERVERS) | set(CLIENTS)) - found), "a check names an example that does not exist"
    assert not sorted(set(CLIENTS) & set(SERVERS)), "a unit is both a server and a client"


def test_every_topic_has_both_languages_in_main_and_a_readme():
    for folder in sorted(p for p in EXAMPLES.iterdir() if (p / "python").is_dir() or (p / "cpp").is_dir()):
        assert (folder / "python" / "main.py").is_file(), f"{folder.name}: no python/main.py"
        assert (folder / "cpp" / "main.cpp").is_file(), f"{folder.name}: no cpp/main.cpp"
        assert (folder / "README.md").is_file(), f"{folder.name}: no README.md"

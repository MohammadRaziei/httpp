"""cmake/EmbedFile.cmake: files of awkward sizes and contents survive the trip into a C++ binary, in both the
string-literal form and the byte-array form (the one MSVC needs)."""

import os
import shutil
import subprocess

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))).replace("\\", "/")  # forward slashes: a backslash path is an invalid escape in CMake code and in a C++ string literal
CASES = {
    "empty": b"",
    "one": b"A",
    "b31": bytes(range(31)),
    "b32": bytes(range(32)),
    "b33": bytes(range(33)),
    "b64": bytes(range(64)),
    "all_bytes_twice": bytes(range(256)) * 2,
    "text_with_specials": b'say "hi" \\n ?? ??/ \x00\x01 \xe2\x80\xa6 @NAME@ ; [x] ${y}\r\n' * 7,
    "minified_one_long_line": b"var a=1;" * 40000,  # 320 KB, no newline at all
}

CMAKE_LISTS = """\
cmake_minimum_required(VERSION 3.21)
project(embedtest CXX)
set(CMAKE_CXX_STANDARD 17)
list(APPEND CMAKE_MODULE_PATH "{root}/cmake")
include(EmbedFile)
embed_files(OUTPUT ${{CMAKE_BINARY_DIR}}/gen/data.hpp NAMESPACE t FILES
{files})
add_executable(check check.cpp)
target_include_directories(check PRIVATE ${{CMAKE_BINARY_DIR}}/gen)
"""

CHECK_CPP = """\
#include "data.hpp"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
static int failures = 0;
static void check(const char* name, const unsigned char* data, std::size_t size, const char* path) {{
    std::ifstream in(path, std::ios::binary);
    std::string want((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (want.size() != size || (size && std::memcmp(want.data(), data, size) != 0)) {{
        std::printf("MISMATCH %s: want %zu bytes, have %zu\\n", name, want.size(), size);
        ++failures;
    }}
}}
int main() {{
{checks}
    std::printf(failures ? "FAILED\\n" : "OK\\n");
    return failures ? 1 : 0;
}}
"""


@pytest.mark.skipif(shutil.which("cmake") is None or not (shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")),
                    reason="needs cmake and a C++ compiler")
@pytest.mark.parametrize("as_array", [False, True], ids=["string_literals", "byte_arrays"])
def test_files_survive_embedding(tmp_path, as_array):
    files, checks = [], []
    for name, content in CASES.items():
        path = tmp_path / f"{name}.bin"
        path.write_bytes(content)
        files.append(f"    {name} {path.as_posix()}")
        checks.append(f'    check("{name}", t::{name}, t::{name}_size, "{path.as_posix()}");')
    (tmp_path / "CMakeLists.txt").write_text(CMAKE_LISTS.format(root=ROOT, files="\n".join(files)))
    (tmp_path / "check.cpp").write_text(CHECK_CPP.format(checks="\n".join(checks)))

    build = tmp_path / "build"
    flags = ["-DHTTPP_EMBED_AS_ARRAY=" + ("ON" if as_array else "OFF"), "-DCMAKE_BUILD_TYPE=Release"]
    for cmd in (["cmake", "-S", str(tmp_path), "-B", str(build), *flags], ["cmake", "--build", str(build)]):
        r = subprocess.run(cmd, capture_output=True, text=True)
        assert r.returncode == 0, r.stdout + r.stderr
    exe = next(p for p in (build / "check", build / "Release" / "check.exe", build / "check.exe") if p.exists())
    r = subprocess.run([str(exe)], capture_output=True, text=True)
    assert r.returncode == 0 and r.stdout.strip() == "OK", r.stdout + r.stderr


def test_a_changed_file_changes_the_header_and_an_unchanged_one_does_not(tmp_path):
    if shutil.which("cmake") is None:
        pytest.skip("needs cmake")
    data = tmp_path / "d.bin"
    data.write_bytes(b"one")
    (tmp_path / "CMakeLists.txt").write_text(
        "cmake_minimum_required(VERSION 3.21)\nproject(t NONE)\n"
        f'list(APPEND CMAKE_MODULE_PATH "{ROOT}/cmake")\ninclude(EmbedFile)\n'
        f'embed_files(OUTPUT ${{CMAKE_BINARY_DIR}}/gen/d.hpp NAMESPACE t FILES d {data.as_posix()})\n')
    build, header = tmp_path / "b", tmp_path / "b" / "gen" / "d.hpp"
    configure = ["cmake", "-S", str(tmp_path), "-B", str(build)]
    assert subprocess.run(configure, capture_output=True).returncode == 0
    first = (header.stat().st_mtime_ns, header.read_text())
    assert subprocess.run(configure, capture_output=True).returncode == 0
    assert header.stat().st_mtime_ns == first[0]  # unchanged input: the header is not touched, so nothing rebuilds
    data.write_bytes(b"two!")
    assert subprocess.run(configure, capture_output=True).returncode == 0
    assert header.read_text() != first[1] and "\\x74\\x77\\x6f\\x21" in header.read_text()

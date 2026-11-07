// httpp: the command line (ADR-0041).
//
// Native commands, no interpreter and no GIL:
//   httpp server   [DIRECTORY]   serve a directory
//   httpp browse   [DIRECTORY]   read-only file browser
//   httpp download URL           download a URL
//   httpp curl     URL           a small curl
// Python commands live behind `py` and need a Python with httpp installed:
//   httpp py run main:app ...    run an httpp app
//   httpp py path ...            print the SDK directories
//
// Everything after `py` is handed to `<python> -m httpp py` untouched, so the options of the Python commands are defined
// in one place only (src/bindings/python/httpp/__main__.py).
//
// macOS 10.13 limits apply here too: POSIX calls instead of std::filesystem.

#include "CLI11.hpp"

#include "httpp.h"
#include "httpp/client.hpp"
#include "httpp/download.hpp"
#include "httpp/file_browser.hpp"
#include "httpp/server.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#  include <io.h>
#  include <process.h>
#  include <sys/stat.h>
#  include <windows.h>
#else
#  include <sys/stat.h>
#  include <unistd.h>
#  ifdef __APPLE__
#    include <mach-o/dyld.h>
#  endif
#endif

namespace {

#define HTTPP_CLI_STR2(x) #x
#define HTTPP_CLI_STR(x) HTTPP_CLI_STR2(x)
const char* const k_version = HTTPP_CLI_STR(HTTPP_VERSION_MAJOR) "." HTTPP_CLI_STR(HTTPP_VERSION_MINOR) "." HTTPP_CLI_STR(HTTPP_VERSION_PATCH);

int fail(const std::string& message) {
    std::cerr << "httpp: error: " << message << std::endl;
    return 1;
}

bool is_directory(const std::string& path) {
#ifdef _WIN32
    struct _stat st;
    return _stat(path.c_str(), &st) == 0 && (st.st_mode & _S_IFDIR) != 0;
#else
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

// The absolute form of a directory that exists, for the "Browsing ..." line (the library resolves it again itself).
std::string absolute_path(const std::string& path) {
#ifdef _WIN32
    char buf[MAX_PATH * 4];
    DWORD n = GetFullPathNameA(path.c_str(), static_cast<DWORD>(sizeof buf), buf, nullptr);
    return (n > 0 && n < sizeof buf) ? std::string(buf, n) : path;
#else
    char* p = ::realpath(path.c_str(), nullptr);
    if (!p) return path;
    std::string out(p);
    std::free(p);
    return out;
#endif
}

// --- serving until Ctrl+C or SIGTERM ---

std::atomic<int> g_signals{0};
static_assert(std::atomic<int>::is_always_lock_free, "the signal handler needs a lock-free atomic");

extern "C" void on_signal(int) { g_signals.fetch_add(1); }

// Blocks until the server is done. The first signal drains (requests in progress finish within `grace`), the second cuts at once.
// shutdown() and stop() do nothing until the port is bound, so they are repeated until listen() has returned.
bool serve(httpp::server& srv, const std::string& host, int port, double grace) {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::atomic<bool> finished{false};
    bool listened = false;
    std::thread listener([&] {
        listened = srv.listen(host, port);
        finished.store(true);
    });
    const auto grace_ms = std::chrono::milliseconds(static_cast<long long>(grace * 1000.0));
    while (!finished.load()) {
        int n = g_signals.load();
        if (n >= 2) srv.stop();
        else if (n == 1) srv.shutdown(grace_ms);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    listener.join();
    return listened;
}

int cmd_server(const std::string& directory, const std::string& host, int port, double grace) {
    if (!is_directory(directory)) return fail("'" + directory + "' is not a folder");
    httpp::server srv;
    try {
        srv.serve_directory("/", directory);
    } catch (const std::exception& e) {
        return fail(e.what());
    }
    std::cout << "Serving '" << directory << "' at http://" << host << ":" << port << "/" << std::endl;
    if (!serve(srv, host, port, grace)) return fail("cannot listen on " + host + ":" + std::to_string(port));
    return 0;
}

struct browse_args {
    std::string directory = ".";
    std::string host = "127.0.0.1";
    int port = 8000;
    double grace = 10;
    httpp::file_browser_options options;
    bool no_search = false;
};

int cmd_browse(const browse_args& a) {
    if (!is_directory(a.directory)) return fail("'" + a.directory + "' is not a folder");
    const std::string root = absolute_path(a.directory);
    httpp::server srv;
    httpp::file_browser_options options = a.options;
    options.search = !a.no_search;
    try {
        httpp::mount_file_browser(srv, "/", root, options);
    } catch (const std::exception& e) {
        return fail(e.what());
    }
    std::cout << "Browsing '" << root << "' (read-only) at http://" << a.host << ":" << a.port << "/" << std::endl;
    if (!serve(srv, a.host, a.port, a.grace)) return fail("cannot listen on " + a.host + ":" + std::to_string(a.port));
    return 0;
}

int cmd_download(const std::string& url, const std::string& output, bool quiet) {
    if (!output.empty()) {
        // A real download to a file, with a terminal progress bar: all of it is httpp::download.
        httpp::download_result result = httpp::download(url, output).enable_progress(!quiet).run();
        if (!result.ok) {
            std::cerr << "error: " << result.error << std::endl;
            return 1;
        }
        return 0;
    }
    // No destination: like `curl URL`, the body goes to stdout.
    httpp::response res = httpp::client::fetch(url);
    if (!res.ok()) {
        std::cerr << "error: request failed with status " << res.status << std::endl;
        return 1;
    }
    std::fwrite(res.body.data(), 1, res.body.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
    return 0;
}

int cmd_curl(const std::string& url, const std::string& method, const std::vector<std::string>& headers, const std::string& data,
             bool has_data, const std::string& output) {
    httpp::client::request req(url);
    if (!method.empty()) req.method(method);
    for (const std::string& h : headers) {
        std::size_t colon = h.find(':');
        if (colon == std::string::npos) {
            std::cerr << "error: malformed header (expected 'Name: value'): '" << h << "'" << std::endl;
            return 1;
        }
        auto trim = [](std::string s) {
            const char* ws = " \t\r\n";
            std::size_t b = s.find_first_not_of(ws);
            if (b == std::string::npos) return std::string();
            return s.substr(b, s.find_last_not_of(ws) - b + 1);
        };
        req.header(trim(h.substr(0, colon)), trim(h.substr(colon + 1)));
    }
    if (has_data) req.data(data);

    httpp::response res = req.run();
    if (!output.empty()) {
        std::ofstream out(output, std::ios::binary);
        if (!out) {
            std::cerr << "error: cannot write '" << output << "'" << std::endl;
            return 1;
        }
        out.write(res.body.data(), static_cast<std::streamsize>(res.body.size()));
    } else {
        std::fwrite(res.body.data(), 1, res.body.size(), stdout);
        std::fputc('\n', stdout);
        std::fflush(stdout);
    }
    if (!res.ok()) {
        std::cerr << "error: request failed with status " << res.status << std::endl;
        return 1;
    }
    return 0;
}

// --- `httpp py ...`: hand the rest to the Python half ---

// The directory of this executable, with a trailing separator, or "" if it cannot be told.
std::string exe_dir(const char* argv0) {
    std::string path;
#if defined(_WIN32)
    char buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameA(nullptr, buf, static_cast<DWORD>(sizeof buf));
    if (n > 0 && n < sizeof buf) path.assign(buf, n);
#elif defined(__APPLE__)
    char buf[4096];
    uint32_t size = sizeof buf;
    if (_NSGetExecutablePath(buf, &size) == 0) path = buf;
#else
    char buf[4096];
    ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n > 0) path.assign(buf, static_cast<std::size_t>(n));
#endif
    if (path.empty() && argv0 && std::strchr(argv0, '/')) path = argv0;
    std::size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}

bool is_executable_file(const std::string& path) {
#ifdef _WIN32
    return _access(path.c_str(), 0) == 0;
#else
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && ::access(path.c_str(), X_OK) == 0;
#endif
}

#ifdef _WIN32
// One argument in the form CommandLineToArgvW and the C runtime read back (the CRT does not quote for us).
std::string quote_windows_arg(const std::string& arg) {
    if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string::npos) return arg;
    std::string out = "\"";
    std::size_t backslashes = 0;
    for (char c : arg) {
        if (c == '\\') {
            ++backslashes;
        } else if (c == '"') {
            out.append(backslashes * 2 + 1, '\\');
            out += '"';
            backslashes = 0;
        } else {
            out.append(backslashes, '\\');
            out += c;
            backslashes = 0;
        }
    }
    out.append(backslashes * 2, '\\');
    out += '"';
    return out;
}
#endif

// Runs `python -m httpp py <args>`; returns the exit code (on POSIX it only returns when the interpreter could not be started).
// Which Python: $HTTPP_PYTHON (httpp/httpp_cli.py sets it to its own interpreter when it starts this executable), else python3 or python
// next to this executable (an SDK install has none), else python3 or python on PATH.
int run_python(const char* argv0, const std::vector<std::string>& args) {
    std::vector<std::string> candidates;
    const char* env = std::getenv("HTTPP_PYTHON");
    if (env && *env) candidates.push_back(env);
    const std::string dir = exe_dir(argv0);
    if (!dir.empty()) {
#ifdef _WIN32
        candidates.push_back(dir + "python.exe");
#else
        candidates.push_back(dir + "python3");
        candidates.push_back(dir + "python");
#endif
    }
    for (const char* name : {"python3", "python"}) candidates.push_back(name);

    for (const std::string& py : candidates) {
        const bool bare = py.find_first_of("/\\") == std::string::npos;  // a bare name is looked up on PATH
        if (!bare && !is_executable_file(py)) {
            if (env && *env && py == env) return fail("HTTPP_PYTHON is set to '" + py + "', which is not an executable file");
            continue;
        }
        std::vector<std::string> argv_store{py, "-m", "httpp", "py"};
        argv_store.insert(argv_store.end(), args.begin(), args.end());
#ifdef _WIN32
        std::vector<std::string> quoted;
        for (const std::string& a : argv_store) quoted.push_back(quote_windows_arg(a));
        std::vector<const char*> argv_c;
        for (const std::string& a : quoted) argv_c.push_back(a.c_str());
        argv_c.push_back(nullptr);
        intptr_t code = bare ? _spawnvp(_P_WAIT, py.c_str(), argv_c.data()) : _spawnv(_P_WAIT, py.c_str(), argv_c.data());
        if (code == -1) continue;
        return static_cast<int>(code);
#else
        std::vector<char*> argv_c;
        for (std::string& a : argv_store) argv_c.push_back(&a[0]);
        argv_c.push_back(nullptr);
        if (bare) ::execvp(py.c_str(), argv_c.data());
        else ::execv(py.c_str(), argv_c.data());
        // exec failed (not found, not permitted): try the next candidate
#endif
    }
    std::cerr << "httpp: error: py commands need a Python with httpp installed (pip install httpp); none was found.\n"
                 "Set HTTPP_PYTHON to the interpreter to use." << std::endl;
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    // `py` and the two names that used to be top-level Python commands are decided before the parser sees anything, so the
    // arguments of a Python command reach Python untouched (even `--help`, even a value that looks like one of our options).
    if (argc >= 2) {
        const std::string first = argv[1];
        if (first == "py") {
            std::vector<std::string> rest(argv + 2, argv + argc);
            if (rest.empty()) {
                std::cerr << "usage: httpp py COMMAND [ARGS...]\n"
                             "Python commands: run, path. Try: httpp py --help" << std::endl;
                return 2;
            }
            return run_python(argv[0], rest);
        }
        if (first == "run" || first == "path") {
            std::cerr << "httpp: '" << first << "' is a Python command: use `httpp py " << first << "`" << std::endl;
            return 2;
        }
    }

    CLI::App app{"httpp: a lightweight HTTP client + server toolkit."};
    app.set_version_flag("--version", std::string("httpp ") + k_version);
    app.require_subcommand(0, 1);
    app.footer("Python commands need a Python with httpp installed and go behind py, for example:\n"
               "httpp py run main:app --reload\n"
               "httpp py path --include-dir\n"
               "httpp py --help");

    // server
    std::string server_dir = ".", server_host = "0.0.0.0";
    int server_port = 8000;
    double server_grace = 10;
    auto* c_server = app.add_subcommand("server", "Serve a directory over HTTP, like `python -m http.server`.");
    c_server->add_option("directory", server_dir, "The folder to serve")->capture_default_str();
    c_server->add_option("--host", server_host)->capture_default_str();
    c_server->add_option("--port,-p", server_port)->check(CLI::Range(1, 65535))->capture_default_str();
    c_server->add_option("--shutdown-timeout", server_grace, "On Ctrl+C or SIGTERM, how long requests in progress may take to finish")
        ->check(CLI::NonNegativeNumber)->capture_default_str()->type_name("SECONDS");

    // browse
    browse_args b;
    auto* c_browse = app.add_subcommand("browse", "Browse a directory in the web browser (read-only: list, open, download).");
    c_browse->add_option("directory", b.directory, "The folder to browse")->capture_default_str();
    c_browse->add_option("--host", b.host, "Use 0.0.0.0 to share on every interface (there is no login)")->capture_default_str();
    c_browse->add_option("--port,-p", b.port)->check(CLI::Range(1, 65535))->capture_default_str();
    c_browse->add_option("--title", b.options.title)->capture_default_str();
    c_browse->add_option("--theme", b.options.theme)->check(CLI::IsMember({"auto", "light", "dark"}))->capture_default_str();
    c_browse->add_option("--accent", b.options.accent, "Accent colour")->type_name("#RRGGBB");
    c_browse->add_flag("--no-search", b.no_search, "Hide the filter box");
    c_browse->add_flag("--hidden", b.options.show_hidden, "List and serve names that start with a dot");
    c_browse->add_option("--max-entries", b.options.max_entries, "Longest folder listing")->check(CLI::PositiveNumber)->capture_default_str()->type_name("N");
    c_browse->add_option("--view", b.options.view)->check(CLI::IsMember({"list", "grid"}))->capture_default_str();
    c_browse->add_option("--sort", b.options.sort)->check(CLI::IsMember({"name", "size", "date"}))->capture_default_str();
    c_browse->add_option("--shutdown-timeout", b.grace, "On Ctrl+C or SIGTERM, how long requests in progress may take to finish")
        ->check(CLI::NonNegativeNumber)->capture_default_str()->type_name("SECONDS");

    // download
    std::string dl_url, dl_output;
    bool dl_quiet = false;
    auto* c_download = app.add_subcommand("download", "Download a URL (to stdout, or a file with -o).");
    c_download->add_option("url", dl_url)->required();
    c_download->add_option("--output,-o", dl_output, "Save to this file (shows a progress bar) instead of printing to stdout");
    c_download->add_flag("--quiet,-q", dl_quiet, "With -o, suppress the progress bar");

    // curl
    std::string curl_url, curl_method, curl_data, curl_output;
    std::vector<std::string> curl_headers;
    auto* c_curl = app.add_subcommand("curl", "Make a request (limited curl-like subset: -X/-H/-d).");
    c_curl->add_option("url", curl_url)->required();
    c_curl->add_option("--request,-X", curl_method, "HTTP method (default: GET, or POST if -d is given)");
    c_curl->add_option("--header,-H", curl_headers, "Extra header 'Name: value' (repeatable)")->type_name("HEADER");
    auto* curl_data_opt = c_curl->add_option("--data,-d", curl_data, "Request body");
    c_curl->add_option("--output,-o", curl_output, "Write the response body to this file instead of stdout");

    // `py` exists for --help only: the real work is done before parsing.
    app.add_subcommand("py", "Python commands: run, path (need a Python with httpp installed). Everything after `py` goes to Python.");

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        int code = app.exit(e);
        return code == 0 ? 0 : 2;
    }

    if (c_server->parsed()) return cmd_server(server_dir, server_host, server_port, server_grace);
    if (c_browse->parsed()) return cmd_browse(b);
    if (c_download->parsed()) return cmd_download(dl_url, dl_output, dl_quiet);
    if (c_curl->parsed()) return cmd_curl(curl_url, curl_method, curl_headers, curl_data, curl_data_opt->count() > 0, curl_output);

    std::cout << app.help() << std::flush;
    return 1;
}

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
// macOS 10.13 limits apply here too: no std::filesystem, the file system goes through httpp::path (ADR-0054, 0058).

#include "CLI11.hpp"

#include "httpp.h"
#include "httpp/client.hpp"
#include "httpp/download.hpp"
#include "httpp/file_browser.hpp"
#include "httpp/logging.hpp"
#include "httpp/server.hpp"
#include "httpp/url.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <initializer_list>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#  include <fcntl.h>
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

bool is_directory(const std::string& dir) { return httpp::path(dir).is_dir(); }

// The absolute form of a directory that exists, for the "Browsing ..." line (the library resolves it again itself).
std::string absolute_path(const std::string& dir) {
    const std::string real = httpp::path(dir).resolve().str();
    return real.empty() ? dir : real;
}

// The native commands log like `httpp py run`: info by default (the listening line, then one line per request), and
// --log-level changes it (ADR-0011). The library itself stays at warning for listen(); only these commands raise it.
const std::initializer_list<std::string> k_log_levels = {"trace", "debug", "info", "warning", "error", "off"};

// What `server` and `browse` do once their server is built: set the log level, show the banner, bind (so the address is known), say where it is,
// then serve until Ctrl+C or SIGTERM. All of it is calls into the library.
int run_server(httpp::server& srv, const std::string& what, const std::string& host, int port, double grace, bool banner, const std::string& log_level) {
    httpp::set_log_level(log_level);  // the option values are checked by CLI11 against k_log_levels
    if (banner && log_level != "off") httpp::print_banner();  // a terminal only: a pipe, a log file and a CI run never see it
    if (!srv.bind(host, port)) return fail("cannot listen on " + host + ":" + std::to_string(port));
    std::cout << what << " at " << srv.url() << std::endl;
    return srv.listen_until_signal(std::chrono::milliseconds(static_cast<long long>(grace * 1000.0))) ? 0 : 1;
}

// HTTPS for the two commands that stay up (server, browse), with the same option names as `httpp run`. The password of an encrypted key
// can come from the environment so it stays out of the process list.
struct tls_args {
    std::string certfile;
    std::string keyfile;
    std::string keyfile_password;
    bool on() const { return !certfile.empty(); }
};

void add_tls_options(CLI::App* cmd, tls_args& t) {
    CLI::Validator existing_file = CLI::ExistingFile;
    existing_file.description("");  // otherwise the help says FILE:FILE
    auto* cert = cmd->add_option("--ssl-certfile", t.certfile, "Serve https: the certificate chain (PEM, leaf first)")
                     ->type_name("FILE")->check(existing_file);
    auto* key = cmd->add_option("--ssl-keyfile", t.keyfile, "The private key (PEM) of --ssl-certfile")
                    ->type_name("FILE")->check(existing_file);
    cert->needs(key);
    key->needs(cert);
    cmd->add_option("--ssl-keyfile-password", t.keyfile_password, "The password of an encrypted --ssl-keyfile (or set HTTPP_SSL_KEYFILE_PASSWORD, which keeps it out of the process list)")
        ->type_name("PASSWORD")->envname("HTTPP_SSL_KEYFILE_PASSWORD");
}

struct server_args {
    std::string directory = ".";
    std::string host = "0.0.0.0";
    int port = 8000;
    double grace = 10;
    std::string log_level = "info";
    bool hidden = false;
    bool no_dates = false;
    bool no_banner = false;
    tls_args tls;
};

int cmd_server(const server_args& a) {
    if (!is_directory(a.directory)) return fail("'" + a.directory + "' is not a folder");
    httpp::server srv;
    try {
        // The same as serve_directory("/", dir), with the two privacy settings: names that start with a dot answer 404 unless --hidden, and
        // --no-dates sends no file times.
        httpp::directory(httpp::path(a.directory)).set_show_hidden(a.hidden).set_show_dates(!a.no_dates).mount(srv, "/");
        if (a.tls.on()) srv.set_tls(httpp::path(a.tls.certfile), httpp::path(a.tls.keyfile), a.tls.keyfile_password);
    } catch (const std::exception& e) {
        return fail(e.what());
    }
    return run_server(srv, "Serving '" + a.directory + "'", a.host, a.port, a.grace, !a.no_banner, a.log_level);
}

struct browse_args {
    std::string directory = ".";
    std::string host = "127.0.0.1";
    int port = 8000;
    double grace = 10;
    std::string title = "Files";
    std::string theme = "auto";
    std::string accent;
    bool hidden = false;
    std::size_t max_entries = 5000;
    std::string view = "list";
    std::string sort = "name";
    std::string log_level = "info";
    bool no_search = false;
    bool safe = false;
    bool no_zip = false;
    bool no_powered_by = false;
    bool no_dates = false;
    bool no_banner = false;
    tls_args tls;
};

int cmd_browse(const browse_args& a) {
    if (!is_directory(a.directory)) return fail("'" + a.directory + "' is not a folder");
    const std::string root = absolute_path(a.directory);
    httpp::server srv;
    try {
        using fb = httpp::file_browser;
        // The option values are checked by CLI11 against these names before this runs.
        const fb::theme_mode theme = a.theme == "light" ? fb::theme_mode::light : a.theme == "dark" ? fb::theme_mode::dark : fb::theme_mode::system;
        const fb::view_mode view = a.view == "grid" ? fb::view_mode::grid : fb::view_mode::list;
        const fb::sort_key sort = a.sort == "size" ? fb::sort_key::size : a.sort == "date" ? fb::sort_key::date : fb::sort_key::name;
        fb(root).set_title(a.title).set_theme(theme).set_accent(a.accent).set_search(!a.no_search).set_show_hidden(a.hidden).set_show_dates(!a.no_dates)
            .set_max_entries(a.max_entries).set_view(view).set_sort(sort).set_safe_mode(a.safe).set_zip(!a.no_zip).set_powered_by(!a.no_powered_by).mount(srv, "/");
        if (a.tls.on()) srv.set_tls(httpp::path(a.tls.certfile), httpp::path(a.tls.keyfile), a.tls.keyfile_password);
    } catch (const std::exception& e) {
        return fail(e.what());
    }
    return run_server(srv, "Browsing '" + root + "' (read-only)", a.host, a.port, a.grace, !a.no_banner, a.log_level);
}

// Standard output as plain bytes. On Windows a text-mode stdout turns every "\n" into "\r\n", which corrupts a body.
void stdout_binary() {
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif
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
    stdout_binary();
    std::fwrite(res.body.data(), 1, res.body.size(), stdout);
    std::fflush(stdout);
    return 0;
}

std::string trim_ws(const std::string& s) {
    const char* ws = " \t\r\n";
    std::size_t b = s.find_first_not_of(ws);
    if (b == std::string::npos) return std::string();
    return s.substr(b, s.find_last_not_of(ws) - b + 1);
}

// The status line and the headers of a response, ended by an empty line, the way they come off the wire. The engine speaks
// HTTP/1.1 and the response carries neither a version nor a reason phrase, so the line is "HTTP/1.1 <status>".
std::string head_text(const httpp::response& res) {
    std::string out = "HTTP/1.1 " + std::to_string(res.status) + "\r\n";
    for (const auto& [name, value] : res.headers) out += name + ": " + value + "\r\n";
    out += "\r\n";
    return out;
}

// Everything `httpp curl` takes from the command line.
struct curl_args {
    std::string url, method, data, output, user;
    std::vector<std::string> headers;
    bool has_data = false;
    bool has_user = false;
    bool include_headers = false; // -i
    bool head_only = false;       // -I
    bool verbose = false;         // -v
    bool location = false;        // -L
    long max_time = 0;            // -m, in seconds; 0 when not given
};

int cmd_curl(const curl_args& a) {
    httpp::request req(a.url);

    // The method that will be sent, as curl picks it: -X wins, then -I, then POST when there is a body.
    std::string effective = a.method;
    if (effective.empty()) effective = a.head_only ? "HEAD" : (a.has_data ? "POST" : "GET");
    if (!a.method.empty() || a.head_only) req.method(effective);

    std::vector<std::pair<std::string, std::string>> sent;
    for (const std::string& h : a.headers) {
        std::size_t colon = h.find(':');
        if (colon == std::string::npos) {
            std::cerr << "error: malformed header (expected 'Name: value'): '" << h << "'" << std::endl;
            return 1;
        }
        sent.emplace_back(trim_ws(h.substr(0, colon)), trim_ws(h.substr(colon + 1)));
        req.header(sent.back().first, sent.back().second);
    }
    if (a.has_data) req.data(a.data);
    if (a.location) req.follow_redirects(true);
    if (a.max_time > 0) req.timeout(a.max_time);
    if (a.has_user) {
        std::size_t colon = a.user.find(':'); // only the first colon splits: a password may contain more
        if (colon == std::string::npos) {
            std::cerr << "error: malformed --user (expected 'user:password')" << std::endl;
            return 1;
        }
        req.basic_auth(a.user.substr(0, colon), a.user.substr(colon + 1));
    }

    if (a.verbose) {
        httpp::url target = httpp::parse_url(a.url);
        std::string path = httpp::url_path(target);
        std::string query = httpp::url_query(target);
        if (!query.empty()) path += "?" + query;
        std::cerr << "> " << effective << " " << path << " HTTP/1.1\n";
        for (const auto& [name, value] : sent) std::cerr << "> " << name << ": " << value << "\n";
        std::cerr << ">" << std::endl;
    }

    httpp::response res = req.run();

    if (a.verbose && res.status != 0) {
        std::cerr << "< HTTP/1.1 " << res.status << "\n";
        for (const auto& [name, value] : res.headers) std::cerr << "< " << name << ": " << value << "\n";
        std::cerr << "<" << std::endl;
    }

    // -i and -I put the head in front of the body (-I has no body). A failed transfer has no head to show.
    std::string out;
    if ((a.include_headers || a.head_only) && res.status != 0) out += head_text(res);
    if (!a.head_only) out += res.body;

    if (!a.output.empty()) {
        std::ofstream file(httpp::path(a.output).native(), std::ios::binary);
        if (!file) {
            std::cerr << "error: cannot write '" << a.output << "'" << std::endl;
            return 1;
        }
        file.write(out.data(), static_cast<std::streamsize>(out.size()));
    } else {
        stdout_binary();
        std::fwrite(out.data(), 1, out.size(), stdout);
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

bool is_executable_file(const std::string& file) { return httpp::path(file).is_executable(); }

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
    server_args sv;
    auto* c_server = app.add_subcommand("server", "Serve a directory over HTTP, like `python -m http.server`.");
    c_server->add_option("directory", sv.directory, "The folder to serve")->capture_default_str();
    c_server->add_option("--host", sv.host, "Use 127.0.0.1 to keep it on this computer (the default is every interface, and there is no login)")->capture_default_str();
    c_server->add_option("--port,-p", sv.port)->check(CLI::Range(1, 65535))->capture_default_str();
    c_server->add_flag("--hidden", sv.hidden, "Serve names that start with a dot too (otherwise they answer 404)");
    c_server->add_flag("--no-dates", sv.no_dates, "Send no file times: no Last-Modified and no ETag (every answer says Cache-Control: no-cache)");
    c_server->add_flag("--no-banner", sv.no_banner, "Do not print the banner on a terminal");
    add_tls_options(c_server, sv.tls);
    c_server->add_option("--shutdown-timeout", sv.grace, "On Ctrl+C or SIGTERM, how long requests in progress may take to finish")
        ->check(CLI::NonNegativeNumber)->capture_default_str()->type_name("SECONDS");
    c_server->add_option("--log-level", sv.log_level, "What goes to stderr: info is the address and one line per request, off is nothing")
        ->check(CLI::IsMember(k_log_levels))->capture_default_str();

    // browse
    browse_args b;
    auto* c_browse = app.add_subcommand("browse", "Browse a directory in the web browser (read-only: list, open, download).");
    c_browse->add_option("directory", b.directory, "The folder to browse")->capture_default_str();
    c_browse->add_option("--host", b.host, "Use 0.0.0.0 to share on every interface (there is no login)")->capture_default_str();
    c_browse->add_option("--port,-p", b.port)->check(CLI::Range(1, 65535))->capture_default_str();
    c_browse->add_option("--title", b.title)->capture_default_str();
    c_browse->add_option("--theme", b.theme)->check(CLI::IsMember({"auto", "light", "dark"}))->capture_default_str();
    c_browse->add_option("--accent", b.accent, "Accent colour")->type_name("#RRGGBB");
    c_browse->add_flag("--no-search", b.no_search, "Hide the filter box");
    c_browse->add_flag("--hidden", b.hidden, "List and serve names that start with a dot");
    c_browse->add_flag("--safe", b.safe, "Only download html and svg files, never show them (default: shown, sandboxed so no script runs and nothing outside is fetched)");
    c_browse->add_flag("--no-zip", b.no_zip, "Do not offer selecting files and downloading them as one ZIP (it is on here; the library default is off)");
    c_browse->add_flag("--no-dates", b.no_dates, "Show and send no file times: no Modified column, no sort by date, no Last-Modified or ETag, ZIP entries dated 1980-01-01");
    c_browse->add_flag("--no-banner", b.no_banner, "Do not print the banner on a terminal");
    add_tls_options(c_browse, b.tls);
    c_browse->add_flag("--no-powered-by", b.no_powered_by, "Hide the \"Powered by httpp\" line at the bottom of the page");
    c_browse->add_option("--max-entries", b.max_entries, "Longest folder listing")->check(CLI::PositiveNumber)->capture_default_str()->type_name("N");
    c_browse->add_option("--view", b.view)->check(CLI::IsMember({"list", "grid"}))->capture_default_str();
    c_browse->add_option("--sort", b.sort)->check(CLI::IsMember({"name", "size", "date"}))->capture_default_str();
    c_browse->add_option("--log-level", b.log_level, "What goes to stderr: info is the address and one line per request, off is nothing")
        ->check(CLI::IsMember(k_log_levels))->capture_default_str();
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
    curl_args ca;
    auto* c_curl = app.add_subcommand("curl", "Make a request (a small curl-like subset: -X -H -d -o -i -I -v -L -m -u).");
    c_curl->add_option("url", ca.url)->required();
    c_curl->add_option("--request,-X", ca.method, "HTTP method (default: GET, or POST if -d is given, or HEAD with -I)");
    c_curl->add_option("--header,-H", ca.headers, "Extra header 'Name: value' (repeatable)")->type_name("HEADER");
    auto* curl_data_opt = c_curl->add_option("--data,-d", ca.data, "Request body");
    c_curl->add_option("--output,-o", ca.output, "Write the response body to this file instead of stdout");
    c_curl->add_flag("--include,-i", ca.include_headers, "Print the status line and the response headers before the body");
    c_curl->add_flag("--head,-I", ca.head_only, "Send HEAD and print only the status line and the headers");
    c_curl->add_flag("--verbose,-v", ca.verbose, "Print the request line and the -H headers, then the response status and headers, to stderr");
    c_curl->add_flag("--location,-L", ca.location, "Follow redirects");
    c_curl->add_option("--max-time,-m", ca.max_time, "Give up after this many seconds")->check(CLI::Range(1L, 2147483647L))->type_name("SECONDS");
    auto* curl_user_opt = c_curl->add_option("--user,-u", ca.user, "Basic authentication as 'user:password'")->type_name("USER:PASSWORD");

    // `py` exists for --help only: the real work is done before parsing.
    app.add_subcommand("py", "Python commands: run, path (need a Python with httpp installed). Everything after `py` goes to Python.");

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        int code = app.exit(e);
        return code == 0 ? 0 : 2;
    }

    if (c_server->parsed()) return cmd_server(sv);
    if (c_browse->parsed()) return cmd_browse(b);
    if (c_download->parsed()) return cmd_download(dl_url, dl_output, dl_quiet);
    if (c_curl->parsed()) {
        ca.has_data = curl_data_opt->count() > 0;
        ca.has_user = curl_user_opt->count() > 0;
        return cmd_curl(ca);
    }

    std::cout << app.help() << std::flush;
    return 1;
}

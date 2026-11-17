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

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>
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
// The curl release whose behaviour `httpp curl` follows: it is in the default User-Agent and in `httpp curl --version`.
const char* const k_curl_compat_version = "8.18.0";
const char* const k_version =HTTPP_CLI_STR(HTTPP_VERSION_MAJOR) "." HTTPP_CLI_STR(HTTPP_VERSION_MINOR) "." HTTPP_CLI_STR(HTTPP_VERSION_PATCH);

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

bool same_header_name(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

// Everything `httpp curl` takes from the command line.
struct curl_args {
    std::vector<std::string> urls;
    std::vector<std::string> outputs; // every -o; the n-th goes with the n-th URL
    std::string method, user, user_agent, referer, write_out, upload_file, dump_header;
    std::vector<std::string> headers;
    std::vector<std::string> data; // every -d; joined with '&' like curl; "@file" reads a file
    bool has_user = false;
    bool remote_name = false;     // -O
    bool silent = false;          // -s
    bool show_error = false;      // -S
    bool compressed = false;      // --compressed: accepted, the client always asks for gzip and deflate
    bool fail = false;            // -f
    bool include_headers = false; // -i
    bool head_only = false;       // -I
    bool verbose = false;         // -v
    bool location = false;        // -L
    bool retry_connrefused = false;
    long max_time = 0;            // -m, in seconds; 0 when not given
    long connect_timeout = 0;     // --connect-timeout, in seconds; 0 when not given
    int max_redirs = -1;          // --max-redirs; -1 when not given
    int retry = 0;                // --retry
    long retry_delay = 0;         // --retry-delay, in seconds; 0 = 1, 2, 4, ... up to 10 minutes
};

// What the command works out once, before the first transfer.
struct curl_plan {
    std::vector<std::pair<std::string, std::string>> sent; // headers from -H, -A, -e and --compressed
    bool has_body = false;
    std::string body;
    std::string user_name, user_password;
    std::ofstream dump_file; // -D FILE
    bool dump_to_stdout = false;
};

// What went wrong with a transfer, in a few words, for the "error:" line.
const char* error_label(httpp::error_kind kind) {
    switch (kind) {
        case httpp::error_kind::invalid_url: return "invalid URL";
        case httpp::error_kind::unsupported_method: return "unsupported method";
        case httpp::error_kind::connection: return "connection failed";
        case httpp::error_kind::timeout: return "timed out";
        case httpp::error_kind::tls: return "TLS failed";
        case httpp::error_kind::too_many_redirects: return "too many redirects";
        case httpp::error_kind::too_large: return "response too large";
        case httpp::error_kind::canceled: return "canceled";
        default: return "request failed";
    }
}

// The exit code curl uses for the same trouble, so a script can tell them apart.
int curl_exit_code(httpp::error_kind kind) {
    switch (kind) {
        case httpp::error_kind::invalid_url: return 3;          // CURLE_URL_MALFORMAT
        case httpp::error_kind::connection: return 7;           // CURLE_COULDNT_CONNECT (a failed lookup lands here too)
        case httpp::error_kind::timeout: return 28;             // CURLE_OPERATION_TIMEDOUT
        case httpp::error_kind::tls: return 35;                 // CURLE_SSL_CONNECT_ERROR
        case httpp::error_kind::too_many_redirects: return 47;  // CURLE_TOO_MANY_REDIRECTS
        case httpp::error_kind::too_large: return 63;           // CURLE_FILESIZE_EXCEEDED
        default: return 1;
    }
}

constexpr int curl_exit_http_error = 22; // CURLE_HTTP_RETURNED_ERROR, with -f
constexpr int curl_exit_write_error = 23; // CURLE_WRITE_ERROR: a file could not be written or named
constexpr int curl_exit_read_error = 26;  // CURLE_READ_ERROR: a file for -d or -T could not be read

// "error: ..." on stderr, unless -s hides it and -S does not bring it back.
void curl_error(const curl_args& a, const std::string& message) {
    if (!a.silent || a.show_error) std::cerr << "error: " << message << std::endl;
}

void stdin_binary() {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
#endif
}

// A whole file, or standard input for "-". False when it cannot be read.
bool read_whole(const std::string& path, std::string& out) {
    if (path == "-") {
        stdin_binary();
        out.assign(std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>());
        return true;
    }
    std::ifstream in(httpp::path(path).native(), std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

std::string file_base_name(const std::string& path) {
    std::size_t cut = path.find_last_of("/\\");
    return cut == std::string::npos ? path : path.substr(cut + 1);
}

// The file name -O uses: the last piece of the URL path. Empty when there is none to use.
std::string remote_file_name(const std::string& url) {
    try {
        std::string path = httpp::url_path(httpp::parse_url(url));
        std::size_t slash = path.rfind('/');
        std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
        if (name == "." || name == ".." || name.find('\\') != std::string::npos) return std::string();
        return name;
    } catch (const std::exception&) {
        return std::string();
    }
}

// -T: a URL whose path ends with a slash gets the file's name added, as curl does.
std::string with_upload_name(const std::string& url, const std::string& name) {
    std::size_t cut = url.find_first_of("?#");
    std::string head = url.substr(0, cut);
    std::string tail = cut == std::string::npos ? std::string() : url.substr(cut);
    if (name.empty() || head.empty() || head.back() != '/') return url;
    return head + name + tail;
}

// curl reads a URL with no "://" as http://, so `httpp curl parrot.live` and `httpp curl localhost:8080/x` work.
std::string with_default_scheme(const std::string& url) {
    return url.find("://") == std::string::npos ? "http://" + url : url;
}

// curl rejects a URL with a space or a control character (exit 3) instead of sending a broken request line.
bool url_has_unsafe_character(const std::string& url) {
    return std::any_of(url.begin(), url.end(), [](unsigned char c) { return c <= 0x20 || c == 0x7f; });
}

std::string response_header(const httpp::response& res, const char* name) {
    for (const auto& [key, value] : res.headers)
        if (same_header_name(key, name)) return value;
    return std::string();
}

// -w: %{http_code} %{content_type} %{size_download} %{exitcode} %{errormsg} %{url}, and \n \r \t \\ %%.
// A %{name} that is not one of these stays as typed. size_download is the size of the body after decoding.
std::string expand_write_out(const std::string& format, const std::string& url, const httpp::response& res, int exit_code,
                             const std::string& error_text) {
    std::string out;
    for (std::size_t i = 0; i < format.size(); ++i) {
        char c = format[i];
        if (c == '\\' && i + 1 < format.size()) {
            char next = format[++i];
            if (next == 'n') out += '\n';
            else if (next == 'r') out += '\r';
            else if (next == 't') out += '\t';
            else if (next == '\\') out += '\\';
            else { out += '\\'; out += next; }
        } else if (c == '%' && i + 1 < format.size() && format[i + 1] == '%') {
            out += '%';
            ++i;
        } else if (c == '%' && i + 1 < format.size() && format[i + 1] == '{') {
            std::size_t close = format.find('}', i + 2);
            std::string name = close == std::string::npos ? std::string() : format.substr(i + 2, close - i - 2);
            std::string value;
            bool known = true;
            if (name == "http_code") {
                value = std::to_string(res.status);
                while (value.size() < 3) value.insert(value.begin(), '0'); // 000 when there was no answer
            } else if (name == "content_type") value = response_header(res, "content-type");
            else if (name == "size_download") value = std::to_string(res.body.size());
            else if (name == "exitcode") value = std::to_string(exit_code);
            else if (name == "errormsg") value = error_text;
            else if (name == "url") value = url;
            else known = false;
            if (known) {
                out += value;
                i = close;
            } else {
                out += c;
            }
        } else {
            out += c;
        }
    }
    return out;
}

// Only a few statuses and a timeout are worth another try, as in curl; a refused connection only with --retry-connrefused.
bool is_transient(const curl_args& a, const httpp::response& res) {
    if (res.failed()) {
        return res.error == httpp::error_kind::timeout || (a.retry_connrefused && res.error == httpp::error_kind::connection);
    }
    return res.status == 408 || res.status == 429 || res.status == 500 || res.status == 502 || res.status == 503 || res.status == 504;
}

void write_stdout(const std::string& bytes) {
    stdout_binary();
    std::fwrite(bytes.data(), 1, bytes.size(), stdout);
    std::fflush(stdout);
}

// One transfer. Returns its exit code.
int curl_transfer(const curl_args& a, curl_plan& plan, const std::string& given_url, const std::string& output) {
    std::string url = with_default_scheme(given_url);
    if (!a.upload_file.empty() && a.upload_file != "-") url = with_upload_name(url, file_base_name(a.upload_file));

    // The method that will be sent, as curl picks it: -X wins, then -I, then PUT for -T, then POST when there is a body.
    std::string chosen = a.method;
    if (chosen.empty() && a.head_only) chosen = "HEAD";
    if (chosen.empty() && !a.upload_file.empty()) chosen = "PUT";
    const std::string effective = chosen.empty() ? (plan.has_body ? "POST" : "GET") : chosen;

    auto fill = [&](httpp::request& req) {
        if (!chosen.empty()) req.method(chosen);
        for (const auto& [name, value] : plan.sent) req.header(name, value);
        if (plan.has_body) req.data(plan.body);
        if (a.location) req.follow_redirects(true);
        if (a.max_time > 0) req.timeout(a.max_time);
        if (a.connect_timeout > 0) req.connect_timeout(a.connect_timeout);
        if (a.max_redirs >= 0) req.max_redirects(a.max_redirs);
        if (a.has_user) req.basic_auth(plan.user_name, plan.user_password);
    };

    const bool malformed = url_has_unsafe_character(url);

    if (a.verbose && !malformed) {
        std::string path = url; // shown as typed when it does not parse; the request then fails below
        try {
            httpp::url target = httpp::parse_url(url);
            path = httpp::url_path(target);
            std::string query = httpp::url_query(target);
            if (!query.empty()) path += "?" + query;
        } catch (const std::exception&) {
        }
        std::cerr << "> " << effective << " " << path << " HTTP/1.1\n";
        for (const auto& [name, value] : plan.sent) std::cerr << "> " << name << ": " << value << "\n";
        std::cerr << ">" << std::endl;
    }

    auto attempt = [&]() {
        if (malformed) { // never sent: not transient, so no retry either
            httpp::response bad;
            bad.error = httpp::error_kind::invalid_url;
            bad.error_message = "not a usable http(s) URL: " + given_url;
            return bad;
        }
        httpp::request req(url);
        fill(req);
        return req.run();
    };
    httpp::response res = attempt();
    for (int tried = 0; tried < a.retry && is_transient(a, res); ++tried) {
        long delay = a.retry_delay > 0 ? a.retry_delay : std::min<long>(600, 1L << std::min(tried, 9));
        if (!a.silent) {
            std::cerr << "Warning: transient problem, will retry in " << delay << " seconds (" << (tried + 1) << " of " << a.retry << ")" << std::endl;
        }
        std::this_thread::sleep_for(std::chrono::seconds(delay));
        res = attempt();
    }

    int code = 0;
    std::string error_text;
    if (res.failed()) { // no HTTP answer to show, and no file to leave behind
        error_text = error_label(res.error);
        if (!res.error_message.empty() && res.error_message != error_text) error_text += " (" + res.error_message + ")";
        curl_error(a, error_text);
        code = curl_exit_code(res.error);
    } else {
        if (a.verbose) {
            std::cerr << "< HTTP/1.1 " << res.status << "\n";
            for (const auto& [name, value] : res.headers) std::cerr << "< " << name << ": " << value << "\n";
            std::cerr << "<" << std::endl;
        }
        if (plan.dump_to_stdout) write_stdout(head_text(res));
        else if (plan.dump_file.is_open()) plan.dump_file << head_text(res) << std::flush;

        if (a.fail && res.status >= 400) { // like curl, an HTTP error status is an answer, not a failure, unless -f asks for one
            error_text = "the requested URL returned error: " + std::to_string(res.status);
            curl_error(a, error_text);
            code = curl_exit_http_error;
        } else {
            // -i and -I put the head in front of the body (-I has no body).
            std::string out;
            if (a.include_headers || a.head_only) out += head_text(res);
            if (!a.head_only) out += res.body;

            std::string target = output;
            if (target.empty() && a.remote_name) {
                target = remote_file_name(url);
                if (target.empty()) {
                    error_text = "no file name to use for -O in '" + url + "'";
                    curl_error(a, error_text);
                    code = curl_exit_write_error;
                }
            }
            if (code != 0) {
                // nothing to write
            } else if (!target.empty()) {
                std::ofstream file(httpp::path(target).native(), std::ios::binary);
                if (!file) {
                    error_text = "cannot write '" + target + "'";
                    curl_error(a, error_text);
                    code = curl_exit_write_error;
                } else {
                    file.write(out.data(), static_cast<std::streamsize>(out.size()));
                }
            } else {
                write_stdout(out);
            }
        }
    }

    if (!a.write_out.empty()) write_stdout(expand_write_out(a.write_out, given_url, res, code, error_text));
    return code;
}

int cmd_curl(const curl_args& a) {
    curl_plan plan;

    for (const std::string& h : a.headers) {
        std::size_t colon = h.find(':');
        if (colon == std::string::npos) {
            curl_error(a, "malformed header (expected 'Name: value'): '" + h + "'");
            return 1;
        }
        plan.sent.emplace_back(trim_ws(h.substr(0, colon)), trim_ws(h.substr(colon + 1)));
    }
    // -A and -e are just headers, unless -H already gave one of the same name.
    auto add_unless_given = [&](const char* name, const std::string& value) {
        if (value.empty()) return;
        for (const auto& h : plan.sent) if (same_header_name(h.first, name)) return;
        plan.sent.emplace_back(name, value);
    };
    // Like curl, announce ourselves as curl/<version> unless -A or -H says otherwise: some sites (parrot.live) answer only that.
    add_unless_given("User-Agent", a.user_agent.empty() ? std::string("curl/") + k_curl_compat_version : a.user_agent);
    add_unless_given("Referer", a.referer);

    if (a.has_user) {
        std::size_t colon = a.user.find(':'); // only the first colon splits: a password may contain more
        if (colon == std::string::npos) {
            curl_error(a, "malformed --user (expected 'user:password')");
            return 1;
        }
        plan.user_name = a.user.substr(0, colon);
        plan.user_password = a.user.substr(colon + 1);
    }

    if (!a.upload_file.empty() && !a.data.empty()) {
        curl_error(a, "-T and -d cannot be used together");
        return 1;
    }
    if (!a.upload_file.empty()) {
        if (!read_whole(a.upload_file, plan.body)) {
            curl_error(a, "cannot read '" + a.upload_file + "'");
            return curl_exit_read_error;
        }
        plan.has_body = true;
    } else if (!a.data.empty()) {
        for (const std::string& piece : a.data) {
            std::string value = piece;
            if (!piece.empty() && piece[0] == '@') { // -d @file: the file, without its line breaks (as curl does for -d)
                if (!read_whole(piece.substr(1), value)) {
                    curl_error(a, "cannot read '" + piece.substr(1) + "'");
                    return curl_exit_read_error;
                }
                value.erase(std::remove_if(value.begin(), value.end(), [](char c) { return c == '\r' || c == '\n'; }), value.end());
            }
            plan.body += (plan.body.empty() ? "" : "&") + value;
        }
        plan.has_body = true;
    }

    std::string write_out = a.write_out;
    if (!write_out.empty() && write_out[0] == '@') { // -w @file
        std::string text;
        if (!read_whole(write_out.substr(1), text)) {
            curl_error(a, "cannot read '" + write_out.substr(1) + "'");
            return curl_exit_read_error;
        }
        write_out = text;
    }

    if (a.dump_header == "-") {
        plan.dump_to_stdout = true;
    } else if (!a.dump_header.empty()) {
        plan.dump_file.open(httpp::path(a.dump_header).native(), std::ios::binary);
        if (!plan.dump_file) {
            curl_error(a, "cannot write '" + a.dump_header + "'");
            return curl_exit_write_error;
        }
    }

    // Each URL in turn. A failure does not stop the rest; the first failure is the exit code.
    curl_args run_args = a;
    run_args.write_out = write_out;
    int first_failure = 0;
    for (std::size_t i = 0; i < a.urls.size(); ++i) {
        int code = curl_transfer(run_args, plan, a.urls[i], i < a.outputs.size() ? a.outputs[i] : std::string());
        if (code != 0 && first_failure == 0) first_failure = code;
    }
    return first_failure;
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
    auto* c_curl = app.add_subcommand("curl", "Make requests (a curl-like subset: -X -H -d -o -O -f -s -S -i -I -v -L -m -u -A -e -w -T -D -d @file --compressed --retry; several URLs).");
    c_curl->add_option("url", ca.urls, "URL (several are fetched in turn; a failure does not stop the rest, the first one sets the exit code)")->required();
    c_curl->set_version_flag("--version,-V", std::string("httpp curl ") + k_version + " compatible with curl " + k_curl_compat_version);
    c_curl->add_option("--request,-X", ca.method, "HTTP method (default: GET, or POST if -d is given, PUT with -T, HEAD with -I)");
    c_curl->add_option("--header,-H", ca.headers, "Extra header 'Name: value' (repeatable)")->type_name("HEADER")->allow_extra_args(false);
    c_curl->add_option("--data,-d", ca.data, "Request body (repeatable: joined with '&'; '@file' reads a file without its line breaks, '@-' stdin)")->allow_extra_args(false);
    c_curl->add_option("--output,-o", ca.outputs, "Write the response body to this file instead of stdout (repeatable: the n-th file goes with the n-th URL)")->allow_extra_args(false);
    c_curl->add_flag("--remote-name,-O", ca.remote_name, "Write the body to a file named after the end of the URL path (for URLs without -o)");
    c_curl->add_flag("--silent,-s", ca.silent, "Hide the error line and the retry warnings");
    c_curl->add_flag("--show-error,-S", ca.show_error, "With -s: show the error line anyway");
    c_curl->add_option("--write-out,-w", ca.write_out, "After each transfer print FORMAT: %{http_code} %{content_type} %{size_download} %{exitcode} %{errormsg} %{url}, \\n \\t; '@file' reads it from a file")->type_name("FORMAT");
    c_curl->add_option("--upload-file,-T", ca.upload_file, "Send this file ('-' is stdin) with PUT; a URL ending in '/' gets the file name added")->type_name("FILE");
    c_curl->add_option("--dump-header,-D", ca.dump_header, "Write the status line and headers of each answer to this file ('-' is stdout)")->type_name("FILE");
    c_curl->add_flag("--compressed", ca.compressed, "Accepted for curl scripts: the client always sends 'Accept-Encoding: gzip, deflate' and decodes the answer (an explicit -H wins)");
    c_curl->add_option("--retry", ca.retry, "Try again up to NUM times after a timeout or a 408, 429, 500, 502, 503 or 504")->check(CLI::Range(0, 1000000))->type_name("NUM");
    c_curl->add_option("--retry-delay", ca.retry_delay, "Seconds between retries (default 1, 2, 4, ... up to 600)")->check(CLI::Range(0L, 86400L))->type_name("SECONDS");
    c_curl->add_flag("--retry-connrefused", ca.retry_connrefused, "Also retry when the connection fails (a refused connection, a failed lookup)");
    c_curl->add_flag("--fail,-f", ca.fail, "Exit with 22 and print nothing when the status is 400 or more");
    c_curl->add_flag("--include,-i", ca.include_headers, "Print the status line and the response headers before the body");
    c_curl->add_flag("--head,-I", ca.head_only, "Send HEAD and print only the status line and the headers");
    c_curl->add_flag("--verbose,-v", ca.verbose, "Print the request line and the -H headers, then the response status and headers, to stderr");
    c_curl->add_flag("--location,-L", ca.location, "Follow redirects");
    c_curl->add_option("--max-time,-m", ca.max_time, "Give up when a connect, write or read step takes longer than this many seconds")->check(CLI::Range(1L, 2147483647L))->type_name("SECONDS");
    c_curl->add_option("--connect-timeout", ca.connect_timeout, "Give up when resolving, connecting or the TLS handshake takes longer than this many seconds")
        ->check(CLI::Range(1L, 2147483647L))->type_name("SECONDS");
    c_curl->add_option("--max-redirs", ca.max_redirs, "With -L: fail with exit code 47 after this many redirects (default 20)")
        ->check(CLI::Range(0, 2147483647))->type_name("NUM");
    c_curl->add_option("--user-agent,-A", ca.user_agent, "Send this User-Agent (an explicit -H wins)")->type_name("NAME");
    c_curl->add_option("--referer,-e", ca.referer, "Send this Referer (an explicit -H wins)")->type_name("URL");
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
        ca.has_user = curl_user_opt->count() > 0;
        return cmd_curl(ca);
    }

    std::cout << app.help() << std::flush;
    return 1;
}

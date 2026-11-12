// httpp::mount_file_browser: a read-only file browser. The tests that matter most are the ones that try to leave the served root.
#include "utest/utest.h"
#include "httpp/client.hpp"
#include "httpp/file_browser.hpp"
#include "httpp/server.hpp"
#include "local_server.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#define EXPECT_STR_EQ(a, b) EXPECT_TRUE(std::string(a) == std::string(b))

using testutil::local_server;
namespace fs = std::filesystem;

namespace {

bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

void write_file(const fs::path& p, const std::string& body) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << body;
}

// A served root with a sibling "outside" directory that holds a secret no request may ever return.
struct tree {
    fs::path top, root;
    tree() {
        static int counter = 0;
        top = fs::temp_directory_path() / ("httpp_fb_" + std::to_string(static_cast<long>(std::chrono::steady_clock::now().time_since_epoch().count() % 1000000007)) + "_" + std::to_string(counter++));
        fs::remove_all(top);
        root = top / "root";
        write_file(root / "a.txt", "hello world");
        write_file(root / "sub" / "b.txt", "bee");
        write_file(root / "sub" / "deep" / "c.bin", "cee");
        write_file(root / "evil.html", "<script>alert(1)</script>");
        write_file(root / ".dotfile", "dot");
        write_file(root / ".hidden" / "secret.txt", "hidden secret");
        write_file(top / "outside" / "secret.txt", "TOPSECRET");
#ifndef _WIN32
        fs::create_symlink(top / "outside" / "secret.txt", root / "link_out");
        fs::create_symlink(top / "outside", root / "dir_out");
        fs::create_symlink(root / "sub", root / "link_in");
#endif
    }
    ~tree() { std::error_code ec; fs::remove_all(top, ec); }
};

#ifndef _WIN32
struct raw_reply {
    int status = 0;
    std::string head, body;
    std::string header(const std::string& name) const {
        const std::string key = "\r\n" + name + ": ";
        const std::size_t at = head.find(key);
        if (at == std::string::npos) return "";
        const std::size_t from = at + key.size();
        return head.substr(from, head.find("\r\n", from) - from);
    }
};

// One request written byte for byte (no client-side normalization of "..", "%2e" and friends), read until the server closes.
raw_reply raw_request(int port, const std::string& method, const std::string& target, const std::string& extra = "") {
    raw_reply r;
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<uint16_t>(port));
    ::inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) { ::close(fd); return r; }
    const std::string req = method + " " + target + " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n" + extra +
                            (method == "POST" || method == "PUT" || method == "PATCH" ? "Content-Length: 1\r\n\r\nx" : "\r\n");
    ::send(fd, req.data(), req.size(), 0);
    std::string all;
    char buf[8192];
    for (;;) {
        const ssize_t n = ::recv(fd, buf, sizeof buf, 0);
        if (n <= 0) break;
        all.append(buf, static_cast<std::size_t>(n));
    }
    ::close(fd);
    const std::size_t split = all.find("\r\n\r\n");
    if (split == std::string::npos) return r;
    r.head = all.substr(0, split);
    r.body = all.substr(split + 4);
    if (r.head.size() > 12) r.status = std::atoi(r.head.substr(9, 3).c_str());
    return r;
}
#endif

bool rejects(const std::string& base, const std::string& root, const httpp::file_browser_options& o = {}) {
    httpp::server s;
    try { httpp::mount_file_browser(s, base, root, o); } catch (const std::invalid_argument&) { return true; }
    return false;
}

} // namespace

UTEST(httpp_file_browser, lists_the_root_folders_first_and_hides_dotfiles_and_links_that_leave_the_root) {
    tree t;
    local_server srv([&](httpp::server& s) { httpp::mount_file_browser(s, "/files", t.root.string()); });
    auto r = httpp::client::fetch(srv.url("/files/api/list"));
    ASSERT_EQ(200, r.status);
    EXPECT_TRUE(contains(r.header("Content-Type"), "application/json"));
    EXPECT_TRUE(contains(r.body, "\"name\":\"a.txt\""));
    EXPECT_TRUE(contains(r.body, "\"name\":\"sub\""));
    EXPECT_TRUE(contains(r.body, "\"type\":\"dir\""));
    EXPECT_TRUE(contains(r.body, "\"size\":11"));
    EXPECT_TRUE(contains(r.body, "\"truncated\":false"));
    EXPECT_FALSE(contains(r.body, ".dotfile"));
    EXPECT_FALSE(contains(r.body, ".hidden"));
    EXPECT_FALSE(contains(r.body, "link_out"));
    EXPECT_FALSE(contains(r.body, "dir_out"));
    EXPECT_TRUE(contains(r.body, "\"name\":\"link_in\"")); // a link that stays inside is fine
    EXPECT_LT(r.body.find("\"name\":\"sub\""), r.body.find("\"name\":\"a.txt\"")); // folders come first
    auto sub = httpp::client::fetch(srv.url("/files/api/list?path=sub"));
    ASSERT_EQ(200, sub.status);
    EXPECT_TRUE(contains(sub.body, "\"name\":\"b.txt\""));
    EXPECT_TRUE(contains(sub.body, "\"path\":\"/sub\""));
    auto deep = httpp::client::fetch(srv.url("/files/api/list?path=/sub/deep/"));
    ASSERT_EQ(200, deep.status);
    EXPECT_TRUE(contains(deep.body, "\"name\":\"c.bin\""));
}

UTEST(httpp_file_browser, show_hidden_lists_dotfiles_and_the_default_answers_404_for_them) {
    tree t;
    httpp::file_browser_options shown;
    shown.show_hidden = true;
    local_server hidden([&](httpp::server& s) { httpp::mount_file_browser(s, "/files", t.root.string()); });
    local_server open([&](httpp::server& s) { httpp::mount_file_browser(s, "/files", t.root.string(), shown); });
    EXPECT_EQ(404, httpp::client::fetch(hidden.url("/files/raw/.dotfile")).status);
    EXPECT_EQ(404, httpp::client::fetch(hidden.url("/files/raw/.hidden/secret.txt")).status);
    EXPECT_EQ(404, httpp::client::fetch(hidden.url("/files/api/list?path=.hidden")).status);
    auto list = httpp::client::fetch(open.url("/files/api/list"));
    EXPECT_TRUE(contains(list.body, "\"name\":\".dotfile\""));
    EXPECT_EQ(200, httpp::client::fetch(open.url("/files/raw/.dotfile")).status);
    EXPECT_EQ(200, httpp::client::fetch(open.url("/files/api/list?path=.hidden")).status);
}

#ifndef _WIN32
UTEST(httpp_file_browser, nothing_outside_the_root_is_ever_returned) {
    tree t;
    local_server srv([&](httpp::server& s) { httpp::mount_file_browser(s, "/files", t.root.string()); });
    const std::vector<std::string> targets = {
        "/files/raw/../outside/secret.txt",
        "/files/raw/%2e%2e/outside/secret.txt",
        "/files/raw/%2E%2E/outside/secret.txt",
        "/files/raw/sub/../../outside/secret.txt",
        "/files/raw/sub/%2e%2e/%2e%2e/outside/secret.txt",
        "/files/raw/..%2foutside/secret.txt",
        "/files/raw/..%5coutside%5csecret.txt",
        "/files/raw/..\\outside\\secret.txt",
        "/files/raw/link_out",
        "/files/raw/dir_out/secret.txt",
        "/files/raw//etc/passwd",
        "/files/raw/%2fetc%2fpasswd",
        "/files/raw/a.txt%00.png",
        "/files/api/list?path=..",
        "/files/api/list?path=%2e%2e",
        "/files/api/list?path=%2e%2e%2f",
        "/files/api/list?path=sub/../..",
        "/files/api/list?path=sub%2f..%2f..%2foutside",
        "/files/api/list?path=..%5coutside",
        "/files/api/list?path=..\\outside",
        "/files/api/list?path=dir_out",
        "/files/api/list?path=link_out",
        "/files/api/list?path=/../outside",
        "/files/api/list?path=/etc",
        "/files/api/list?path=%2fetc%2f",
        "/files/api/list?path=a.txt%00",
    };
    for (const auto& target : targets) {
        auto r = raw_request(srv.port, "GET", target);
        EXPECT_TRUE(r.status == 400 || r.status == 404);
        EXPECT_FALSE(contains(r.body, "TOPSECRET"));
        EXPECT_FALSE(contains(r.body, "root:"));
        EXPECT_FALSE(contains(r.body, "secret.txt\""));
    }
}

UTEST(httpp_file_browser, missing_hidden_and_outside_look_the_same) {
    tree t;
    local_server srv([&](httpp::server& s) { httpp::mount_file_browser(s, "/files", t.root.string()); });
    auto missing = raw_request(srv.port, "GET", "/files/api/list?path=nope");
    auto hidden = raw_request(srv.port, "GET", "/files/api/list?path=.hidden");
    auto outside = raw_request(srv.port, "GET", "/files/api/list?path=dir_out");
    auto a_file = raw_request(srv.port, "GET", "/files/api/list?path=a.txt");
    ASSERT_EQ(404, missing.status);
    EXPECT_EQ(404, hidden.status);
    EXPECT_EQ(404, outside.status);
    EXPECT_EQ(404, a_file.status);
    EXPECT_STR_EQ(missing.body, hidden.body);
    EXPECT_STR_EQ(missing.body, outside.body);
    EXPECT_STR_EQ(missing.body, a_file.body);
}

UTEST(httpp_file_browser, write_methods_do_nothing) {
    tree t;
    local_server srv([&](httpp::server& s) { httpp::mount_file_browser(s, "/files", t.root.string()); });
    for (const char* method : {"POST", "PUT", "DELETE", "PATCH"}) {
        for (const char* target : {"/files/api/list", "/files/raw/a.txt", "/files/raw/new.txt", "/files", "/files/"}) {
            auto r = raw_request(srv.port, method, target);
            EXPECT_TRUE(r.status == 404 || r.status == 405);
        }
    }
    EXPECT_TRUE(fs::exists(t.root / "a.txt"));
    EXPECT_FALSE(fs::exists(t.root / "new.txt"));
    std::ifstream in(t.root / "a.txt");
    std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_STR_EQ("hello world", body);
}

UTEST(httpp_file_browser, a_name_that_is_not_utf8_is_left_out_of_the_json) {
    tree t;
    { std::ofstream(t.root / std::string("bad\xff\xfe.txt")) << "x"; }
    local_server srv([&](httpp::server& s) { httpp::mount_file_browser(s, "/files", t.root.string()); });
    auto r = httpp::client::fetch(srv.url("/files/api/list"));
    ASSERT_EQ(200, r.status);
    EXPECT_FALSE(contains(r.body, "bad"));
    EXPECT_TRUE(contains(r.body, "\"name\":\"a.txt\"")); // the rest is still there
    for (unsigned char c : r.body) EXPECT_LT(c, 0x80);
}
#endif

UTEST(httpp_file_browser, a_unicode_name_is_listed_and_downloads_with_filename_star) {
    tree t;
    const std::string name = "caf\xc3\xa9.txt"; // café.txt
    write_file(t.root / name, "bean");
    local_server srv([&](httpp::server& s) { httpp::mount_file_browser(s, "/files", t.root.string()); });
    auto list = httpp::client::fetch(srv.url("/files/api/list"));
    EXPECT_TRUE(contains(list.body, "\"name\":\"" + name + "\""));
    auto r = httpp::client::fetch(srv.url("/files/raw/caf%C3%A9.txt?download=1"));
    ASSERT_EQ(200, r.status);
    EXPECT_STR_EQ("bean", r.body);
    EXPECT_TRUE(contains(r.header("Content-Disposition"), "attachment"));
    EXPECT_TRUE(contains(r.header("Content-Disposition"), "filename*=UTF-8''caf%C3%A9.txt"));
}

UTEST(httpp_file_browser, raw_serves_ranges_and_head_like_a_static_directory) {
    tree t;
    local_server srv([&](httpp::server& s) { httpp::mount_file_browser(s, "/files", t.root.string()); });
    auto all = httpp::client::fetch(srv.url("/files/raw/a.txt"));
    ASSERT_EQ(200, all.status);
    EXPECT_STR_EQ("hello world", all.body);
    EXPECT_TRUE(contains(all.header("Content-Type"), "text/plain"));
    EXPECT_STR_EQ("bytes", all.header("Accept-Ranges"));
    auto part = httpp::request(srv.url("/files/raw/a.txt")).header("Range", "bytes=6-10").run();
    ASSERT_EQ(206, part.status);
    EXPECT_STR_EQ("world", part.body);
    EXPECT_STR_EQ("bytes 6-10/11", part.header("Content-Range"));
    auto bad = httpp::request(srv.url("/files/raw/a.txt")).header("Range", "bytes=50-60").run();
    EXPECT_EQ(416, bad.status);
    auto head = httpp::request(srv.url("/files/raw/a.txt")).method("HEAD").run();
    EXPECT_EQ(200, head.status);
    EXPECT_TRUE(head.body.empty());
    EXPECT_STR_EQ("11", head.header("Content-Length"));
}

UTEST(httpp_file_browser, active_and_unknown_types_are_always_downloads_and_nothing_is_sniffed) {
    tree t;
    write_file(t.root / "x.svg", "<svg xmlns='http://www.w3.org/2000/svg'><script>1</script></svg>");
    write_file(t.root / "x.xml", "<a/>");
    write_file(t.root / "x.js", "alert(1)");
    write_file(t.root / "noext", "who knows");
    write_file(t.root / "pic.png", "not really a png");
    local_server srv([&](httpp::server& s) { httpp::mount_file_browser(s, "/files", t.root.string()); });
    for (const char* name : {"evil.html", "x.svg", "x.xml", "x.js", "noext"}) {
        auto r = httpp::client::fetch(srv.url(std::string("/files/raw/") + name));
        ASSERT_EQ(200, r.status);
        EXPECT_TRUE(contains(r.header("Content-Disposition"), "attachment"));
        EXPECT_STR_EQ("nosniff", r.header("X-Content-Type-Options"));
    }
    auto png = httpp::client::fetch(srv.url("/files/raw/pic.png"));
    EXPECT_STR_EQ("nosniff", png.header("X-Content-Type-Options"));
    EXPECT_FALSE(contains(png.header("Content-Disposition"), "attachment")); // a plain image may be shown
    auto txt = httpp::client::fetch(srv.url("/files/raw/a.txt"));
    EXPECT_FALSE(contains(txt.header("Content-Disposition"), "attachment"));
    auto forced = httpp::client::fetch(srv.url("/files/raw/a.txt?download=1"));
    EXPECT_TRUE(contains(forced.header("Content-Disposition"), "attachment"));
}

UTEST(httpp_file_browser, a_folder_is_not_a_file_and_index_html_is_not_served) {
    tree t;
    write_file(t.root / "sub" / "index.html", "<h1>index</h1>");
    local_server srv([&](httpp::server& s) { httpp::mount_file_browser(s, "/files", t.root.string()); });
    EXPECT_EQ(404, httpp::client::fetch(srv.url("/files/raw/sub")).status);
    EXPECT_EQ(404, httpp::client::fetch(srv.url("/files/raw/sub/")).status);
    EXPECT_EQ(404, httpp::client::fetch(srv.url("/files/raw/")).status);
    EXPECT_EQ(404, httpp::client::fetch(srv.url("/files/raw")).status);
    auto r = httpp::client::fetch(srv.url("/files/raw/sub/index.html")); // named explicitly it is just a file, and a download
    EXPECT_EQ(200, r.status);
    EXPECT_TRUE(contains(r.header("Content-Disposition"), "attachment"));
}

UTEST(httpp_file_browser, a_big_folder_is_cut_at_max_entries_and_says_so) {
    tree t;
    for (int i = 0; i < 10; i++) write_file(t.root / "many" / ("f" + std::to_string(i) + ".txt"), "x");
    httpp::file_browser_options o;
    o.max_entries = 3;
    local_server srv([&](httpp::server& s) { httpp::mount_file_browser(s, "/files", t.root.string(), o); });
    auto r = httpp::client::fetch(srv.url("/files/api/list?path=many"));
    ASSERT_EQ(200, r.status);
    EXPECT_TRUE(contains(r.body, "\"truncated\":true"));
    EXPECT_TRUE(contains(r.body, "\"limit\":3"));
    std::size_t names = 0;
    for (std::size_t at = r.body.find("\"name\""); at != std::string::npos; at = r.body.find("\"name\"", at + 1)) names++;
    EXPECT_EQ(3u, names);
}

UTEST(httpp_file_browser, the_page_is_html_with_the_options_filled_in_and_a_strict_policy) {
    tree t;
    httpp::file_browser_options o;
    o.title = "<b>My \"files\"</b>";
    o.theme = "dark";
    o.accent = "#12ab34";
    o.view = "grid";
    o.sort = "date";
    o.search = false;
    local_server srv([&](httpp::server& s) { httpp::mount_file_browser(s, "/files", t.root.string(), o); });
    for (const char* path : {"/files", "/files/"}) {
        auto r = httpp::client::fetch(srv.url(path));
        ASSERT_EQ(200, r.status);
        EXPECT_TRUE(contains(r.header("Content-Type"), "text/html"));
        EXPECT_FALSE(contains(r.body, "<b>My"));
        EXPECT_TRUE(contains(r.body, "&lt;b&gt;My"));
        EXPECT_TRUE(contains(r.body, "#12ab34"));
        EXPECT_TRUE(contains(r.body, "\"theme\":\"dark\""));
        EXPECT_TRUE(contains(r.body, "\"view\":\"grid\""));
        EXPECT_TRUE(contains(r.body, "\"sort\":\"date\""));
        EXPECT_TRUE(contains(r.body, "\"search\":false"));
        EXPECT_TRUE(contains(r.body, "\"base\":\"/files\""));
        EXPECT_FALSE(contains(r.body, "__TITLE__"));
        EXPECT_FALSE(contains(r.body, "__CONFIG__"));
        EXPECT_TRUE(contains(r.header("Content-Security-Policy"), "default-src 'none'"));
        EXPECT_STR_EQ("nosniff", r.header("X-Content-Type-Options"));
        EXPECT_FALSE(contains(r.body, t.root.string()));
    }
}

UTEST(httpp_file_browser, can_be_mounted_at_the_root_of_a_server) {
    tree t;
    local_server srv([&](httpp::server& s) { httpp::mount_file_browser(s, "/", t.root.string()); });
    EXPECT_EQ(200, httpp::client::fetch(srv.url("/")).status);
    EXPECT_TRUE(contains(httpp::client::fetch(srv.url("/api/list")).body, "\"name\":\"a.txt\""));
    EXPECT_STR_EQ("hello world", httpp::client::fetch(srv.url("/raw/a.txt")).body);
}

UTEST(httpp_file_browser, bad_arguments_are_refused_when_mounting) {
    tree t;
    const std::string root = t.root.string();
    EXPECT_FALSE(rejects("/files", root));
    EXPECT_TRUE(rejects("files", root));                       // base must start with '/'
    EXPECT_TRUE(rejects("", root));
    EXPECT_TRUE(rejects("/files", (t.top / "missing").string()));
    EXPECT_TRUE(rejects("/files", (t.root / "a.txt").string())); // a file, not a folder
    EXPECT_TRUE(rejects("/files", ""));
#ifndef _WIN32
    EXPECT_TRUE(rejects("/files", "/"));                       // the whole disk is never served
    EXPECT_TRUE(rejects("/files", "//"));
#endif
    httpp::file_browser_options o;
    o.accent = "red";
    EXPECT_TRUE(rejects("/files", root, o));
    o.accent = "#12345";
    EXPECT_TRUE(rejects("/files", root, o));
    o.accent = "#12345g";
    EXPECT_TRUE(rejects("/files", root, o));
    o.accent = "#12ab34\"><script>";
    EXPECT_TRUE(rejects("/files", root, o));
    o = {};
    o.theme = "purple";
    EXPECT_TRUE(rejects("/files", root, o));
    o = {};
    o.view = "tiles";
    EXPECT_TRUE(rejects("/files", root, o));
    o = {};
    o.sort = "color";
    EXPECT_TRUE(rejects("/files", root, o));
    o = {};
    o.max_entries = 0;
    EXPECT_TRUE(rejects("/files", root, o));
    o = {};
    o.accent = "#ABCDEF"; // upper case hex is fine
    EXPECT_FALSE(rejects("/files", root, o));
}

UTEST(httpp_server_directory_options, the_old_serve_directory_still_serves_dotfiles_and_index_html) {
    tree t;
    write_file(t.root / "index.html", "<h1>home</h1>");
    local_server srv([&](httpp::server& s) { s.serve_directory("/site", t.root.string()); });
    auto home = httpp::client::fetch(srv.url("/site/"));
    EXPECT_EQ(200, home.status);
    EXPECT_STR_EQ("<h1>home</h1>", home.body);
    EXPECT_FALSE(contains(home.header("Content-Disposition"), "attachment"));
    EXPECT_EQ(200, httpp::client::fetch(srv.url("/site/.dotfile")).status);
}

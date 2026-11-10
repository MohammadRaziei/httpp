// httpp::path (ADR-0055): the pathlib-shaped path the public API takes.
#include "utest/utest.h"
#include "httpp/path.hpp"
#include "httpp/sha256.hpp"
#include "local_server.hpp"

#include <iterator>
#include <string>

using httpp::path;

namespace {
std::string s(const path& p) { return p.str(); }
}

UTEST(path, construction_cleans_like_pathlib) {
    ASSERT_TRUE(s("a//b///c/") == "a/b/c");
    ASSERT_TRUE(s("/") == "/");
    ASSERT_TRUE(s("//") == "/");
    ASSERT_TRUE(s("") == "");
    ASSERT_TRUE(path().empty());
}

UTEST(path, parts_follow_pathlib) {
    const path p = "dir/archive.tar.gz";
    ASSERT_TRUE(s(p.name()) == "archive.tar.gz");
    ASSERT_TRUE(s(p.suffix()) == ".gz");
    ASSERT_TRUE(s(p.stem()) == "archive.tar");
    ASSERT_TRUE(s(p.parent()) == "dir");
    ASSERT_TRUE(s(path(".bashrc").suffix()) == "");
    ASSERT_TRUE(s(path(".bashrc").stem()) == ".bashrc");
    ASSERT_TRUE(s(path("noext").suffix()) == "");
    ASSERT_TRUE(s(path("a.").suffix()) == "");
}

UTEST(path, parent_edges) {
    ASSERT_TRUE(s(path("a").parent()) == ".");
    ASSERT_TRUE(s(path("/a").parent()) == "/");
    ASSERT_TRUE(s(path("/").parent()) == "/");
    ASSERT_TRUE(s(path("a/b").parent()) == "a");
    ASSERT_TRUE(path().parent().empty());
}

UTEST(path, join_and_with) {
    ASSERT_TRUE(s(path("a") / "b") == "a/b");
    ASSERT_TRUE(s(path("a/") / "b") == "a/b");
    ASSERT_TRUE(s(path("a") / "/abs") == "/abs"); // an absolute right side wins, like pathlib
    ASSERT_TRUE(s(path() / "b") == "b");
    ASSERT_TRUE(s(path("a") / "") == "a");
    ASSERT_TRUE(s(path("d/f.txt").with_suffix(".bin")) == "d/f.bin");
    ASSERT_TRUE(s(path("d/f.txt").with_suffix("")) == "d/f");
    ASSERT_TRUE(s(path("d/f.txt").with_name("g.md")) == "d/g.md");
    ASSERT_TRUE(s(path("f.txt").with_name("g.md")) == "g.md");
    path q = "x";
    q /= "y";
    ASSERT_TRUE(q == path("x/y"));
}

UTEST(path, cwd_absolute_uri_and_the_source_file_idiom) {
    const path here = path::cwd();
    ASSERT_TRUE(here.is_absolute());
    ASSERT_TRUE(here.is_dir());
    ASSERT_TRUE((path::cwd() / "some" / "file").str() == here.str() + (here.str() == "/" ? "" : "/") + "some/file");
    ASSERT_TRUE(path("x").absolute() == here / "x");
    ASSERT_TRUE(path("/a b/c#d.txt").as_uri() == "file:///a%20b/c%23d.txt");
    ASSERT_TRUE(path("/a/b").as_posix() == "/a/b");
    // the usual "next to this source file" line
    const path data = (path(__FILE__).parent() / "some" / "dir");
    ASSERT_TRUE(data.name() == path("dir"));
    ASSERT_TRUE(data.parent().name() == path("some"));
    ASSERT_TRUE(data.as_uri().compare(0, 8, "file:///") == 0 || data.as_uri().compare(0, 7, "file://") == 0);
}

UTEST(path, absolute) {
    ASSERT_TRUE(path("/a").is_absolute());
    ASSERT_TRUE(path("C:/a").is_absolute());
    ASSERT_FALSE(path("a/b").is_absolute());
}

UTEST(path, disk_members_and_open) {
    const path dir = "path_test_dir";
    if (dir.exists()) (dir / "f.txt").unlink();
    else ASSERT_TRUE(dir.mkdir());
    ASSERT_TRUE(dir.is_dir());
    ASSERT_FALSE(dir.is_file());
    const path f = dir / "f.txt";
    ASSERT_FALSE(f.exists());
    ASSERT_TRUE(f.size() == -1);
    f.write_text("hello");
    ASSERT_TRUE(f.is_file());
    ASSERT_TRUE(f.size() == 5);
    ASSERT_TRUE(f.read_text() == "hello");
    {
        auto in = f.open(); // the default: read, binary
        ASSERT_TRUE(static_cast<bool>(in));
        std::string got;
        in >> got;
        ASSERT_TRUE(got == "hello");
    }
    {
        auto out = f.open(std::ios::out | std::ios::app | std::ios::binary);
        out << "!";
    }
    ASSERT_TRUE(f.read_text() == "hello!");
    ASSERT_TRUE(httpp::sha256_file(f) == httpp::sha256_file("path_test_dir/f.txt")); // a string converts implicitly
    const path g = dir / "g.txt";
    ASSERT_TRUE(f.rename(g));
    ASSERT_FALSE(f.exists());
    ASSERT_TRUE(g.exists());
    ASSERT_TRUE(g.unlink());
    ASSERT_FALSE(path("path_test_dir/missing").open());
}

// The case that motivated open(): serve a file from an address, with the path as the response's file.
UTEST(path, a_path_is_what_a_route_serves_as_a_file) {
    const path dir = "path_serve_dir";
    dir.mkdir();
    const path f = dir / "page.txt";
    f.write_text("served from a path");
    testutil::local_server srv([&](httpp::server& s) {
        s.get("/page", [&](const httpp::request&, httpp::response& res) { res.file = f; });
        s.get("/copy", [&](const httpp::request&, httpp::response& res) {
            auto in = f.open();
            res.body.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        });
    });
    ASSERT_TRUE(httpp::client::fetch(srv.url("/page")).body == "served from a path");
    ASSERT_TRUE(httpp::client::fetch(srv.url("/copy")).body == "served from a path");
    f.unlink();
}

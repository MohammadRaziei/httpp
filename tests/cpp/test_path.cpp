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
        s.get("/page", [&](const httpp::server_request&, httpp::response& res) { res.file = f; });
        s.get("/copy", [&](const httpp::server_request&, httpp::response& res) {
            auto in = f.open();
            res.body.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        });
    });
    ASSERT_TRUE(httpp::client::fetch(srv.url("/page")).body == "served from a path");
    ASSERT_TRUE(httpp::client::fetch(srv.url("/copy")).body == "served from a path");
    f.unlink();
}

// ---- the file system half (formerly the internal httpp::fs, ADR-0054; merged into path by ADR-0058) ----

#include <algorithm>
#include <vector>
#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

const path area = "fs_test_area";

// a fresh empty directory
void fresh() {
    std::vector<path> entries;
    if (area.iterdir(entries))
        for (const auto& e : entries) { e.unlink(); e.rmdir(); }
    area.mkdir();
}

} // namespace

UTEST(path, is_relative_to_is_a_path_test_not_a_prefix_test) {
    ASSERT_TRUE(path("/data").is_relative_to("/data"));
    ASSERT_TRUE(path("/data/x/y").is_relative_to("/data"));
    ASSERT_TRUE(path("/data/x").is_relative_to("/data/"));
    ASSERT_FALSE(path("/data2").is_relative_to("/data"));
    ASSERT_FALSE(path("/dat").is_relative_to("/data"));
    ASSERT_FALSE(path("/").is_relative_to("/data"));
    ASSERT_TRUE(path("/anything").is_relative_to("/"));
    ASSERT_TRUE(path("C:/x").is_relative_to("C:/"));
    ASSERT_FALSE(path("D:/x").is_relative_to("C:/"));
}

UTEST(path, stat_tells_files_directories_and_nothing) {
    fresh();
    (area / "a.txt").write_text("hello");
    const path::status f = (area / "a.txt").stat();
    ASSERT_TRUE(f.type == path::kind::file);
    ASSERT_EQ(5, f.size);
    ASSERT_TRUE(f.mtime > 1500000000);
    ASSERT_TRUE(area.stat().type == path::kind::directory);
    ASSERT_TRUE((area / "nope").stat().type == path::kind::missing);
}

UTEST(path, mkdir_is_true_only_for_the_call_that_created_it) {
    fresh();
    const path d = area / "lock";
    ASSERT_TRUE(d.mkdir());
    ASSERT_FALSE(d.mkdir());      // there already
    ASSERT_TRUE(d.is_dir());
    ASSERT_FALSE((area / "no/such/parent").mkdir()); // cannot be made
    ASSERT_FALSE((area / "no/such/parent").is_dir());
    ASSERT_TRUE(d.rmdir());
    ASSERT_FALSE(d.exists());
}

UTEST(path, iterdir_leaves_out_dot_and_dotdot) {
    fresh();
    (area / "one").write_text("1");
    (area / "two").mkdir();
    std::vector<path> entries;
    ASSERT_TRUE(area.iterdir(entries));
    std::sort(entries.begin(), entries.end());
    ASSERT_EQ(2u, entries.size());
    ASSERT_TRUE(entries[0] == area / "one" && entries[1] == area / "two");
    ASSERT_FALSE((area / "nope").iterdir(entries));
}

UTEST(path, rename_moves_over_an_existing_file) {
    fresh();
    const path from = area / "from", to = area / "to";
    from.write_text("new");
    to.write_text("old contents, longer");
    ASSERT_TRUE(from.rename(to));
    ASSERT_FALSE(from.exists());
    ASSERT_EQ(3, to.size());
    ASSERT_TRUE(to.unlink());
    ASSERT_FALSE(to.unlink());
}

UTEST(path, a_utf8_name_works) {
    fresh();
    const path p = area / "\xD9\x84\xD9\x88\xD8\xAC.txt"; // a Persian word
    p.write_text("x");
    ASSERT_TRUE(p.is_file());
    std::vector<path> entries;
    ASSERT_TRUE(area.iterdir(entries));
    ASSERT_EQ(1u, entries.size());
    ASSERT_TRUE(entries[0].name().str() == "\xD9\x84\xD9\x88\xD8\xAC.txt");
}

UTEST(path, resolve_resolves_dots_and_is_empty_for_a_missing_path) {
    fresh();
    (area / "sub").mkdir();
    const path a = area.resolve();
    const path b = path(area.str() + "/sub/..").resolve();
    ASSERT_FALSE(a.empty());
    ASSERT_TRUE(a == b);
    ASSERT_TRUE((area / "missing").resolve().empty());
    ASSERT_TRUE((area / "sub").resolve().is_relative_to(a));
    ASSERT_FALSE(a.is_root());
}

#ifndef _WIN32
UTEST(path, a_link_is_seen_by_lstat_and_followed_by_stat) {
    fresh();
    (area / "target").write_text("abc");
    ASSERT_EQ(0, ::symlink("target", (area / "link").c_str()));
    ASSERT_TRUE((area / "link").lstat().link);
    const path::status s = (area / "link").stat();
    ASSERT_TRUE(s.type == path::kind::file);
    ASSERT_EQ(3, s.size);
    ASSERT_TRUE((area / "link").resolve() == (area / "target").resolve());
    ASSERT_TRUE(path("/").resolve().is_root());
}

UTEST(path, is_executable_needs_the_permission) {
    fresh();
    const path p = area / "tool";
    p.write_text("#!/bin/sh\n");
    ASSERT_EQ(0, ::chmod(p.c_str(), 0644));
    ASSERT_FALSE(p.is_executable());
    ASSERT_EQ(0, ::chmod(p.c_str(), 0755));
    ASSERT_TRUE(p.is_executable());
    ASSERT_FALSE(area.is_executable()); // a directory is not a program
}
#endif

UTEST(path, zz_cleanup) {
    fresh();
    area.rmdir();
    ASSERT_FALSE(area.exists());
}

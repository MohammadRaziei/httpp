// httpp::path (ADR-0054): the pathlib-shaped path the public API takes.
#include "utest/utest.h"
#include "httpp/env.hpp"
#include "httpp/path.hpp"
#include "httpp/sha256.hpp"
#include "local_server.hpp"

#include <atomic>
#include <chrono>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using httpp::path;
using httpp::temp_path;

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
    ASSERT_TRUE(HTTPP_PATH_CWD == path(__FILE__).parent());
    const path data = (HTTPP_PATH_CWD / "some" / "dir");
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

// ---- the file system half (formerly the internal httpp::fs, ADR-0054; merged into path by ADR-0054) ----

#include <algorithm>
#include <vector>
#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

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
    temp_path area("httpp_path_test_"); // removed, with everything in it, when the test ends
    (area / "a.txt").write_text("hello");
    const path::status f = (area / "a.txt").stat();
    ASSERT_TRUE(f.type == path::kind::file);
    ASSERT_EQ(5, f.size);
    ASSERT_TRUE(f.mtime > 1500000000);
    ASSERT_TRUE(area->stat().type == path::kind::directory);
    ASSERT_TRUE((area / "nope").stat().type == path::kind::missing);
}

UTEST(path, mkdir_is_true_only_for_the_call_that_created_it) {
    temp_path area("httpp_path_test_"); // removed, with everything in it, when the test ends
    const path d = area / "lock";
    ASSERT_TRUE(d.mkdir());
    ASSERT_FALSE(d.mkdir());      // there already
    ASSERT_TRUE(d.is_dir());
    ASSERT_FALSE((area / "no/such/parent").mkdir()); // cannot be made
    ASSERT_FALSE((area / "no/such/parent").is_dir());
    ASSERT_TRUE(d.rmdir());
    ASSERT_FALSE(d.exists());
}

UTEST(path, mkdirs_makes_every_missing_level_and_is_true_when_the_folder_is_there) {
    temp_path area("httpp_path_test_"); // removed, with everything in it, when the test ends
    const path deep = area / "a/b/c";
    ASSERT_TRUE(deep.mkdirs());
    ASSERT_TRUE((area / "a/b").is_dir());
    ASSERT_TRUE(deep.mkdirs()); // there already
    (area / "file").write_text("x");
    ASSERT_FALSE((area / "file/sub").mkdirs()); // a file is in the way
    ASSERT_FALSE(path().mkdirs());
    ASSERT_TRUE(path(".").mkdirs());
}

UTEST(path, iterdir_leaves_out_dot_and_dotdot) {
    temp_path area("httpp_path_test_"); // removed, with everything in it, when the test ends
    (area / "one").write_text("1");
    (area / "two").mkdir();
    std::vector<path> entries;
    ASSERT_TRUE(area->iterdir(entries));
    std::sort(entries.begin(), entries.end());
    ASSERT_EQ(2u, entries.size());
    ASSERT_TRUE(entries[0] == area / "one" && entries[1] == area / "two");
    ASSERT_FALSE((area / "nope").iterdir(entries));
}

UTEST(path, rename_moves_over_an_existing_file) {
    temp_path area("httpp_path_test_"); // removed, with everything in it, when the test ends
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
    temp_path area("httpp_path_test_"); // removed, with everything in it, when the test ends
    const path p = area / "\xD9\x84\xD9\x88\xD8\xAC.txt"; // a Persian word
    p.write_text("x");
    ASSERT_TRUE(p.is_file());
    std::vector<path> entries;
    ASSERT_TRUE(area->iterdir(entries));
    ASSERT_EQ(1u, entries.size());
    ASSERT_TRUE(entries[0].name().str() == "\xD9\x84\xD9\x88\xD8\xAC.txt");
}

UTEST(path, resolve_resolves_dots_and_is_empty_for_a_missing_path) {
    temp_path area("httpp_path_test_"); // removed, with everything in it, when the test ends
    (area / "sub").mkdir();
    const path a = area->resolve();
    const path b = path(area->str() + "/sub/..").resolve();
    ASSERT_FALSE(a.empty());
    ASSERT_TRUE(a == b);
    ASSERT_TRUE((area / "missing").resolve().empty());
    ASSERT_TRUE((area / "sub").resolve().is_relative_to(a));
    ASSERT_FALSE(a.is_root());
}

#ifndef _WIN32
UTEST(path, a_link_is_seen_by_lstat_and_followed_by_stat) {
    temp_path area("httpp_path_test_"); // removed, with everything in it, when the test ends
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
    temp_path area("httpp_path_test_"); // removed, with everything in it, when the test ends
    const path p = area / "tool";
    p.write_text("#!/bin/sh\n");
    ASSERT_EQ(0, ::chmod(p.c_str(), 0644));
    ASSERT_FALSE(p.is_executable());
    ASSERT_EQ(0, ::chmod(p.c_str(), 0755));
    ASSERT_TRUE(p.is_executable());
    ASSERT_FALSE(area->is_executable()); // a directory is not a program
}
#endif

UTEST(path, rmtree_removes_a_file_a_tree_and_what_is_not_there) {
    temp_path area("httpp_path_test_"); // removed, with everything in it, when the test ends
    ASSERT_TRUE((area / "nothing").rmtree()); // never there is fine
    (area / "one").write_text("x");
    ASSERT_TRUE((area / "one").rmtree());
    ASSERT_FALSE((area / "one").exists());

    ASSERT_TRUE((area / "a" / "b" / "c").mkdirs());
    (area / "a" / "top.txt").write_text("1");
    (area / "a" / "b" / "mid.txt").write_text("2");
    (area / "a" / "b" / "c" / "deep.txt").write_text("3");
    ASSERT_TRUE((area / "a").rmtree());
    ASSERT_FALSE((area / "a").exists());
    ASSERT_TRUE(area->is_dir()); // the parent stays
}

#ifndef _WIN32
UTEST(path, rmtree_removes_a_link_and_never_follows_it) {
    temp_path area("httpp_path_test_"); // removed, with everything in it, when the test ends
    ASSERT_TRUE((area / "outside").mkdirs());
    (area / "outside" / "keep.txt").write_text("keep");
    ASSERT_TRUE((area / "tree").mkdirs());
    ASSERT_EQ(0, ::symlink("../outside", (area / "tree" / "to_dir").c_str()));
    ASSERT_EQ(0, ::symlink("missing", (area / "tree" / "dangling").c_str()));

    ASSERT_TRUE((area / "tree").rmtree());
    ASSERT_FALSE((area / "tree").exists());
    ASSERT_TRUE((area / "outside" / "keep.txt").is_file()); // what the link pointed to is untouched

    ASSERT_EQ(0, ::symlink("outside", (area / "link").c_str()));
    ASSERT_TRUE((area / "link").rmtree()); // the link itself, not the directory behind it
    ASSERT_FALSE((area / "link").lstat().link);
    ASSERT_TRUE((area / "outside" / "keep.txt").is_file());
}
#endif

// ---- temp_path ----

#ifndef _WIN32
UTEST(path, temp_dir_reads_tmpdir_then_tmp_then_temp_and_skips_what_is_empty_or_not_a_folder) {
    namespace env = httpp::env;
    temp_path first("httpp_td_first_");
    temp_path second("httpp_td_second_");
    env::scope tmpdir("TMPDIR"); // each starts unset, and the old values come back at the end
    env::scope tmp("TMP");
    env::scope temp("TEMP");

    ASSERT_TRUE(path::temp_dir() == path("/tmp")); // nothing set: the fallback

    env::set("TEMP", first->str());
    ASSERT_TRUE(path::temp_dir() == first.get());
    env::set("TMP", second->str()); // TMP comes before TEMP
    ASSERT_TRUE(path::temp_dir() == second.get());
    env::set("TMPDIR", first->str()); // and TMPDIR before both
    ASSERT_TRUE(path::temp_dir() == first.get());

    env::set("TMPDIR", ""); // empty: skipped
    ASSERT_TRUE(path::temp_dir() == second.get());
    env::set("TMPDIR", (first / "missing").str()); // not a folder: skipped
    ASSERT_TRUE(path::temp_dir() == second.get());
    env::set("TMP", (first / "missing").str());
    env::set("TEMP", (first / "missing").str());
    ASSERT_TRUE(path::temp_dir() == path("/tmp"));
}
#endif

UTEST(temp_path, makes_a_folder_in_the_temp_directory_and_removes_it_with_the_object) {
    path where;
    {
        temp_path t;
        where = t.get();
        ASSERT_TRUE(where.is_dir());
        ASSERT_TRUE(where.parent() == path::temp_dir());
        ASSERT_TRUE((t / "a" / "b").mkdirs());
        (t / "a" / "b" / "f.txt").write_text("x");
        ASSERT_FALSE(t.gone());
    }
    ASSERT_FALSE(where.exists()); // the whole tree went with it
}

UTEST(temp_path, the_name_is_the_prefix_and_something_that_does_not_repeat) {
    temp_path a("httpp_tp_");
    temp_path b("httpp_tp_");
    ASSERT_TRUE(a->name().str().rfind("httpp_tp_", 0) == 0);
    ASSERT_TRUE(b->name().str().rfind("httpp_tp_", 0) == 0);
    ASSERT_TRUE(a.get() != b.get());
    temp_path d; // the default prefix
    ASSERT_TRUE(d->name().str().rfind("httpp_", 0) == 0);
}

UTEST(temp_path, can_be_made_inside_another_folder) {
    temp_path outer("httpp_tp_outer_");
    temp_path inner(outer, "inner_");
    ASSERT_TRUE(inner->parent() == outer.get());
    ASSERT_TRUE(inner->is_dir());
    path where = inner.get();
    ASSERT_TRUE(inner.done());
    ASSERT_FALSE(where.exists());
    ASSERT_TRUE(outer->is_dir()); // only its own folder goes
}

UTEST(temp_path, a_parent_that_is_not_there_throws_and_makes_nothing) {
    temp_path outer("httpp_tp_outer_");
    bool threw = false;
    try {
        temp_path t(outer / "missing", "x_");
    } catch (const std::runtime_error&) {
        threw = true;
    }
    ASSERT_TRUE(threw);
    ASSERT_FALSE((outer / "missing").exists());
}

UTEST(temp_path, done_removes_now_and_says_whether_it_worked) {
    temp_path t;
    (t / "f.txt").write_text("x");
    ASSERT_FALSE(t.gone());
    ASSERT_TRUE(t.done());
    ASSERT_TRUE(t.gone());
    ASSERT_TRUE(t.done()); // again: still gone, nothing to do
}

UTEST(temp_path, release_keeps_the_folder_and_nothing_removes_it_any_more) {
    path kept;
    {
        temp_path t;
        kept = t.release();
        ASSERT_TRUE(kept == t.get());
        ASSERT_FALSE(t.done()); // not ours to remove now, so it is still there
    }
    ASSERT_TRUE(kept.is_dir());
    ASSERT_TRUE(kept.rmtree()); // the test cleans up what it released
}

UTEST(temp_path, a_move_hands_the_folder_over_and_the_old_one_goes_when_assigned_over) {
    path first;
    path second;
    {
        temp_path a;
        first = a.get();
        temp_path b(std::move(a)); // b owns it now, a owns nothing
        ASSERT_TRUE(b.get() == first);
        ASSERT_TRUE(first.is_dir());
        temp_path c;
        second = c.get();
        c = std::move(b); // c's own folder is removed, and it takes b's
        ASSERT_FALSE(second.exists());
        ASSERT_TRUE(c.get() == first);
        ASSERT_TRUE(first.is_dir());
    }
    ASSERT_FALSE(first.exists());
}

UTEST(temp_path, it_is_a_path_where_a_path_is_taken) {
    temp_path t;
    const auto make = [](const path& p) { return (p / "made").mkdir(); };
    ASSERT_TRUE(make(t)); // converts to const path&
    ASSERT_TRUE((t / "made").is_dir());
    ASSERT_TRUE(t->is_dir());
    ASSERT_TRUE(path::temp_dir().is_dir());
}

// ---- write_text_atomic and read_text ranges ----

namespace {
path scratch_dir(const char* name) {
    path d = path(std::string("test_tmp_path_") + name);
    for (const path& e : [&] { std::vector<path> v; d.iterdir(v); return v; }()) e.unlink();
    d.mkdir();
    return d;
}
int leftovers(const path& d, const std::string& keep) {
    std::vector<path> v;
    d.iterdir(v);
    int n = 0;
    for (const path& e : v) n += e.name() != keep;
    return n;
}
}  // namespace

UTEST(path, write_text_atomic_creates_and_replaces_a_file_and_leaves_nothing_behind) {
    const path d = scratch_dir("atomic");
    const path f = d / "state.txt";
    f.write_text_atomic("first");
    ASSERT_TRUE(f.read_text() == "first");
    f.write_text_atomic("second, a longer text");
    ASSERT_TRUE(f.read_text() == "second, a longer text");
    f.write_text_atomic("");
    ASSERT_TRUE(f.exists() && f.read_text().empty());
    ASSERT_EQ(0, leftovers(d, "state.txt"));  // no ".tmp.*" files
    f.unlink();
    d.rmdir();
}

UTEST(path, write_text_atomic_throws_and_leaves_no_file_when_the_directory_is_missing) {
    const path f = path("test_tmp_path_no_such_dir") / "x" / "state.txt";
    bool threw = false;
    try {
        f.write_text_atomic("data");
    } catch (const std::runtime_error&) {
        threw = true;
    }
    ASSERT_TRUE(threw);
    ASSERT_FALSE(f.exists());
}

UTEST(path, write_text_atomic_keeps_the_old_file_when_the_target_cannot_be_replaced) {
    const path d = scratch_dir("blocked");
    const path target = d / "dir_in_the_way";
    target.mkdir();  // rename() of a file over a directory fails
    bool threw = false;
    try {
        target.write_text_atomic("data");
    } catch (const std::runtime_error&) {
        threw = true;
    }
    ASSERT_TRUE(threw);
    ASSERT_TRUE(target.is_dir());
    ASSERT_EQ(0, leftovers(d, "dir_in_the_way"));  // the temporary file was removed
    target.rmdir();
    d.rmdir();
}

// A reader never sees a half-written file, however often a writer replaces it (write_text() would show an empty or cut one).
UTEST(path, a_reader_sees_only_whole_texts_while_writers_replace_the_file) {
    const path d = scratch_dir("race");
    const path f = d / "shared.txt";
    const std::string a(200000, 'a'), b(300000, 'b');
    f.write_text_atomic(a);
    std::atomic<bool> stop{false};
    std::atomic<int> torn{0}, reads{0};
    std::thread w1([&] { while (!stop) f.write_text_atomic(a); });
    std::thread w2([&] { while (!stop) f.write_text_atomic(b); });
    std::thread r([&] {
        while (!stop) {
            const std::string got = f.read_text();
            ++reads;
            if (got != a && got != b) ++torn;
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    stop = true;
    w1.join();
    w2.join();
    r.join();
    ASSERT_TRUE(reads > 0);
    ASSERT_EQ(0, torn.load());
    ASSERT_EQ(0, leftovers(d, "shared.txt"));
    f.unlink();
    d.rmdir();
}

UTEST(path, read_text_takes_a_byte_range) {
    const path d = scratch_dir("range");
    const path f = d / "line.txt";
    f.write_text("abcdefghij");
    ASSERT_TRUE(f.read_text(0, 4) == "abcd");
    ASSERT_TRUE(f.read_text(3, 6) == "def");
    ASSERT_TRUE(f.read_text(7, 100) == "hij");  // cut at the end of the file
    ASSERT_TRUE(f.read_text(0, 100) == "abcdefghij");
    ASSERT_TRUE(f.read_text(5) == "fghij");      // from start to the end
    ASSERT_TRUE(f.read_text(10, 20).empty());    // start at the end
    ASSERT_TRUE(f.read_text(50, 60).empty());    // start past it
    ASSERT_TRUE(f.read_text(4, 4).empty());
    ASSERT_TRUE(f.read_text(6, 2).empty());      // end before start
    ASSERT_TRUE(f.read_text() == "abcdefghij");
    bool threw = false;
    try {
        (d / "missing").read_text(0, 10);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    ASSERT_TRUE(threw);
    // a range larger than the read buffer
    f.write_text(std::string(20000, 'x') + "END");
    ASSERT_TRUE(f.read_text(19998, 30000) == "xxEND");
    ASSERT_TRUE(f.read_text(0, 17000).size() == 17000u);
    f.unlink();
    d.rmdir();
}

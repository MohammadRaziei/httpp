// The internal file system module (ADR-0054): what httpp uses instead of std::filesystem.
#include "utest/utest.h"
#include "internal/fs.hpp"

#include <algorithm>
#include <fstream>
#include <string>
#include <vector>
#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = httpp::fs;

namespace {

const std::string base = "fs_test_area";

void write_file(const std::string& path, const std::string& text) {
    std::ofstream out(fs::native(path), std::ios::binary | std::ios::trunc);
    out << text;
}

// a fresh empty directory
void fresh() {
    std::vector<std::string> names;
    if (fs::list(base, names)) {
        for (const auto& n : names) {
            fs::remove_file(fs::join(base, n));
            fs::remove_dir(fs::join(base, n));
        }
    }
    fs::make_dir(base);
}

} // namespace

UTEST(fs, text_helpers) {
    ASSERT_TRUE(fs::join("a", "b") == "a/b");
    ASSERT_TRUE(fs::join("a/", "b") == "a/b");
    ASSERT_TRUE(fs::join("a", "/b") == "a/b");
    ASSERT_TRUE(fs::join("a/", "/b") == "a/b");
    ASSERT_TRUE(fs::join("", "b") == "b");
    ASSERT_TRUE(fs::filename("/x/y/z.txt") == "z.txt");
    ASSERT_TRUE(fs::filename("z.txt") == "z.txt");
    ASSERT_TRUE(fs::filename("C:\\x\\z.txt") == "z.txt");
}

UTEST(fs, inside_is_a_path_test_not_a_prefix_test) {
    ASSERT_TRUE(fs::is_inside("/data", "/data"));
    ASSERT_TRUE(fs::is_inside("/data/x/y", "/data"));
    ASSERT_TRUE(fs::is_inside("/data/x", "/data/"));
    ASSERT_FALSE(fs::is_inside("/data2", "/data"));
    ASSERT_FALSE(fs::is_inside("/dat", "/data"));
    ASSERT_FALSE(fs::is_inside("/", "/data"));
    ASSERT_TRUE(fs::is_inside("/anything", "/"));
    ASSERT_TRUE(fs::is_inside("C:/x", "C:/"));
    ASSERT_FALSE(fs::is_inside("D:/x", "C:/"));
}

UTEST(fs, stat_tells_files_directories_and_nothing) {
    fresh();
    write_file(fs::join(base, "a.txt"), "hello");
    const fs::info f = fs::stat(fs::join(base, "a.txt"));
    ASSERT_TRUE(f.type == fs::kind::file);
    ASSERT_EQ(5, f.size);
    ASSERT_TRUE(f.mtime > 1500000000);
    ASSERT_TRUE(fs::stat(base).type == fs::kind::directory);
    ASSERT_TRUE(fs::stat(fs::join(base, "nope")).type == fs::kind::missing);
}

UTEST(fs, make_dir_tells_created_from_existing_from_failed) {
    fresh();
    const std::string d = fs::join(base, "lock");
    ASSERT_TRUE(fs::make_dir(d) == fs::made::created);
    ASSERT_TRUE(fs::make_dir(d) == fs::made::existed);
    ASSERT_TRUE(fs::make_dir(fs::join(base, "no/such/parent")) == fs::made::failed);
    ASSERT_TRUE(fs::remove_dir(d));
    ASSERT_TRUE(fs::stat(d).type == fs::kind::missing);
}

UTEST(fs, list_leaves_out_dot_and_dotdot) {
    fresh();
    write_file(fs::join(base, "one"), "1");
    fs::make_dir(fs::join(base, "two"));
    std::vector<std::string> names;
    ASSERT_TRUE(fs::list(base, names));
    std::sort(names.begin(), names.end());
    ASSERT_EQ(2u, names.size());
    ASSERT_TRUE(names[0] == "one" && names[1] == "two");
    ASSERT_FALSE(fs::list(fs::join(base, "nope"), names));
}

UTEST(fs, replace_moves_over_an_existing_file) {
    fresh();
    const std::string from = fs::join(base, "from"), to = fs::join(base, "to");
    write_file(from, "new");
    write_file(to, "old contents, longer");
    ASSERT_TRUE(fs::replace(from, to));
    ASSERT_TRUE(fs::stat(from).type == fs::kind::missing);
    ASSERT_EQ(3, fs::stat(to).size);
    ASSERT_TRUE(fs::remove_file(to));
    ASSERT_FALSE(fs::remove_file(to));
}

UTEST(fs, a_utf8_name_works) {
    fresh();
    const std::string path = fs::join(base, "\xD9\x84\xD9\x88\xD8\xAC.txt"); // a Persian word
    write_file(path, "x");
    ASSERT_TRUE(fs::stat(path).type == fs::kind::file);
    std::vector<std::string> names;
    ASSERT_TRUE(fs::list(base, names));
    ASSERT_EQ(1u, names.size());
    ASSERT_TRUE(names[0] == "\xD9\x84\xD9\x88\xD8\xAC.txt");
}

UTEST(fs, real_resolves_dots_and_is_empty_for_a_missing_path) {
    fresh();
    fs::make_dir(fs::join(base, "sub"));
    const std::string a = fs::real(base);
    const std::string b = fs::real(base + "/sub/..");
    ASSERT_FALSE(a.empty());
    ASSERT_TRUE(a == b);
    ASSERT_TRUE(fs::real(base + "/missing").empty());
    ASSERT_TRUE(fs::is_inside(fs::real(base + "/sub"), a));
    ASSERT_FALSE(fs::is_root(a));
}

#ifndef _WIN32
UTEST(fs, a_link_is_seen_by_lstat_and_followed_by_stat) {
    fresh();
    write_file(fs::join(base, "target"), "abc");
    ASSERT_EQ(0, ::symlink("target", fs::join(base, "link").c_str()));
    const fs::info l = fs::lstat(fs::join(base, "link"));
    ASSERT_TRUE(l.link);
    const fs::info s = fs::stat(fs::join(base, "link"));
    ASSERT_TRUE(s.type == fs::kind::file);
    ASSERT_EQ(3, s.size);
    ASSERT_TRUE(fs::real(fs::join(base, "link")) == fs::real(fs::join(base, "target")));
    ASSERT_TRUE(fs::is_root(fs::real("/")));
}

UTEST(fs, is_executable_needs_the_permission) {
    fresh();
    const std::string p = fs::join(base, "tool");
    write_file(p, "#!/bin/sh\n");
    ASSERT_EQ(0, ::chmod(p.c_str(), 0644));
    ASSERT_FALSE(fs::is_executable(p));
    ASSERT_EQ(0, ::chmod(p.c_str(), 0755));
    ASSERT_TRUE(fs::is_executable(p));
    ASSERT_FALSE(fs::is_executable(base)); // a directory is not a program
}
#endif

UTEST(fs, cleanup) {
    fresh();
    fs::remove_dir(base);
    ASSERT_TRUE(fs::stat(base).type == fs::kind::missing);
}

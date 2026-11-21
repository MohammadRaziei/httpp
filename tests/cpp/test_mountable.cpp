// httpp::mountable (ADR-0007): directory, file_browser and swagger put on a server through one base class.
#include "utest/utest.h"
#include "httpp/client.hpp"
#include "httpp/file_browser.hpp"
#include "httpp/server.hpp"
#include "httpp/swagger.hpp"
#include "local_server.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

using testutil::local_server;
namespace fs = std::filesystem;

// The three are mountable, and the base is only a base.
static_assert(std::is_base_of_v<httpp::mountable, httpp::directory>, "directory is mountable");
static_assert(std::is_base_of_v<httpp::mountable, httpp::file_browser>, "file_browser is mountable");
static_assert(std::is_base_of_v<httpp::mountable, httpp::swagger>, "swagger is mountable");
static_assert(std::is_abstract_v<httpp::mountable>, "mountable cannot be made on its own");
static_assert(std::has_virtual_destructor_v<httpp::mountable>, "a part can be deleted through the base");
static_assert(!std::is_abstract_v<httpp::directory> && !std::is_abstract_v<httpp::file_browser> && !std::is_abstract_v<httpp::swagger>,
              "the three stay concrete values");
static_assert(std::is_copy_constructible_v<httpp::swagger> && std::is_copy_constructible_v<httpp::directory> &&
                  std::is_copy_constructible_v<httpp::file_browser>,
              "the three stay copyable");

namespace {

// Written against the base: it knows nothing of which part it gets.
void put_on(const httpp::mountable& part, httpp::server s, const std::string& base) { part.mount(s, base); }

bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

struct folder {
    fs::path root;
    folder() {
        static int counter = 0;
        root = fs::temp_directory_path() /
               ("httpp_mountable_" + std::to_string(static_cast<long>(std::chrono::steady_clock::now().time_since_epoch().count() % 10000000)) +
                "_" + std::to_string(++counter));
        fs::remove_all(root);
        fs::create_directories(root);
        std::ofstream(root / "a.txt", std::ios::binary) << "hello from the folder";
    }
    ~folder() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};

} // namespace

UTEST(httpp_mountable, one_function_mounts_each_of_the_three_through_the_base) {
    folder dir;
    local_server srv([&](httpp::server& s) {
        put_on(httpp::swagger("/openapi.json"), s, "/docs");
        put_on(httpp::directory(dir.root.string()), s, "/static");
        put_on(httpp::file_browser(dir.root.string()), s, "/files");
    });

    auto docs = httpp::client::fetch(srv.url("/docs"));
    ASSERT_EQ(200, docs.status);
    EXPECT_TRUE(contains(docs.header("Content-Type"), "text/html"));

    auto file = httpp::client::fetch(srv.url("/static/a.txt"));
    ASSERT_EQ(200, file.status);
    EXPECT_TRUE(file.body == "hello from the folder");

    auto browser = httpp::client::fetch(srv.url("/files"));
    ASSERT_EQ(200, browser.status);
    EXPECT_TRUE(contains(browser.body, "a.txt") || contains(browser.header("Content-Type"), "text/html"));
}

UTEST(httpp_mountable, a_list_of_different_parts_is_mounted_in_one_loop) {
    folder dir;
    std::vector<std::pair<std::unique_ptr<httpp::mountable>, std::string>> parts;
    parts.emplace_back(std::make_unique<httpp::swagger>("/openapi.json"), "/api-docs");
    parts.emplace_back(std::make_unique<httpp::directory>(dir.root.string()), "/assets");

    local_server srv([&](httpp::server& s) {
        for (const auto& [part, base] : parts) part->mount(s, base);  // the parts are owned through the base: its destructor is virtual
    });
    EXPECT_EQ(200, httpp::client::fetch(srv.url("/api-docs")).status);
    EXPECT_EQ(200, httpp::client::fetch(srv.url("/assets/a.txt")).status);
    EXPECT_EQ(404, httpp::client::fetch(srv.url("/docs")).status);  // nothing mounted at the swagger default: the base we gave was used
}

UTEST(httpp_mountable, the_derived_classes_keep_their_own_default_base) {
    folder dir;
    local_server srv([&](httpp::server& s) {
        httpp::swagger("/openapi.json").mount(s);                    // "/docs"
        httpp::directory(dir.root.string()).mount(s);                // "/"
    });
    EXPECT_EQ(200, httpp::client::fetch(srv.url("/docs")).status);
    auto file = httpp::client::fetch(srv.url("/a.txt"));
    ASSERT_EQ(200, file.status);
    EXPECT_TRUE(file.body == "hello from the folder");
}

UTEST(httpp_mountable, a_bad_base_throws_through_the_base_too) {
    httpp::server s;
    httpp::swagger sw("/openapi.json");
    const httpp::mountable& as_base = sw;
    bool threw = false;
    try {
        as_base.mount(s, "docs");  // no leading '/'
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    EXPECT_TRUE(threw);
}

UTEST(httpp_mountable, mount_returns_the_handle_so_a_chain_goes_on) {
    local_server srv([](httpp::server& s) {
        const httpp::swagger sw("/openapi.json");
        const httpp::mountable& as_base = sw;
        httpp::server back = as_base.mount(s, "/docs");  // the handle of the same server
        back.get("/after", [](const httpp::server_request&, httpp::response& res) { res.json("{}"); });
    });
    EXPECT_EQ(200, httpp::client::fetch(srv.url("/docs")).status);
    EXPECT_EQ(200, httpp::client::fetch(srv.url("/after")).status);  // a route added through the returned handle is on the same server
}

UTEST(httpp_mountable, a_part_is_still_a_copyable_value) {
    local_server srv([](httpp::server& s) {
        httpp::swagger a("/openapi.json");
        a.title("first-title");
        httpp::swagger b = a;
        b.title("second-title");  // the copy is independent of the original
        a.mount(s, "/a");
        b.mount(s, "/b");
    });
    EXPECT_TRUE(contains(httpp::client::fetch(srv.url("/a")).body, "first-title"));
    EXPECT_TRUE(contains(httpp::client::fetch(srv.url("/b")).body, "second-title"));
}

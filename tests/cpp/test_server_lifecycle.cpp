// What a program does around a server (ADR-0041 and 0081): `server` is a shared handle, mount() takes and returns it, bind() before
// listening, url(), listen_until_signal(), stopper(), the log level by name, the banner.
#include "utest/utest.h"
#include "httpp.h"
#include "httpp/banner.hpp"
#include "httpp/file_browser.hpp"
#include "httpp/server.hpp"
#include "httpp/swagger.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

namespace {

int port_of(const std::string& url) {  // "http://127.0.0.1:8000/" -> 8000
    return std::stoi(url.substr(url.rfind(':') + 1));
}

}  // namespace

// The dangling form cannot compile: a chain on a temporary hands back a server (a handle), never a reference into the temporary.
static_assert(std::is_same<decltype(std::declval<httpp::server&>().max_body(1)), httpp::server&>::value, "on a named server the chain returns a reference");
static_assert(std::is_same<decltype(std::declval<httpp::server&&>().max_body(1)), httpp::server>::value, "on a temporary the chain returns a handle");
static_assert(std::is_same<decltype(std::declval<httpp::server&&>().tls("a", "b")), httpp::server>::value, "tls on a temporary returns a handle");
static_assert(std::is_same<decltype(httpp::directory(".").mount(httpp::server(), "/")), httpp::server>::value, "mount returns a handle");

UTEST(httpp_mount, mount_returns_a_handle_to_the_server_it_was_given) {
    httpp::server srv;
    httpp::server a = httpp::directory(".").mount(srv, "/files");
    ASSERT_TRUE(a.same_as(srv));
    httpp::server b = httpp::file_browser(".").mount(srv, "/browse");
    ASSERT_TRUE(b.same_as(srv));
    httpp::server c = httpp::swagger("/openapi.json").mount(srv);
    ASSERT_TRUE(c.same_as(srv));
    httpp::server other;
    ASSERT_FALSE(other.same_as(srv));
}

UTEST(httpp_mount, the_owners_chain_works_with_a_temporary_server) {
    auto srv = httpp::directory(".").show_hidden(false).mount(httpp::server(), "/");
    ASSERT_TRUE(srv.bind("127.0.0.1", 0));
    ASSERT_TRUE(srv.url().rfind("http://127.0.0.1:", 0) == 0);
}

UTEST(httpp_mount, the_chain_goes_on_into_set_tls_which_names_the_file_it_cannot_read) {
    std::string message;
    try {
        auto srv = httpp::directory(".").mount(httpp::server(), "/").tls("no_such_cert.pem", "no_such_key.pem");
    } catch (const std::runtime_error& e) {
        message = e.what();
    }
    ASSERT_TRUE(message.find("no_such_cert.pem") != std::string::npos);

    httpp::server named;
    message.clear();
    try {
        httpp::directory(".").mount(named, "/").tls("no_such_cert.pem", "no_such_key.pem");
    } catch (const std::runtime_error& e) {
        message = e.what();
    }
    ASSERT_TRUE(message.find("no_such_cert.pem") != std::string::npos);
}

UTEST(httpp_server_handle, a_copy_shares_the_server_bind_through_one_and_url_through_the_other) {
    httpp::server a;
    httpp::server b = a;
    ASSERT_TRUE(a.same_as(b));
    ASSERT_STREQ("", b.url().c_str());
    ASSERT_TRUE(a.bind("127.0.0.1", 0));
    ASSERT_TRUE(b.url().rfind("http://127.0.0.1:", 0) == 0);
    ASSERT_STREQ(a.url().c_str(), b.url().c_str());

    httpp::server c;
    c = b;  // assignment gives another handle to the same server too
    ASSERT_TRUE(c.same_as(a));
}

UTEST(httpp_server_handle, mounting_through_one_handle_is_served_by_the_other) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "httpp_shared_handle_test";
    std::filesystem::create_directories(dir);
    { std::ofstream(dir / "hello.txt") << "hello"; }

    httpp::server a;
    httpp::server b = a;
    httpp::directory(dir.string()).mount(a, "/f");  // through `a`
    ASSERT_TRUE(b.bind("127.0.0.1", 0));
    const std::string url = b.url();
    std::thread t([b]() mutable { b.listen_after_bind(); });  // and `b` serves it
    auto res = httpp::client::fetch(url + "f/hello.txt");
    a.stop();
    t.join();
    ASSERT_EQ(200, res.status);
    ASSERT_STREQ("hello", res.body.c_str());
    std::filesystem::remove_all(dir);
}

UTEST(httpp_server_handle, the_last_handle_closes_the_server_and_its_port_can_be_bound_again) {
    int port = 0;
    {
        httpp::server a;
        httpp::server b = a;
        ASSERT_TRUE(a.bind("127.0.0.1", 0));
        port = port_of(a.url());
    }
    // every handle is gone: nothing holds the port
    httpp::server again;
    ASSERT_TRUE(again.bind("127.0.0.1", port));

    // one handle left is enough to keep it
    httpp::server keep;
    ASSERT_TRUE(keep.bind("127.0.0.1", 0));
    const int kept = port_of(keep.url());
    {
        httpp::server copy = keep;
    }  // the copy goes, the server stays
    httpp::server second;
    ASSERT_FALSE(second.bind("127.0.0.1", kept));
}

UTEST(httpp_server_handle, a_moved_handle_keeps_the_server) {
    httpp::server a;
    httpp::server witness = a;
    httpp::server moved = std::move(a);
    ASSERT_TRUE(moved.same_as(witness));
}

UTEST(httpp_server_stopper, does_not_keep_the_server_alive_and_does_nothing_once_it_is_gone) {
    httpp::server_stopper stop;
    ASSERT_FALSE(stop.alive());  // not tied to a server
    stop.stop();
    int port = 0;
    {
        httpp::server srv;
        ASSERT_TRUE(srv.bind("127.0.0.1", 0));
        port = port_of(srv.url());
        stop = srv.stopper();
        ASSERT_TRUE(stop.alive());
        httpp::server_stopper copy = stop;  // a stopper is copyable
        ASSERT_TRUE(copy.alive());
    }
    ASSERT_FALSE(stop.alive());
    stop.stop();  // no crash, nothing to stop
    stop.shutdown(std::chrono::milliseconds(10));
    httpp::server again;
    ASSERT_TRUE(again.bind("127.0.0.1", port));  // the stopper did not hold the port
}

UTEST(httpp_server_stopper, stops_a_listening_server_from_another_thread) {
    httpp::server srv;
    ASSERT_TRUE(srv.bind("127.0.0.1", 0));
    std::atomic<bool> finished{false};
    std::thread t([srv, &finished]() mutable {
        srv.listen_after_bind();
        finished = true;
    });
    const auto stop = srv.stopper();
    for (int i = 0; i < 400 && !finished; ++i) {  // stop() does nothing until the loop runs, so it is repeated
        stop.stop();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    t.join();
    ASSERT_TRUE(finished);
}

UTEST(httpp_server_stopper, a_handler_that_captures_a_stopper_can_end_the_server_and_nothing_leaks) {
    std::weak_ptr<int> probe;
    {
        auto token = std::make_shared<int>(1);  // lives exactly as long as the handler that captures it
        probe = token;
        httpp::server srv;
        srv.get("/quit", [stop = srv.stopper(), token](const httpp::server_request&, httpp::response& res) {
            stop.shutdown(std::chrono::seconds(2));
            res.body = "bye";
        });
        ASSERT_TRUE(srv.bind("127.0.0.1", 0));
        const std::string url = srv.url();
        std::thread t([srv]() mutable { srv.listen_after_bind(); });
        auto res = httpp::client::fetch(url + "quit");
        t.join();
        ASSERT_EQ(200, res.status);
    }
    // the last handle is gone: the server, its routes and the handler were freed (a captured `server` would have kept them forever)
    ASSERT_TRUE(probe.expired());
}

UTEST(httpp_server_url, is_empty_before_a_bind_and_names_the_bound_port_after) {
    httpp::server srv;
    ASSERT_STREQ("", srv.url().c_str());
    ASSERT_TRUE(srv.bind("127.0.0.1", 0));
    const std::string url = srv.url();
    ASSERT_TRUE(url.rfind("http://127.0.0.1:", 0) == 0);
    ASSERT_TRUE(url.back() == '/');
    ASSERT_TRUE(port_of(url) > 0);  // the port that was really bound, not the 0 that was asked for
}

UTEST(httpp_server_url, a_taken_port_does_not_bind_and_leaves_the_url_empty) {
    httpp::server first;
    ASSERT_TRUE(first.bind("127.0.0.1", 0));
    httpp::server second;
    ASSERT_FALSE(second.bind("127.0.0.1", port_of(first.url())));
    ASSERT_STREQ("", second.url().c_str());
}

UTEST(httpp_server_url, an_ipv6_host_is_in_brackets) {
    httpp::server srv;
    if (!srv.bind("::1", 0)) return;  // a machine without IPv6 loopback has nothing to check
    ASSERT_TRUE(srv.url().rfind("http://[::1]:", 0) == 0);
}

UTEST(httpp_server_listen_until_signal, refuses_to_run_before_a_bind) {
    httpp::server srv;
    ASSERT_FALSE(srv.listen_until_signal(std::chrono::milliseconds(100)));
}

UTEST(httpp_server_listen_until_signal, stops_on_sigterm_returns_true_and_puts_the_handler_back) {
    const auto before = std::signal(SIGTERM, SIG_DFL);
    std::signal(SIGTERM, before);

    httpp::server srv;
    ASSERT_TRUE(srv.bind("127.0.0.1", 0));
    bool result = false;
    std::thread server_thread([&] { result = srv.listen_until_signal(std::chrono::milliseconds(500)); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    std::raise(SIGTERM);  // the library's handler is installed while it listens
    server_thread.join();
    ASSERT_TRUE(result);

    const auto after = std::signal(SIGTERM, before);
    ASSERT_TRUE(after == before);  // and it is the one that was there before, not ours
}

UTEST(httpp_log_level_by_name, knows_the_names_of_the_one_level_type_in_any_case_and_refuses_others) {
    for (const char* name : {"trace", "debug", "INFO", "Warning", "warn", "error", "off", "25"}) ASSERT_TRUE(httpp::set_log_level(name));
    ASSERT_FALSE(httpp::set_log_level("verbose"));
    ASSERT_FALSE(httpp::set_log_level(""));
    httpp::set_log_level(httpp::logging::level::info);  // the rest of the suite expects the usual level
}

UTEST(httpp_banner, carries_the_version_and_is_written_only_to_a_terminal) {
    const std::string text = httpp::banner();
    const std::string version = std::to_string(HTTPP_VERSION_MAJOR) + "." + std::to_string(HTTPP_VERSION_MINOR) + "." + std::to_string(HTTPP_VERSION_PATCH);
    ASSERT_TRUE(text.find("  v" + version + "\n") != std::string::npos);

    std::FILE* file = std::tmpfile();  // a file is not a terminal
    ASSERT_TRUE(file != nullptr);
    ASSERT_FALSE(httpp::print_banner(file));
    ASSERT_EQ(0L, std::ftell(file));
    std::fclose(file);
    ASSERT_FALSE(httpp::print_banner(nullptr));
}

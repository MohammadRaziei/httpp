// What a program does around a server (ADR-0078): mount() hands the same server back, bind() before listening, url(), listen_until_signal(),
// the log level by name, the banner.
#include "utest/utest.h"
#include "httpp.h"
#include "httpp/banner.hpp"
#include "httpp/file_browser.hpp"
#include "httpp/server.hpp"
#include "httpp/swagger_ui.hpp"

#include <csignal>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

int port_of(const std::string& url) {  // "http://127.0.0.1:8000/" -> 8000
    return std::stoi(url.substr(url.rfind(':') + 1));
}

}  // namespace

UTEST(httpp_mount, mount_returns_the_server_it_was_given_and_not_a_copy) {
    httpp::server srv;
    httpp::server& a = httpp::directory(".").mount(srv, "/files");
    ASSERT_TRUE(&a == &srv);
    httpp::server& b = httpp::file_browser(".").mount(srv, "/browse");
    ASSERT_TRUE(&b == &srv);
    httpp::server& c = httpp::swagger_ui().mount(srv);
    ASSERT_TRUE(&c == &srv);
}

UTEST(httpp_mount, the_chain_goes_on_into_set_tls_which_names_the_file_it_cannot_read) {
    httpp::server srv;
    std::string message;
    try {
        httpp::directory(".").mount(srv, "/").set_tls("no_such_cert.pem", "no_such_key.pem");
    } catch (const std::runtime_error& e) {
        message = e.what();
    }
    ASSERT_TRUE(message.find("no_such_cert.pem") != std::string::npos);
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

UTEST(httpp_log_level_by_name, knows_the_six_names_in_any_case_and_refuses_others) {
    for (const char* name : {"trace", "debug", "INFO", "Warning", "error", "off"}) ASSERT_TRUE(httpp::set_log_level(name));
    ASSERT_FALSE(httpp::set_log_level("verbose"));
    ASSERT_FALSE(httpp::set_log_level(""));
    httpp::set_log_level(httpp::log_level::info);  // the rest of the suite expects the usual level
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

// The streaming body sink of the client engine (ADR-0052): a response body goes to a callback chunk by chunk
// instead of into response::body.
#include "utest/utest.h"
#include "local_server.hpp"
#include "internal/async_http.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <vector>

using httpp::detail::async_http;
using httpp::detail::async_job;
using httpp::detail::body_sink;

namespace {

constexpr std::size_t big = 6 * 1024 * 1024;

void add_routes(httpp::server& s) {
    s.get("/big", [](const httpp::server_request&, httpp::response& r) {
        r.status = 200;
        r.body.assign(big, 'q');
    });
    s.get("/missing", [](const httpp::server_request&, httpp::response& r) {
        r.status = 404;
        r.body = "not here";
    });
    s.get("/gz", [](const httpp::server_request&, httpp::response& r) {
        r.status = 200;
        r.headers.emplace_back("Content-Encoding", "gzip");
        r.body = "these bytes are not gzip, and must reach the sink untouched";
    });
}

async_job job_for(int port, const std::string& target, std::shared_ptr<body_sink> sink) {
    async_job j;
    j.host = "127.0.0.1";
    j.port = port;
    j.target = target;
    j.sink = std::move(sink);
    return j;
}

httpp::response run(async_http& eng, async_job job) {
    auto p = std::make_shared<std::promise<httpp::response>>();
    auto f = p->get_future();
    eng.submit(std::move(job), [p](httpp::response r) { p->set_value(std::move(r)); });
    if (f.wait_for(std::chrono::seconds(20)) != std::future_status::ready) {
        httpp::response r;
        r.error = httpp::error_kind::timeout;
        return r;
    }
    return f.get();
}

} // namespace

UTEST(async_stream, a_big_body_goes_to_the_sink_in_chunks_and_not_into_the_response) {
    testutil::local_server srv(add_routes);
    async_http eng;
    std::size_t total = 0, chunks = 0;
    int head_status = 0;
    bool data_before_head = false;
    auto sink = std::make_shared<body_sink>();
    sink->on_head = [&](const httpp::response& h) {
        head_status = h.status;
        return body_sink::verdict::stream;
    };
    sink->on_data = [&](const char* d, std::size_t n) {
        if (head_status == 0) data_before_head = true;
        for (std::size_t i = 0; i < n; i += 100000) if (d[i] != 'q') return false;
        total += n;
        ++chunks;
        return true;
    };
    auto r = run(eng, job_for(srv.port, "/big", sink));
    ASSERT_EQ(200, r.status);
    ASSERT_TRUE(r.error == httpp::error_kind::none);
    ASSERT_TRUE(r.body.empty());
    ASSERT_EQ(big, total);
    ASSERT_TRUE(chunks > 1);
    ASSERT_FALSE(data_before_head);
}

UTEST(async_stream, skip_discards_the_body_but_still_answers_with_the_head) {
    testutil::local_server srv(add_routes);
    async_http eng;
    std::size_t total = 0;
    auto sink = std::make_shared<body_sink>();
    sink->on_head = [](const httpp::response& h) {
        return h.status == 200 ? body_sink::verdict::stream : body_sink::verdict::skip;
    };
    sink->on_data = [&](const char*, std::size_t n) { total += n; return true; };
    auto r = run(eng, job_for(srv.port, "/missing", sink));
    ASSERT_EQ(404, r.status);
    ASSERT_TRUE(r.error == httpp::error_kind::none);
    ASSERT_TRUE(r.body.empty());
    ASSERT_EQ(0u, total);
}

UTEST(async_stream, a_sink_that_refuses_a_chunk_stops_the_exchange) {
    testutil::local_server srv(add_routes);
    async_http eng;
    std::size_t total = 0;
    auto sink = std::make_shared<body_sink>();
    sink->on_head = [](const httpp::response&) { return body_sink::verdict::stream; };
    sink->on_data = [&](const char*, std::size_t n) { total += n; return total < 100000; };
    auto r = run(eng, job_for(srv.port, "/big", sink));
    ASSERT_EQ(0, r.status);
    ASSERT_TRUE(r.error == httpp::error_kind::canceled);
    ASSERT_TRUE(total < big);
}

UTEST(async_stream, abort_from_the_head_ends_the_exchange_before_any_data) {
    testutil::local_server srv(add_routes);
    async_http eng;
    bool data = false;
    auto sink = std::make_shared<body_sink>();
    sink->on_head = [](const httpp::response&) { return body_sink::verdict::abort; };
    sink->on_data = [&](const char*, std::size_t) { data = true; return true; };
    auto r = run(eng, job_for(srv.port, "/big", sink));
    ASSERT_TRUE(r.error == httpp::error_kind::canceled);
    ASSERT_FALSE(data);
}

UTEST(async_stream, the_body_reaches_the_sink_exactly_as_sent_without_decoding) {
    testutil::local_server srv(add_routes);
    async_http eng;
    std::string got;
    auto sink = std::make_shared<body_sink>();
    sink->on_head = [](const httpp::response&) { return body_sink::verdict::stream; };
    sink->on_data = [&](const char* d, std::size_t n) { got.append(d, n); return true; };
    auto r = run(eng, job_for(srv.port, "/gz", sink));
    ASSERT_EQ(200, r.status);
    ASSERT_TRUE(got.find("these bytes are not gzip") == 0);
}

UTEST(async_stream, the_connection_is_reused_after_a_streamed_response) {
    testutil::local_server srv(add_routes);
    async_http eng;
    for (int i = 0; i < 3; ++i) {
        std::size_t total = 0;
        auto sink = std::make_shared<body_sink>();
        sink->on_head = [](const httpp::response&) { return body_sink::verdict::stream; };
        sink->on_data = [&](const char*, std::size_t n) { total += n; return true; };
        auto job = job_for(srv.port, "/big", sink);
        job.keep_alive = true;
        auto r = run(eng, std::move(job));
        ASSERT_EQ(200, r.status);
        ASSERT_EQ(big, total);
    }
}

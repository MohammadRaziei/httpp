// Which route answers a request: the first one registered (same method, same number of segments) that
// fits, where ":name" takes any non-empty segment and a literal segment is compared after percent-decoding.
// These pin that behaviour down, including against a straight re-implementation of the old linear scan.
#include "utest/utest.h"
#include "httpp/client.hpp"
#include "httpp/server.hpp"
#include "local_server.hpp"

#include <asio.hpp>

#include <cctype>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using testutil::local_server;

namespace {

// The client tidies a path ("//", a trailing "/"), so send the request line byte for byte instead.
httpp::response fetch(const local_server& srv, const std::string& path, const std::string& method = "GET") {
    asio::io_context io;
    asio::ip::tcp::socket sock(io);
    std::error_code ec;
    sock.connect({asio::ip::make_address("127.0.0.1"), static_cast<unsigned short>(srv.port)}, ec);
    const std::string req = method + " " + path + " HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
    asio::write(sock, asio::buffer(req), ec);
    std::string raw;
    char buf[4096];
    for (;;) {
        const std::size_t n = sock.read_some(asio::buffer(buf), ec);
        if (n == 0 || ec) break;
        raw.append(buf, n);
    }
    httpp::response res;
    if (raw.compare(0, 9, "HTTP/1.1 ") == 0) res.status = std::atoi(raw.c_str() + 9);
    const std::size_t head_end = raw.find("\r\n\r\n");
    if (head_end != std::string::npos) res.body = raw.substr(head_end + 4);
    return res;
}

// A route answers "<its id>|<name=value;...>".
void add(httpp::server& s, const std::string& method, const std::string& path, int id) {
    s.route(method, path, [id](const httpp::request& req, httpp::response& res) {
        std::string out = std::to_string(id) + "|";
        for (const auto& p : req.path_params) out += p.first + "=" + p.second + ";";
        res.body = out;
    });
}

// ---- the old algorithm, kept here as the oracle ----

std::vector<std::string> split(const std::string& path) {
    std::vector<std::string> parts;
    std::size_t pos = 1;
    while (pos <= path.size()) {
        std::size_t end = path.find('/', pos);
        if (end == std::string::npos) end = path.size();
        parts.push_back(path.substr(pos, end - pos));
        pos = end + 1;
    }
    return parts;
}

struct oracle_route {
    std::string method;
    std::vector<std::string> segs;
};

// `decoded` = the request path split into already-decoded segments
std::string oracle(const std::vector<oracle_route>& routes, const std::string& method, const std::vector<std::string>& decoded) {
    for (std::size_t i = 0; i < routes.size(); ++i) {
        const auto& r = routes[i];
        if (r.method != method || r.segs.size() != decoded.size()) continue;
        std::string params;
        bool ok = true;
        for (std::size_t k = 0; k < decoded.size() && ok; ++k) {
            const std::string& seg = r.segs[k];
            if (seg.size() > 1 && seg[0] == ':') {
                if (decoded[k].empty()) ok = false;
                else params += seg.substr(1) + "=" + decoded[k] + ";";
            } else {
                ok = seg == decoded[k];
            }
        }
        if (ok) return std::to_string(i) + "|" + params;
    }
    return "404";
}

} // namespace

UTEST(httpp_routing, the_first_registered_route_that_fits_wins_whatever_its_shape) {
    local_server a([](httpp::server& s) {
        add(s, "GET", "/users/:id", 0);
        add(s, "GET", "/users/me", 1); // shadowed by the route above
    });
    ASSERT_STREQ("0|id=me;", fetch(a, "/users/me").body.c_str());
    local_server b([](httpp::server& s) {
        add(s, "GET", "/users/me", 0);
        add(s, "GET", "/users/:id", 1);
    });
    ASSERT_STREQ("0|", fetch(b, "/users/me").body.c_str());
    ASSERT_STREQ("1|id=7;", fetch(b, "/users/7").body.c_str());
    local_server c([](httpp::server& s) {
        add(s, "GET", "/a/b", 0);
        add(s, "GET", "/a/b", 1); // registered twice: the first stays
    });
    ASSERT_STREQ("0|", fetch(c, "/a/b").body.c_str());
}

UTEST(httpp_routing, segments_are_decoded_before_they_are_compared_and_captured) {
    local_server srv([](httpp::server& s) {
        add(s, "GET", "/x y", 0);
        add(s, "GET", "/files/:name", 1);
        add(s, "GET", "/a/b", 2);
        add(s, "GET", "/:/colon", 3); // a lone ':' is an ordinary literal segment
    });
    ASSERT_STREQ("0|", fetch(srv, "/x%20y").body.c_str());
    ASSERT_STREQ("1|name=a/b;", fetch(srv, "/files/a%2Fb").body.c_str()); // one segment that contains a slash
    ASSERT_STREQ("1|name=caf\xC3\xA9;", fetch(srv, "/files/caf%C3%A9").body.c_str());
    const std::string encoded_slash = "/a%2Fb";  // outside the macro: utest uses the expression text as a printf format
    ASSERT_EQ(404, fetch(srv, encoded_slash).status); // decodes to ONE segment "a/b", not to /a/b
    ASSERT_STREQ("2|", fetch(srv, "/a/b").body.c_str());
    ASSERT_STREQ("3|", fetch(srv, "/:/colon").body.c_str());
}

UTEST(httpp_routing, a_parameter_needs_a_non_empty_segment_and_the_segment_count_must_match) {
    local_server srv([](httpp::server& s) {
        add(s, "GET", "/u/:id", 0);
        add(s, "GET", "/", 1);
        add(s, "GET", "/t/", 2); // a trailing slash is an empty last segment
    });
    ASSERT_STREQ("0|id=1;", fetch(srv, "/u/1").body.c_str());
    ASSERT_EQ(404, fetch(srv, "/u/").status);
    ASSERT_EQ(404, fetch(srv, "/u/1/2").status);
    ASSERT_EQ(404, fetch(srv, "/u").status);
    ASSERT_STREQ("1|", fetch(srv, "/").body.c_str());
    ASSERT_STREQ("2|", fetch(srv, "/t/").body.c_str());
    ASSERT_EQ(404, fetch(srv, "/t").status);
}

UTEST(httpp_routing, methods_are_kept_apart_and_head_uses_the_get_route) {
    local_server srv([](httpp::server& s) {
        add(s, "GET", "/m/:id", 0);
        add(s, "POST", "/m/:id", 1);
        add(s, "PUT", "/m/fixed", 2);
    });
    ASSERT_STREQ("0|id=5;", fetch(srv, "/m/5").body.c_str());
    ASSERT_STREQ("1|id=5;", fetch(srv, "/m/5", "POST").body.c_str());
    ASSERT_STREQ("2|", fetch(srv, "/m/fixed", "PUT").body.c_str());
    ASSERT_EQ(405, fetch(srv, "/m/fixed", "DELETE").status);
    ASSERT_EQ(200, fetch(srv, "/m/5", "HEAD").status);
}

UTEST(httpp_routing, hundreds_of_routes_each_answer_for_themselves) {
    local_server srv([](httpp::server& s) {
        for (int i = 0; i < 300; ++i) add(s, "GET", "/r" + std::to_string(i) + "/x", i);
        for (int i = 0; i < 300; ++i) add(s, "GET", "/p" + std::to_string(i) + "/:v", 1000 + i);
    });
    for (int i : {0, 1, 149, 299}) {
        ASSERT_STREQ((std::to_string(i) + "|").c_str(), fetch(srv, "/r" + std::to_string(i) + "/x").body.c_str());
        ASSERT_STREQ((std::to_string(1000 + i) + "|v=z;").c_str(), fetch(srv, "/p" + std::to_string(i) + "/z").body.c_str());
    }
    ASSERT_EQ(404, fetch(srv, "/r300/x").status);
}

UTEST(httpp_routing, random_routes_and_paths_agree_with_the_old_linear_scan) {
    std::mt19937 rng(12345);
    const std::vector<std::string> words = {"a", "b", "users", "x y", "caf\xC3\xA9", "a/b", ":id", ":name", ":", ":q", ""};
    std::vector<oracle_route> routes;
    local_server srv([&](httpp::server& s) {
        const char* methods[] = {"GET", "POST"};
        for (int i = 0; i < 40; ++i) {
            oracle_route r;
            r.method = methods[rng() % 2];
            const int n = 1 + static_cast<int>(rng() % 3);
            std::string path;
            for (int k = 0; k < n; ++k) {
                std::string w = words[rng() % words.size()];
                if (w.empty() || w == "a/b") w = "a"; // registered segments: no slash, not empty
                r.segs.push_back(w);
                path += "/" + w;
            }
            routes.push_back(r);
            add(s, r.method, path, i);
        }
    });
    auto encode = [](const std::string& w) { // what a client would send for this decoded segment
        std::string out;
        for (unsigned char c : w) {
            if ((c < 0x80 && std::isalnum(c)) || c == ':') out += static_cast<char>(c); // ASCII only: macOS's isalnum(0xC3) is true
            else { char buf[4]; std::snprintf(buf, sizeof buf, "%%%02X", c); out += buf; }
        }
        return out;
    };
    int checked = 0;
    for (int t = 0; t < 250; ++t) {
        const std::string method = (rng() % 2) ? "GET" : "POST";
        std::vector<std::string> decoded;
        std::string path;
        const int n = 1 + static_cast<int>(rng() % 3);
        for (int k = 0; k < n; ++k) {
            decoded.push_back(words[rng() % words.size()]);
            path += "/" + encode(decoded.back());
        }
        const std::string expected = oracle(routes, method, decoded);
        auto res = fetch(srv, path, method);
        const std::string got = (res.status == 404 || res.status == 405) ? "404" : res.body; // "no route for this method"
        if (expected != got) {
            std::fprintf(stderr, "MISMATCH %s %s status=%d expected=[%s] got=[%s]\n", method.c_str(), path.c_str(), res.status,
                         expected.c_str(), got.c_str());
            for (std::size_t i = 0; i < routes.size(); ++i) {
                std::string line;
                for (const auto& seg : routes[i].segs) line += "/" + seg;
                std::fprintf(stderr, "   route %zu %s %s\n", i, routes[i].method.c_str(), line.c_str());
            }
            ASSERT_STREQ(expected.c_str(), got.c_str());
        }
        ++checked;
    }
    ASSERT_EQ(250, checked);
}

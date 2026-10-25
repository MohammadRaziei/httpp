#pragma once

#include "httpp/export.hpp"
#include "httpp/server.hpp"

#include <cstddef>
#include <string>
#include <variant>
#include <vector>

namespace httpp {

// A group of routes that share a path prefix, built apart from any server and added with server::include().
// Nothing is looked up at request time: the prefixes are joined once, when the routes are registered, so a
// route costs the same with or without a router.
//
//   httpp::router users("/users");
//   users.get("/:id", [](const request& req, response& res) { ... });
//   httpp::router api("/api");
//   api.include(users);          // GET /api/users/:id
//   srv.include(api, "/v1");     // GET /v1/api/users/:id
//
// Rules: the order of joining is outer prefix, then the router's own prefix, then the route's path. A prefix gets a
// leading "/" and loses trailing ones ("" and "/" mean none). A route path "/" under a prefix is the prefix itself
// ("/users", never "/users/"). Methods are checked by server::include(), like server::route() does.
class router {
public:
    // One registered route: a method, a path relative to the router (its own prefix not included) and one handler.
    struct entry {
        std::string method;
        std::string path;
        std::variant<handler, async_handler, shared_handler, io_handler> h;
    };

    HTTPP_API explicit router(std::string prefix = "");

    // Same names and meanings as the server's route registration (see server.hpp).
    HTTPP_API router& route(const std::string& method, const std::string& path, handler h);
    HTTPP_API router& get(const std::string& path, handler h);
    HTTPP_API router& post(const std::string& path, handler h);
    HTTPP_API router& put(const std::string& path, handler h);
    HTTPP_API router& patch(const std::string& path, handler h);
    HTTPP_API router& del(const std::string& path, handler h);
    HTTPP_API router& route_async(const std::string& method, const std::string& path, async_handler h);
    HTTPP_API router& get_async(const std::string& path, async_handler h);
    HTTPP_API router& post_async(const std::string& path, async_handler h);
    HTTPP_API router& put_async(const std::string& path, async_handler h);
    HTTPP_API router& patch_async(const std::string& path, async_handler h);
    HTTPP_API router& del_async(const std::string& path, async_handler h);
    HTTPP_API router& route_shared(const std::string& method, const std::string& path, shared_handler h);
    HTTPP_API router& route_io(const std::string& method, const std::string& path, io_handler h);

    // Adds every route of `other` under `prefix` + other.prefix(). `other` may be changed or destroyed afterwards.
    HTTPP_API router& include(const router& other, const std::string& prefix = "");

    HTTPP_API const std::string& prefix() const;
    // Every route, paths relative to this router (the router's own prefix is not part of them).
    HTTPP_API const std::vector<entry>& routes() const;

private:
    std::string prefix_;
    std::vector<entry> routes_;
};

} // namespace httpp

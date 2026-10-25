#include "httpp/router.hpp"

#include <utility>

namespace httpp {
namespace {

// "" and "/" mean no prefix; otherwise one leading "/" and no trailing one.
std::string clean_prefix(std::string p) {
    while (!p.empty() && p.back() == '/') p.pop_back();
    if (!p.empty() && p.front() != '/') p.insert(p.begin(), '/');
    return p;
}

std::string clean_path(std::string p) {
    if (p.empty() || p.front() != '/') p.insert(p.begin(), '/');
    return p;
}

// prefix + path, where "/" under a prefix is the prefix itself. `prefix` is already clean.
std::string join(const std::string& prefix, const std::string& path) {
    const std::string p = clean_path(path);
    if (prefix.empty()) return p;
    return p == "/" ? prefix : prefix + p;
}

}  // namespace

router::router(std::string prefix) : prefix_(clean_prefix(std::move(prefix))) {}

router& router::route(const std::string& method, const std::string& path, handler h) {
    routes_.push_back({method, clean_path(path), std::variant<handler, async_handler, shared_handler, io_handler>(
                                                      std::in_place_type<handler>, std::move(h))});
    return *this;
}
router& router::route_async(const std::string& method, const std::string& path, async_handler h) {
    routes_.push_back({method, clean_path(path), std::variant<handler, async_handler, shared_handler, io_handler>(
                                                      std::in_place_type<async_handler>, std::move(h))});
    return *this;
}
router& router::route_shared(const std::string& method, const std::string& path, shared_handler h) {
    routes_.push_back({method, clean_path(path), std::variant<handler, async_handler, shared_handler, io_handler>(
                                                      std::in_place_type<shared_handler>, std::move(h))});
    return *this;
}
router& router::route_io(const std::string& method, const std::string& path, io_handler h) {
    routes_.push_back({method, clean_path(path), std::variant<handler, async_handler, shared_handler, io_handler>(
                                                      std::in_place_type<io_handler>, std::move(h))});
    return *this;
}

router& router::get(const std::string& path, handler h) { return route("GET", path, std::move(h)); }
router& router::post(const std::string& path, handler h) { return route("POST", path, std::move(h)); }
router& router::put(const std::string& path, handler h) { return route("PUT", path, std::move(h)); }
router& router::patch(const std::string& path, handler h) { return route("PATCH", path, std::move(h)); }
router& router::del(const std::string& path, handler h) { return route("DELETE", path, std::move(h)); }
router& router::get_async(const std::string& path, async_handler h) { return route_async("GET", path, std::move(h)); }
router& router::post_async(const std::string& path, async_handler h) { return route_async("POST", path, std::move(h)); }
router& router::put_async(const std::string& path, async_handler h) { return route_async("PUT", path, std::move(h)); }
router& router::patch_async(const std::string& path, async_handler h) { return route_async("PATCH", path, std::move(h)); }
router& router::del_async(const std::string& path, async_handler h) { return route_async("DELETE", path, std::move(h)); }

router& router::include(const router& other, const std::string& prefix) {
    const std::string outer = clean_prefix(prefix) + other.prefix_;
    std::vector<entry> copy;  // `other` may be this router
    copy.reserve(other.routes_.size());
    for (const entry& e : other.routes_) copy.push_back({e.method, join(outer, e.path), e.h});
    for (entry& e : copy) routes_.push_back(std::move(e));
    return *this;
}

const std::string& router::prefix() const { return prefix_; }
const std::vector<router::entry>& router::routes() const { return routes_; }

// ---- server::include (declared in server.hpp) ----
void server::include(const router& r, const std::string& prefix) {
    const std::string outer = clean_prefix(prefix) + r.prefix();
    for (const router::entry& e : r.routes()) {
        const std::string path = join(outer, e.path);
        switch (e.h.index()) {
            case 0: route(e.method, path, std::get<handler>(e.h)); break;
            case 1: route_async(e.method, path, std::get<async_handler>(e.h)); break;
            case 2: route_shared(e.method, path, std::get<shared_handler>(e.h)); break;
            default: route_io(e.method, path, std::get<io_handler>(e.h)); break;
        }
    }
}

}  // namespace httpp

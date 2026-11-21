#pragma once

#include "httpp/export.hpp"

#include <deque>
#include <initializer_list>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// A writer for OpenAPI 3.1 documents (ADR-0007). It does one thing: you describe an API with C++ calls and `to_json()` gives
// you correct JSON text, with every string escaped, the paths grouped, and every security scheme name checked against the
// declared ones. Nothing here reads JSON, so there is no parser, and nothing looks at your routes: the document is described
// apart from the handlers, so it can drift from them (Python builds its document from the routes themselves, ADR-0007).
//
//   #include <httpp/openapi.hpp>  // not part of httpp.h, like swagger.hpp
//
//   httpp::openapi::document doc("My API", "1.0.0");
//   doc.security_scheme("jwt", httpp::openapi::bearer("JWT"));
//   doc.security(httpp::openapi::any_of({"jwt"}));                     // the default for every operation
//   doc.get("/admin").summary("Admin").security(httpp::openapi::any_of({"basic"})).response(200, "OK");
//   doc.get("/health").no_security().response(200, "OK");               // public although the default asks for a JWT
//   app.get("/openapi.json", [&](const httpp::server_request&, httpp::response& res) { res.json(doc.to_json()); });
//
// Security is documentation for the Swagger UI "Authorize" button and nothing more: it checks no request. The check is
// yours (httpp::basic_auth, or your own code).
//
// Raw JSON. Where a schema or a part of the document is more than a few fields, you give it as JSON text (`schema_json`,
// `content_json`, `component_schema`, `raw`). The library does not parse it: it only refuses text that is blank or not valid
// UTF-8, and puts the rest in as it is. That text being valid JSON is your promise.
//
// Strings must be valid UTF-8; anything else throws std::invalid_argument from to_json(). A document is not thread safe; build
// it, then call to_json() (a const function) as often as you like.
namespace httpp::openapi {

// A security scheme object, as JSON text. Make one with basic(), bearer(), api_key() or raw_scheme().
struct scheme {
    std::string json;
};

// HTTP Basic (RFC 7617). An empty `description` leaves the field out.
HTTPP_API scheme basic(const std::string& description = "");
// A bearer token (`Authorization: Bearer <token>`); `bearer_format` ("JWT") is only a hint for readers of the document.
HTTPP_API scheme bearer(const std::string& bearer_format = "", const std::string& description = "");
// A key called `name` in a "header" (the default), a "query" parameter or a "cookie". Throws std::invalid_argument for an empty
// name or any other place.
HTTPP_API scheme api_key(const std::string& name, const std::string& in = "header", const std::string& description = "");
// Any other scheme object (OAuth2, OpenID Connect, mutualTLS) as JSON text, put in as it is.
HTTPP_API scheme raw_scheme(std::string json);

// One security requirement: every scheme in it is needed together (and), each with its scopes (only OAuth2 and OpenID Connect
// use them). An empty requirement means that credentials are optional.
using requirement = std::vector<std::pair<std::string, std::vector<std::string>>>;
// A list of requirements: any one of them is enough (or). An empty list means "none", which marks something as public.
using requirements = std::vector<requirement>;

// The common case: each name is a requirement on its own, with no scopes ("any of these").
inline requirements any_of(std::initializer_list<std::string> names) {
    requirements out;
    for (const std::string& name : names) {
        requirement one;
        one.emplace_back(name, std::vector<std::string>{});
        out.push_back(std::move(one));
    }
    return out;
}

class document;

// One operation (a method on a path). Get it from document::add() or document::get(); every setter returns the operation, so
// the calls chain. Nothing is checked until document::to_json().
class operation {
public:
    operation() = default;
    operation(std::string method, std::string path) : method_(std::move(method)), path_(std::move(path)) {}

    operation& summary(std::string text) { summary_ = std::move(text); return *this; }
    operation& description(std::string text) { description_ = std::move(text); return *this; }
    operation& operation_id(std::string id) { operation_id_ = std::move(id); return *this; }  // unique in the document
    operation& tag(std::string name) { tags_.push_back(std::move(name)); return *this; }
    operation& deprecated(bool on = true) { deprecated_ = on; return *this; }

    // What this operation needs. Not calling it leaves the document's default; no_security() marks it public.
    operation& security(requirements r) { security_ = std::move(r); return *this; }
    operation& no_security() { security_ = requirements{}; return *this; }

    // A parameter in "path" (always required), "query", "header" or "cookie". `schema_json` is the schema as JSON text.
    operation& parameter(std::string name, std::string in, bool required, std::string schema_json = "{\"type\":\"string\"}",
                         std::string description = "") {
        parameters_.push_back({std::move(name), std::move(in), std::move(schema_json), std::move(description), required, false});
        return *this;
    }
    // A whole parameter object as JSON text, for what parameter() does not say (examples, style, ...).
    operation& parameter_json(std::string object_json) {
        parameters_.push_back({"", "", std::move(object_json), "", false, true});
        return *this;
    }
    // The requestBody object as JSON text.
    operation& request_body(std::string object_json) { request_body_ = std::move(object_json); return *this; }
    // A response: its status ("200", "default", "2XX"), its description (required by OpenAPI) and, when it has a body, the
    // `content` object as JSON text. The same status twice replaces the first.
    operation& response(std::string status, std::string description, std::string content_json = "") {
        for (response_entry& r : responses_) {
            if (r.status == status) {
                r.description = std::move(description);
                r.content_json = std::move(content_json);
                return *this;
            }
        }
        responses_.push_back({std::move(status), std::move(description), std::move(content_json)});
        return *this;
    }
    operation& response(int status, std::string description, std::string content_json = "") {
        return response(std::to_string(status), std::move(description), std::move(content_json));
    }
    // Any other key of the operation object (an extension "x-...", "callbacks", ...) with its value as JSON text. The keys this
    // class writes itself are refused by to_json().
    operation& raw(std::string key, std::string json) { extra_.emplace_back(std::move(key), std::move(json)); return *this; }

private:
    friend class document;
    struct param_entry {
        std::string name, in, schema_json, description;
        bool required;
        bool is_object;  // schema_json holds the whole parameter object
    };
    struct response_entry {
        std::string status, description, content_json;
    };

    std::string method_, path_;
    std::optional<std::string> summary_, description_, operation_id_, request_body_;
    std::vector<std::string> tags_;
    bool deprecated_ = false;
    std::optional<requirements> security_;
    std::vector<param_entry> parameters_;
    std::vector<response_entry> responses_;
    std::vector<std::pair<std::string, std::string>> extra_;
};

// The whole document.
class document {
public:
    // `title` and `version` are required by OpenAPI; an empty one throws std::invalid_argument.
    HTTPP_API document(std::string title, std::string version);

    HTTPP_API document& description(std::string text);
    // Declares a scheme under `name`; the same name again replaces it. Every name a requirement uses must be declared.
    HTTPP_API document& security_scheme(std::string name, scheme s);
    // The default requirement of every operation that does not set its own. Not calling it writes no default.
    HTTPP_API document& security(requirements r);
    // A named schema under components.schemas (`json` is the schema as JSON text); the same name again replaces it.
    HTTPP_API document& component_schema(std::string name, std::string json);
    // Any other top-level key ("servers", "tags", "x-..."), its value as JSON text. The keys this class writes itself are
    // refused by to_json().
    HTTPP_API document& raw(std::string key, std::string json);

    // Adds an operation and returns it, to chain the setters. `method` is get, put, post, delete, options, head, patch or trace in any
    // case; `path` starts with "/" and may hold {names}. The same method on the same path twice throws std::invalid_argument.
    // The reference stays valid for the life of the document.
    HTTPP_API operation& add(std::string method, std::string path);
    // The same for an operation made apart (`operation("get", "/x")`).
    HTTPP_API document& add(operation op);
    operation& get(std::string path) { return add("get", std::move(path)); }
    operation& post(std::string path) { return add("post", std::move(path)); }
    operation& put(std::string path) { return add("put", std::move(path)); }
    operation& patch(std::string path) { return add("patch", std::move(path)); }
    operation& del(std::string path) { return add("delete", std::move(path)); }

    // The document as compact JSON text: the paths in the order they were first added, the methods of a path in the order
    // added. Throws std::invalid_argument for a security name that was not declared, a duplicate operation id, a parameter that
    // is wrong (an unknown place, a path parameter that is not required, the same name and place twice), a reserved key given to
    // raw(), a blank piece of raw JSON, or text that is not UTF-8. The message names the operation.
    HTTPP_API std::string to_json() const;

private:
    std::string title_, version_;
    std::optional<std::string> description_;
    std::vector<std::pair<std::string, scheme>> schemes_;
    std::optional<requirements> security_;
    std::vector<std::pair<std::string, std::string>> schemas_, extra_;
    std::deque<operation> operations_;  // a deque: the references add() hands out stay valid
};

} // namespace httpp::openapi

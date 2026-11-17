#include "httpp/openapi.hpp"

#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace httpp::openapi {

namespace {

[[noreturn]] void fail(const std::string& message) {
    throw std::invalid_argument("httpp::openapi: " + message);
}

// RFC 3629: no overlong forms, no surrogates, nothing above U+10FFFF.
bool valid_utf8(std::string_view s) {
    std::size_t i = 0;
    const std::size_t n = s.size();
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) { ++i; continue; }
        std::size_t len;
        unsigned cp;
        if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1Fu; }
        else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0Fu; }
        else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07u; }
        else return false;
        if (i + len > n) return false;
        for (std::size_t k = 1; k < len; ++k) {
            const unsigned char t = static_cast<unsigned char>(s[i + k]);
            if ((t & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (t & 0x3Fu);
        }
        if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) || cp > 0x10FFFF ||
            (cp >= 0xD800 && cp <= 0xDFFF)) {
            return false;
        }
        i += len;
    }
    return true;
}

// A JSON string literal. `what` names the value for the error message.
void put_string(std::string& out, std::string_view s, const std::string& what) {
    if (!valid_utf8(s)) fail(what + " is not valid UTF-8");
    out += '"';
    for (const char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out += ch;
                }
        }
    }
    out += '"';
}

// Raw JSON text goes in as it is. There is no parser: only a blank value or one that is not UTF-8 is refused.
void put_raw(std::string& out, const std::string& json, const std::string& what) {
    if (json.find_first_not_of(" \t\r\n") == std::string::npos) fail(what + " is blank: give JSON text");
    if (!valid_utf8(json)) fail(what + " is not valid UTF-8");
    out += json;
}

// "{" "key":value "," "key":value "}", writing the commas.
class object {
public:
    explicit object(std::string& out) : out_(out) {}
    void key(std::string_view k) {
        out_ += first_ ? '{' : ',';
        first_ = false;
        put_string(out_, k, "a key");
        out_ += ':';
    }
    void close() { out_ += first_ ? "{}" : "}"; }

private:
    std::string& out_;
    bool first_ = true;
};

// A scheme object is a few fields; this builds one without repeating the comma handling.
scheme make_scheme(std::initializer_list<std::pair<const char*, const std::string*>> fields) {
    std::string j;
    object o(j);
    for (const auto& [k, v] : fields) {
        if (v == nullptr || v->empty()) continue;  // an empty optional field is left out
        o.key(k);
        put_string(j, *v, k);
    }
    o.close();
    return scheme{std::move(j)};
}

bool one_of(const std::string& value, std::initializer_list<const char*> allowed) {
    return std::any_of(allowed.begin(), allowed.end(), [&](const char* a) { return value == a; });
}

std::string lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

const std::initializer_list<const char*> kMethods = {"get", "put", "post", "delete", "options", "head", "patch", "trace"};

const std::unordered_set<std::string> kReservedDocument = {"openapi", "info", "paths", "components", "security"};
const std::unordered_set<std::string> kReservedOperation = {"summary", "description", "operationId", "tags", "deprecated",
                                                            "security", "parameters", "requestBody", "responses"};

} // namespace

// --- scheme builders ---

scheme basic(const std::string& description) {
    const std::string type = "http", sch = "basic";
    return make_scheme({{"type", &type}, {"scheme", &sch}, {"description", &description}});
}

scheme bearer(const std::string& bearer_format, const std::string& description) {
    const std::string type = "http", sch = "bearer";
    return make_scheme({{"type", &type}, {"scheme", &sch}, {"bearerFormat", &bearer_format}, {"description", &description}});
}

scheme api_key(const std::string& name, const std::string& in, const std::string& description) {
    if (name.empty()) fail("api_key: the name must not be empty");
    if (!one_of(in, {"header", "query", "cookie"})) fail("api_key: `in` must be header, query or cookie, not '" + in + "'");
    const std::string type = "apiKey";
    return make_scheme({{"type", &type}, {"name", &name}, {"in", &in}, {"description", &description}});
}

scheme raw_scheme(std::string json) {
    if (json.find_first_not_of(" \t\r\n") == std::string::npos) fail("raw_scheme: the JSON text is blank");
    if (!valid_utf8(json)) fail("raw_scheme: the JSON text is not valid UTF-8");
    return scheme{std::move(json)};
}

// --- document: building ---

document::document(std::string title, std::string version) : title_(std::move(title)), version_(std::move(version)) {
    if (title_.empty()) fail("a document needs a title");
    if (version_.empty()) fail("a document needs a version");
}

document& document::description(std::string text) {
    description_ = std::move(text);
    return *this;
}

document& document::security_scheme(std::string name, scheme s) {
    if (name.empty()) fail("a security scheme needs a name");
    for (auto& entry : schemes_) {
        if (entry.first == name) {
            entry.second = std::move(s);
            return *this;
        }
    }
    schemes_.emplace_back(std::move(name), std::move(s));
    return *this;
}

document& document::security(requirements r) {
    security_ = std::move(r);
    return *this;
}

document& document::component_schema(std::string name, std::string json) {
    if (name.empty()) fail("a component schema needs a name");
    for (auto& entry : schemas_) {
        if (entry.first == name) {
            entry.second = std::move(json);
            return *this;
        }
    }
    schemas_.emplace_back(std::move(name), std::move(json));
    return *this;
}

document& document::raw(std::string key, std::string json) {
    if (key.empty()) fail("raw: the key must not be empty");
    extra_.emplace_back(std::move(key), std::move(json));
    return *this;
}

document& document::add(operation op) {
    op.method_ = lower(std::move(op.method_));
    if (!one_of(op.method_, kMethods)) {
        fail("'" + op.method_ + "' is not an HTTP method OpenAPI knows (get, put, post, delete, options, head, patch, trace)");
    }
    if (op.path_.empty() || op.path_.front() != '/') fail("a path starts with '/', not '" + op.path_ + "'");
    for (const operation& other : operations_) {
        if (other.method_ == op.method_ && other.path_ == op.path_) fail("there already is a " + op.method_ + " on " + op.path_);
    }
    operations_.push_back(std::move(op));
    return *this;
}

operation& document::add(std::string method, std::string path) {
    add(operation(std::move(method), std::move(path)));
    return operations_.back();
}

// --- document: writing ---

std::string document::to_json() const {
    const auto declared = [this](const std::string& name) {
        return std::any_of(schemes_.begin(), schemes_.end(), [&](const auto& e) { return e.first == name; });
    };
    const auto put_requirements = [&](std::string& out, const requirements& rs, const std::string& place) {
        out += '[';
        for (std::size_t i = 0; i < rs.size(); ++i) {
            if (i) out += ',';
            object one(out);
            for (const auto& [name, scopes] : rs[i]) {
                if (!declared(name)) {
                    fail(place + ": the security scheme '" + name + "' is not declared (declare it with security_scheme())");
                }
                one.key(name);
                out += '[';
                for (std::size_t k = 0; k < scopes.size(); ++k) {
                    if (k) out += ',';
                    put_string(out, scopes[k], "a scope of '" + name + "'");
                }
                out += ']';
            }
            one.close();
        }
        out += ']';
    };

    std::string out;
    out.reserve(256 + operations_.size() * 160);
    object doc(out);
    doc.key("openapi");
    out += "\"3.1.0\"";

    doc.key("info");
    {
        object info(out);
        info.key("title");
        put_string(out, title_, "the title");
        info.key("version");
        put_string(out, version_, "the version");
        if (description_ && !description_->empty()) {
            info.key("description");
            put_string(out, *description_, "the description");
        }
        info.close();
    }

    // the paths, in the order each was first added; the methods of one path in the order they were added
    std::vector<std::pair<std::string, std::vector<const operation*>>> groups;
    {
        std::unordered_map<std::string, std::size_t> at;
        for (const operation& op : operations_) {
            auto it = at.find(op.path_);
            if (it == at.end()) {
                it = at.emplace(op.path_, groups.size()).first;
                groups.emplace_back(op.path_, std::vector<const operation*>{});
            }
            groups[it->second].second.push_back(&op);
        }
    }

    doc.key("paths");
    {
        std::unordered_set<std::string> operation_ids;
        object paths(out);
        for (const auto& [path, ops] : groups) {
            paths.key(path);
            object item(out);
            for (const operation* op : ops) {
                const std::string here = op->method_ + " " + op->path_;  // for the error messages
                item.key(op->method_);
                object o(out);
                if (op->summary_) {
                    o.key("summary");
                    put_string(out, *op->summary_, here + ": the summary");
                }
                if (op->description_) {
                    o.key("description");
                    put_string(out, *op->description_, here + ": the description");
                }
                if (op->operation_id_) {
                    if (!operation_ids.insert(*op->operation_id_).second) {
                        fail(here + ": the operation id '" + *op->operation_id_ + "' is used twice");
                    }
                    o.key("operationId");
                    put_string(out, *op->operation_id_, here + ": the operation id");
                }
                if (!op->tags_.empty()) {
                    o.key("tags");
                    out += '[';
                    for (std::size_t i = 0; i < op->tags_.size(); ++i) {
                        if (i) out += ',';
                        put_string(out, op->tags_[i], here + ": a tag");
                    }
                    out += ']';
                }
                if (op->deprecated_) {
                    o.key("deprecated");
                    out += "true";
                }
                if (op->security_) {
                    o.key("security");
                    put_requirements(out, *op->security_, here);
                }
                if (!op->parameters_.empty()) {
                    o.key("parameters");
                    out += '[';
                    std::unordered_set<std::string> seen;
                    for (std::size_t i = 0; i < op->parameters_.size(); ++i) {
                        const auto& p = op->parameters_[i];
                        if (i) out += ',';
                        if (p.is_object) {
                            put_raw(out, p.schema_json, here + ": a parameter object");
                            continue;
                        }
                        if (p.name.empty()) fail(here + ": a parameter needs a name");
                        if (!one_of(p.in, {"path", "query", "header", "cookie"})) {
                            fail(here + ": the parameter '" + p.name + "' is in '" + p.in + "', which is not path, query, header or cookie");
                        }
                        if (p.in == "path" && !p.required) fail(here + ": the path parameter '" + p.name + "' must be required");
                        if (!seen.insert(p.in + '\n' + p.name).second) {
                            fail(here + ": the parameter '" + p.name + "' is declared twice in " + p.in);
                        }
                        object po(out);
                        po.key("name");
                        put_string(out, p.name, here + ": a parameter name");
                        po.key("in");
                        put_string(out, p.in, here + ": a parameter place");
                        po.key("required");
                        out += p.required ? "true" : "false";
                        if (!p.description.empty()) {
                            po.key("description");
                            put_string(out, p.description, here + ": a parameter description");
                        }
                        po.key("schema");
                        put_raw(out, p.schema_json, here + ": the schema of '" + p.name + "'");
                        po.close();
                    }
                    out += ']';
                }
                if (op->request_body_) {
                    o.key("requestBody");
                    put_raw(out, *op->request_body_, here + ": the request body");
                }
                if (!op->responses_.empty()) {
                    o.key("responses");
                    object rs(out);
                    for (const auto& r : op->responses_) {
                        if (r.status.empty()) fail(here + ": a response needs a status");
                        rs.key(r.status);
                        object ro(out);
                        ro.key("description");
                        put_string(out, r.description, here + ": the description of response " + r.status);
                        if (!r.content_json.empty()) {
                            ro.key("content");
                            put_raw(out, r.content_json, here + ": the content of response " + r.status);
                        }
                        ro.close();
                    }
                    rs.close();
                }
                for (const auto& [key, json] : op->extra_) {
                    if (kReservedOperation.count(key)) {
                        fail(here + ": '" + key + "' is written by the library; use its own setter, not raw()");
                    }
                    o.key(key);
                    put_raw(out, json, here + ": the value of '" + key + "'");
                }
                o.close();
            }
            item.close();
        }
        paths.close();
    }

    if (!schemas_.empty() || !schemes_.empty()) {
        doc.key("components");
        object components(out);
        if (!schemas_.empty()) {
            components.key("schemas");
            object schemas(out);
            for (const auto& [name, json] : schemas_) {
                schemas.key(name);
                put_raw(out, json, "the schema '" + name + "'");
            }
            schemas.close();
        }
        if (!schemes_.empty()) {
            components.key("securitySchemes");
            object ss(out);
            for (const auto& [name, s] : schemes_) {
                ss.key(name);
                put_raw(out, s.json, "the security scheme '" + name + "'");
            }
            ss.close();
        }
        components.close();
    }

    if (security_) {
        doc.key("security");
        put_requirements(out, *security_, "the default security");
    }

    for (const auto& [key, json] : extra_) {
        if (kReservedDocument.count(key)) fail("'" + key + "' is written by the library; use its own setter, not raw()");
        doc.key(key);
        put_raw(out, json, "the value of '" + key + "'");
    }
    doc.close();
    return out;
}

} // namespace httpp::openapi

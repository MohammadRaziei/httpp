#include "utest/utest.h"
#include "httpp/openapi.hpp"

#include <stdexcept>
#include <string>

namespace oa = httpp::openapi;

// True when `fn` throws std::invalid_argument whose message contains `needle`.
template <class Fn>
static bool throws_with(Fn fn, const std::string& needle) {
    try {
        fn();
    } catch (const std::invalid_argument& e) {
        return std::string(e.what()).find(needle) != std::string::npos;
    } catch (...) {
        return false;
    }
    return false;
}

UTEST(httpp_openapi, an_empty_document_is_the_smallest_valid_one) {
    oa::document doc("T", "1");
    EXPECT_STREQ(doc.to_json().c_str(), "{\"openapi\":\"3.1.0\",\"info\":{\"title\":\"T\",\"version\":\"1\"},\"paths\":{}}");
}

UTEST(httpp_openapi, a_title_and_a_version_are_required) {
    EXPECT_TRUE(throws_with([] { oa::document("", "1"); }, "title"));
    EXPECT_TRUE(throws_with([] { oa::document("T", ""); }, "version"));
}

UTEST(httpp_openapi, the_scheme_builders_write_openapi_scheme_objects) {
    EXPECT_STREQ(oa::basic().json.c_str(), "{\"type\":\"http\",\"scheme\":\"basic\"}");
    EXPECT_STREQ(oa::bearer().json.c_str(), "{\"type\":\"http\",\"scheme\":\"bearer\"}");
    EXPECT_STREQ(oa::bearer("JWT", "a token").json.c_str(),
                 "{\"type\":\"http\",\"scheme\":\"bearer\",\"bearerFormat\":\"JWT\",\"description\":\"a token\"}");
    EXPECT_STREQ(oa::api_key("X-Key").json.c_str(), "{\"type\":\"apiKey\",\"name\":\"X-Key\",\"in\":\"header\"}");
    EXPECT_STREQ(oa::api_key("k", "query").json.c_str(), "{\"type\":\"apiKey\",\"name\":\"k\",\"in\":\"query\"}");
    EXPECT_STREQ(oa::api_key("sid", "cookie", "the session").json.c_str(),
                 "{\"type\":\"apiKey\",\"name\":\"sid\",\"in\":\"cookie\",\"description\":\"the session\"}");
}

UTEST(httpp_openapi, the_scheme_builders_refuse_bad_arguments) {
    EXPECT_TRUE(throws_with([] { oa::api_key(""); }, "name"));
    EXPECT_TRUE(throws_with([] { oa::api_key("k", "body"); }, "header, query or cookie"));
    EXPECT_TRUE(throws_with([] { oa::raw_scheme("  "); }, "blank"));
    EXPECT_STREQ(oa::raw_scheme("{\"type\":\"mutualTLS\"}").json.c_str(), "{\"type\":\"mutualTLS\"}");
}

UTEST(httpp_openapi, a_scheme_a_default_and_a_per_operation_requirement) {
    oa::document doc("My API", "1.0.0");
    doc.security_scheme("jwt", oa::bearer("JWT"));
    doc.security_scheme("basic", oa::basic());
    doc.security(oa::any_of({"jwt"}));
    doc.get("/admin").summary("Admin").security(oa::any_of({"basic"})).response(200, "OK");
    doc.get("/health").no_security().response(200, "OK");
    EXPECT_STREQ(doc.to_json().c_str(),
                 "{\"openapi\":\"3.1.0\",\"info\":{\"title\":\"My API\",\"version\":\"1.0.0\"},\"paths\":{"
                 "\"/admin\":{\"get\":{\"summary\":\"Admin\",\"security\":[{\"basic\":[]}],\"responses\":{\"200\":{\"description\":\"OK\"}}}},"
                 "\"/health\":{\"get\":{\"security\":[],\"responses\":{\"200\":{\"description\":\"OK\"}}}}},"
                 "\"components\":{\"securitySchemes\":{\"jwt\":{\"type\":\"http\",\"scheme\":\"bearer\",\"bearerFormat\":\"JWT\"},"
                 "\"basic\":{\"type\":\"http\",\"scheme\":\"basic\"}}},\"security\":[{\"jwt\":[]}]}");
}

UTEST(httpp_openapi, an_operation_without_security_writes_no_security_key) {
    oa::document doc("T", "1");
    doc.security_scheme("jwt", oa::bearer());
    doc.get("/x");
    EXPECT_TRUE(doc.to_json().find("\"security\"") == std::string::npos);
}

UTEST(httpp_openapi, an_empty_default_is_written_as_an_empty_list) {
    oa::document doc("T", "1");
    doc.security(oa::requirements{});
    EXPECT_TRUE(doc.to_json().find("\"security\":[]") != std::string::npos);
}

UTEST(httpp_openapi, a_requirement_can_ask_for_schemes_together_and_for_scopes) {
    oa::document doc("T", "1");
    doc.security_scheme("basic", oa::basic());
    doc.security_scheme("key", oa::api_key("K"));
    doc.security_scheme("oauth", oa::raw_scheme("{\"type\":\"oauth2\",\"flows\":{}}"));
    oa::requirement both;
    both.emplace_back("basic", std::vector<std::string>{});
    both.emplace_back("key", std::vector<std::string>{});
    oa::requirement scoped;
    scoped.emplace_back("oauth", std::vector<std::string>{"read", "write"});
    doc.get("/x").security(oa::requirements{both, scoped});
    const std::string json = doc.to_json();
    EXPECT_TRUE(json.find("\"security\":[{\"basic\":[],\"key\":[]},{\"oauth\":[\"read\",\"write\"]}]") != std::string::npos);
}

UTEST(httpp_openapi, an_undeclared_scheme_throws_and_names_the_operation) {
    oa::document doc("T", "1");
    doc.security_scheme("jwt", oa::bearer());
    doc.get("/admin").security(oa::any_of({"basicc"}));
    EXPECT_TRUE(throws_with([&] { doc.to_json(); }, "get /admin"));
    EXPECT_TRUE(throws_with([&] { doc.to_json(); }, "'basicc' is not declared"));

    oa::document other("T", "1");
    other.security(oa::any_of({"nope"}));
    EXPECT_TRUE(throws_with([&] { other.to_json(); }, "the default security"));
}

UTEST(httpp_openapi, declaring_a_scheme_late_or_twice_works) {
    oa::document doc("T", "1");
    doc.get("/x").security(oa::any_of({"jwt"}));
    doc.security_scheme("jwt", oa::basic());
    doc.security_scheme("jwt", oa::bearer("JWT"));  // the same name replaces it, in place
    const std::string json = doc.to_json();
    EXPECT_TRUE(json.find("\"jwt\":{\"type\":\"http\",\"scheme\":\"bearer\",\"bearerFormat\":\"JWT\"}") != std::string::npos);
    EXPECT_TRUE(json.find("\"scheme\":\"basic\"") == std::string::npos);
}

UTEST(httpp_openapi, paths_keep_their_first_seen_order_and_methods_group_under_a_path) {
    oa::document doc("T", "1");
    doc.get("/a");
    doc.get("/b");
    doc.post("/a");
    doc.add("DELETE", "/b");  // any case
    EXPECT_STREQ(doc.to_json().c_str(),
                 "{\"openapi\":\"3.1.0\",\"info\":{\"title\":\"T\",\"version\":\"1\"},\"paths\":{\"/a\":{\"get\":{},\"post\":{}},\"/b\":{\"get\":{},\"delete\":{}}}}");
}

UTEST(httpp_openapi, operations_refuse_a_bad_method_a_bad_path_and_a_repeat) {
    oa::document doc("T", "1");
    EXPECT_TRUE(throws_with([&] { doc.add("fetch", "/x"); }, "not an HTTP method"));
    EXPECT_TRUE(throws_with([&] { doc.get("x"); }, "starts with '/'"));
    EXPECT_TRUE(throws_with([&] { doc.get(""); }, "starts with '/'"));
    doc.get("/x");
    EXPECT_TRUE(throws_with([&] { doc.get("/x"); }, "already is a get on /x"));
    doc.post("/x");  // another method on the same path is fine
}

UTEST(httpp_openapi, references_from_add_stay_valid_while_more_are_added) {
    oa::document doc("T", "1");
    oa::operation& first = doc.get("/first");
    for (int i = 0; i < 500; ++i) doc.get("/p" + std::to_string(i));
    first.summary("still here");  // would be a dangling write if the storage had moved
    EXPECT_TRUE(doc.to_json().find("\"/first\":{\"get\":{\"summary\":\"still here\"}}") != std::string::npos);
}

UTEST(httpp_openapi, an_operation_made_apart_can_be_added) {
    oa::document doc("T", "1");
    oa::operation op("Post", "/items");
    op.summary("Create").tag("items").tag("write").deprecated().operation_id("create_item");
    doc.add(op);
    EXPECT_TRUE(doc.to_json().find("\"/items\":{\"post\":{\"summary\":\"Create\",\"operationId\":\"create_item\","
                                   "\"tags\":[\"items\",\"write\"],\"deprecated\":true}}") != std::string::npos);
}

UTEST(httpp_openapi, every_string_is_escaped) {
    oa::document doc("A \"quoted\" title", "1");
    doc.description("line1\nline2\ttab \\ backslash\x01 control");
    doc.get("/x").summary("caf\xC3\xA9 \xF0\x9F\x9A\x80");  // UTF-8 passes through as it is
    const std::string json = doc.to_json();
    EXPECT_TRUE(json.find("\"title\":\"A \\\"quoted\\\" title\"") != std::string::npos);
    EXPECT_TRUE(json.find("\"description\":\"line1\\nline2\\ttab \\\\ backslash\\u0001 control\"") != std::string::npos);
    EXPECT_TRUE(json.find("\"summary\":\"caf\xC3\xA9 \xF0\x9F\x9A\x80\"") != std::string::npos);
}

UTEST(httpp_openapi, a_path_with_quotes_is_escaped_as_a_key_too) {
    oa::document doc("T", "1");
    doc.get("/a\"b");
    EXPECT_TRUE(doc.to_json().find("\"/a\\\"b\":{\"get\":{}}") != std::string::npos);
}

UTEST(httpp_openapi, text_that_is_not_utf8_throws_instead_of_writing_invalid_json) {
    const char* bad[] = {"\xFF", "\xC3", "\xC0\x80", "\xED\xA0\x80", "\xF4\x90\x80\x80", "a\xE2\x82"};
    for (const char* text : bad) {
        oa::document doc("T", "1");
        doc.get("/x").summary(text);
        EXPECT_TRUE(throws_with([&] { doc.to_json(); }, "UTF-8"));
    }
}

UTEST(httpp_openapi, parameters_a_body_and_responses) {
    oa::document doc("T", "1");
    doc.get("/items/{id}")
        .parameter("id", "path", true, "{\"type\":\"integer\"}", "the id")
        .parameter("q", "query", false)
        .request_body("{\"required\":true,\"content\":{}}")
        .response(200, "OK", "{\"application/json\":{\"schema\":{\"type\":\"object\"}}}")
        .response("default", "error");
    EXPECT_TRUE(doc.to_json().find(
                    "\"/items/{id}\":{\"get\":{\"parameters\":["
                    "{\"name\":\"id\",\"in\":\"path\",\"required\":true,\"description\":\"the id\",\"schema\":{\"type\":\"integer\"}},"
                    "{\"name\":\"q\",\"in\":\"query\",\"required\":false,\"schema\":{\"type\":\"string\"}}],"
                    "\"requestBody\":{\"required\":true,\"content\":{}},"
                    "\"responses\":{\"200\":{\"description\":\"OK\",\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\"}}}},"
                    "\"default\":{\"description\":\"error\"}}}}") != std::string::npos);
}

UTEST(httpp_openapi, the_same_response_status_twice_replaces_the_first) {
    oa::document doc("T", "1");
    doc.get("/x").response(200, "first").response(200, "second");
    const std::string json = doc.to_json();
    EXPECT_TRUE(json.find("first") == std::string::npos);
    EXPECT_TRUE(json.find("\"200\":{\"description\":\"second\"}") != std::string::npos);
}

UTEST(httpp_openapi, a_whole_parameter_object_goes_in_as_it_is) {
    oa::document doc("T", "1");
    doc.get("/x").parameter_json("{\"name\":\"n\",\"in\":\"query\",\"schema\":{},\"examples\":[1,2]}");
    EXPECT_TRUE(doc.to_json().find("\"parameters\":[{\"name\":\"n\",\"in\":\"query\",\"schema\":{},\"examples\":[1,2]}]") != std::string::npos);
}

UTEST(httpp_openapi, parameters_refuse_what_openapi_refuses) {
    {
        oa::document doc("T", "1");
        doc.get("/x/{id}").parameter("id", "path", false);
        EXPECT_TRUE(throws_with([&] { doc.to_json(); }, "must be required"));
    }
    {
        oa::document doc("T", "1");
        doc.get("/x").parameter("n", "body", true);
        EXPECT_TRUE(throws_with([&] { doc.to_json(); }, "not path, query, header or cookie"));
    }
    {
        oa::document doc("T", "1");
        doc.get("/x").parameter("n", "query", false).parameter("n", "query", false);
        EXPECT_TRUE(throws_with([&] { doc.to_json(); }, "declared twice"));
    }
    {
        oa::document doc("T", "1");
        doc.get("/x").parameter("n", "query", false).parameter("n", "header", false);  // another place is another parameter
        doc.to_json();
    }
}

UTEST(httpp_openapi, an_operation_id_may_be_used_once) {
    oa::document doc("T", "1");
    doc.get("/a").operation_id("same");
    doc.get("/b").operation_id("same");
    EXPECT_TRUE(throws_with([&] { doc.to_json(); }, "used twice"));
}

UTEST(httpp_openapi, component_schemas_and_extra_keys_are_put_in_as_raw_json) {
    oa::document doc("T", "1");
    doc.component_schema("Item", "{\"type\":\"object\"}");
    doc.component_schema("Item", "{\"type\":\"string\"}");  // replaces
    doc.security_scheme("basic", oa::basic());
    doc.raw("servers", "[{\"url\":\"/api\"}]");
    doc.get("/x").raw("x-internal", "true");
    EXPECT_STREQ(doc.to_json().c_str(),
                 "{\"openapi\":\"3.1.0\",\"info\":{\"title\":\"T\",\"version\":\"1\"},\"paths\":{\"/x\":{\"get\":{\"x-internal\":true}}},"
                 "\"components\":{\"schemas\":{\"Item\":{\"type\":\"string\"}},\"securitySchemes\":{\"basic\":{\"type\":\"http\",\"scheme\":\"basic\"}}},"
                 "\"servers\":[{\"url\":\"/api\"}]}");
}

UTEST(httpp_openapi, raw_json_that_is_blank_or_reserved_is_refused) {
    {
        oa::document doc("T", "1");
        doc.raw("servers", "   ");
        EXPECT_TRUE(throws_with([&] { doc.to_json(); }, "blank"));
    }
    {
        oa::document doc("T", "1");
        doc.raw("paths", "{}");
        EXPECT_TRUE(throws_with([&] { doc.to_json(); }, "written by the library"));
    }
    {
        oa::document doc("T", "1");
        doc.get("/x").raw("responses", "{}");
        EXPECT_TRUE(throws_with([&] { doc.to_json(); }, "use its own setter"));
    }
    {
        oa::document doc("T", "1");
        doc.component_schema("S", "");
        EXPECT_TRUE(throws_with([&] { doc.to_json(); }, "blank"));
    }
}

UTEST(httpp_openapi, to_json_is_const_and_repeatable_and_a_copy_is_independent) {
    oa::document doc("T", "1");
    doc.get("/x").summary("one");
    const oa::document& view = doc;
    const std::string first = view.to_json();
    EXPECT_STREQ(first.c_str(), view.to_json().c_str());
    oa::document copy = doc;
    copy.get("/y");
    EXPECT_STREQ(first.c_str(), doc.to_json().c_str());
    EXPECT_TRUE(copy.to_json().find("\"/y\"") != std::string::npos);
}

UTEST(httpp_openapi, a_failed_to_json_leaves_the_document_usable) {
    oa::document doc("T", "1");
    doc.get("/x").security(oa::any_of({"later"}));
    EXPECT_TRUE(throws_with([&] { doc.to_json(); }, "'later'"));
    doc.security_scheme("later", oa::basic());
    EXPECT_TRUE(doc.to_json().find("\"later\":[]") != std::string::npos);
}

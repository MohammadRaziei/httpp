// httpp/utils.hpp (ADR-0107): SHA-256 (the digest helpers and the checksum-file parser, ported from libspeech's model_files) and the
// environment helpers. Every environment test uses variable names of its own, so none depends on what the machine has set.
#include "utest/utest.h"
#include "httpp/utils.hpp"

#include <cctype>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using httpp::temp_path;

// ---- SHA-256 ----

namespace {
const std::string empty_digest = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
const std::string abc_digest = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

template <class F> bool throws(F f) {
    try { f(); } catch (const std::runtime_error&) { return true; }
    return false;
}
} // namespace

UTEST(httpp_sha256, known_vectors) {
    EXPECT_TRUE(httpp::sha256_hex("", 0) == empty_digest);
    EXPECT_TRUE(httpp::sha256_hex("abc", 3) == abc_digest);
    // two blocks, so the padding path is exercised too
    const std::string two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    EXPECT_TRUE(httpp::sha256_hex(two.data(), two.size()) ==
                "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

UTEST(httpp_sha256, a_file_is_hashed_in_chunks_to_the_same_digest) {
    temp_path dir("httpp_sha256_");
    const httpp::path file = dir / "file.bin";
    std::string body(200000, 'a');  // more than one read chunk
    { std::ofstream(file.native(), std::ios::binary) << body; }
    EXPECT_TRUE(httpp::sha256_file(file) == httpp::sha256_hex(body.data(), body.size()));
    EXPECT_TRUE(throws([&] { httpp::sha256_file("no_such_file_for_sha256.bin"); }));
}

UTEST(httpp_sha256, the_padding_edges_around_one_block) {
    // 55 bytes is the longest message that still fits one block with its padding, 56 needs a second one; 64 and 119/120 are the next edges
    static const struct { std::size_t length; const char* digest; } cases[] = {
    {1, "ca978112ca1bbdcafac231b39a23dc4da786eff8147c4e72b9807785afee48bb"},
    {55, "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318"},
    {56, "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a"},
    {63, "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34"},
    {64, "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb"},
    {65, "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0"},
    {119, "31eba51c313a5c08226adf18d4a359cfdfd8d2e816b13f4af952f7ea6584dcfb"},
    {120, "2f3d335432c70b580af0e8e1b3674a7c020d683aa5f73aaaedfdc55af904c21c"},
    {127, "c57e9278af78fa3cab38667bef4ce29d783787a2f731d4e12200270f0c32320a"},
    {128, "6836cf13bac400e9105071cd6af47084dfacad4e5e302c94bfed24e013afb73e"},
    {129, "c12cb024a2e5551cca0e08fce8f1c5e314555cc3fef6329ee994a3db752166ae"}};
    for (const auto& c : cases) {
        const std::string data(c.length, 'a');
        EXPECT_TRUE(httpp::sha256_hex(data.data(), data.size()) == c.digest);
    }
}

UTEST(httpp_sha256, a_million_a) {
    const std::string data(1000000, 'a');
    EXPECT_TRUE(httpp::sha256_hex(data.data(), data.size()) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

UTEST(httpp_sha256, files_of_every_awkward_size_agree_with_hashlib) {
    // sizes around the 64-byte block and the 64 KiB read chunk; the bytes are i % 251, so no block repeats
    static const struct { std::size_t length; const char* digest; } cases[] = {
    {0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
    {1, "6e340b9cffb37a989ca544e6bb780a2c78901d3fb33738768511a30617afa01d"},
    {63, "29af2686fd53374a36b0846694cc342177e428d1647515f078784d69cdb9e488"},
    {64, "fdeab9acf3710362bd2658cdc9a29e8f9c757fcf9811603a8c447cd1d9151108"},
    {65, "4bfd2c8b6f1eec7a2afeb48b934ee4b2694182027e6d0fc075074f2fabb31781"},
    {65535, "dda402a2c028f0cbbdbc5c6ebae965eed9c75f71236e7022b0386d3455d5ae2f"},
    {65536, "4b640d85ab3ba30fd02c9fc9db4a8928f416322ad27022ea58a65aaee68a4df2"},
    {65537, "237356e18b503616912abb8ffaed3a72591e397d4ac294c4637917d48a3f529d"},
    {131072, "feb1e4409d009e0ec502eaabe321f86b5197a881e9b765252ec8a75d6957596d"},
    {131073, "dd84db969f4ff2abb79c8c2fbc06e8d8e02c46d6481c958e057f7ad7a24c58a7"},
    {200000, "e24bc62381f1224fbbb74688663f8f9743b9680b193edd666835e97b06e730eb"}};
    temp_path dir("httpp_sha256_");
    for (const auto& c : cases) {
        std::string body(c.length, '\0');
        for (std::size_t i = 0; i < body.size(); ++i) body[i] = static_cast<char>(i % 251);
        const httpp::path file = dir / ("size_" + std::to_string(c.length) + ".bin");
        { std::ofstream out(file.native(), std::ios::binary); out << body; }
        EXPECT_TRUE(httpp::sha256_file(file) == c.digest);
        EXPECT_TRUE(httpp::sha256_hex(body.data(), body.size()) == c.digest);
    }
}

UTEST(httpp_sha256, checksum_file_forms) {
    EXPECT_TRUE(httpp::parse_sha256_checksum(abc_digest, "m.onnx") == abc_digest);
    EXPECT_TRUE(httpp::parse_sha256_checksum(abc_digest + "\n", "m.onnx") == abc_digest);
    EXPECT_TRUE(httpp::parse_sha256_checksum(abc_digest + "  m.onnx\r\n", "m.onnx") == abc_digest);
    EXPECT_TRUE(httpp::parse_sha256_checksum(abc_digest + " *m.onnx\n", "m.onnx") == abc_digest);  // sha256sum -b
    std::string upper = abc_digest;
    for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    EXPECT_TRUE(httpp::parse_sha256_checksum(upper, "m.onnx") == abc_digest);  // returned in lowercase
}

UTEST(httpp_sha256, checksum_file_rejects_everything_else) {
    const auto bad = [](const std::string& text) {
        return throws([&] { httpp::parse_sha256_checksum(text, "m.onnx"); });
    };
    EXPECT_TRUE(bad(""));
    EXPECT_TRUE(bad("abc"));                                              // too short
    EXPECT_TRUE(bad("<!DOCTYPE html><html><body>Not Found</body></html>"));  // an error page
    EXPECT_TRUE(bad(abc_digest + "  other.onnx\n"));                      // for another file
    EXPECT_TRUE(bad(abc_digest + "\n" + abc_digest + "\n"));              // two lines
    EXPECT_TRUE(bad(abc_digest + "x"));                                   // junk glued to the digest
    EXPECT_TRUE(bad("g" + abc_digest.substr(1)));                         // not hex
    EXPECT_TRUE(bad(std::string(2000, 'a')));                             // far too long for one line
}

// ---- environment variables ----

namespace env = httpp::env;

UTEST(httpp_env, get_says_nullopt_for_a_variable_that_is_not_set_and_the_value_for_one_that_is) {
    ASSERT_FALSE(env::get("HTTPP_TEST_ENV_GET").has_value());
    ASSERT_TRUE(env::set("HTTPP_TEST_ENV_GET", "value"));
    ASSERT_TRUE(env::get("HTTPP_TEST_ENV_GET").has_value());
    ASSERT_STREQ("value", env::get("HTTPP_TEST_ENV_GET")->c_str());
    ASSERT_TRUE(env::unset("HTTPP_TEST_ENV_GET"));
    ASSERT_FALSE(env::get("HTTPP_TEST_ENV_GET").has_value());
    ASSERT_TRUE(env::unset("HTTPP_TEST_ENV_GET")); // unsetting what is not there is fine
}

UTEST(httpp_env, set_replaces_the_value) {
    env::scope keep("HTTPP_TEST_ENV_REPLACE");
    ASSERT_TRUE(env::set("HTTPP_TEST_ENV_REPLACE", "one"));
    ASSERT_TRUE(env::set("HTTPP_TEST_ENV_REPLACE", "two"));
    ASSERT_STREQ("two", env::get("HTTPP_TEST_ENV_REPLACE")->c_str());
}

#ifndef _WIN32
UTEST(httpp_env, an_empty_value_is_a_value_where_the_system_has_one) {
    // Windows has no variable that is set and empty (set(name, "") removes it); everywhere else get() tells it from "not set".
    env::scope keep("HTTPP_TEST_ENV_EMPTY");
    ASSERT_TRUE(env::set("HTTPP_TEST_ENV_EMPTY", ""));
    ASSERT_TRUE(env::get("HTTPP_TEST_ENV_EMPTY").has_value());
    ASSERT_STREQ("", env::get("HTTPP_TEST_ENV_EMPTY")->c_str());
}
#endif

UTEST(httpp_env, a_name_that_cannot_be_a_variable_is_refused_and_found_nowhere) {
    ASSERT_FALSE(env::set("", "x"));
    ASSERT_FALSE(env::set("A=B", "x"));
    ASSERT_FALSE(env::unset(""));
    ASSERT_FALSE(env::unset("A=B"));
    ASSERT_FALSE(env::get("").has_value());
    ASSERT_FALSE(env::get("A=B").has_value());
}

UTEST(httpp_env, text_is_utf8_both_ways) {
    env::scope keep("HTTPP_TEST_ENV_UTF8");
    const std::string value = "caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x99\x82"; // é, €, a four-byte character
    ASSERT_TRUE(env::set("HTTPP_TEST_ENV_UTF8", value));
    ASSERT_TRUE(env::get("HTTPP_TEST_ENV_UTF8").has_value());
    ASSERT_TRUE(*env::get("HTTPP_TEST_ENV_UTF8") == value);
}

UTEST(httpp_env, first_takes_the_first_that_is_set_and_not_empty) {
    env::scope a("HTTPP_TEST_ENV_A");
    env::scope b("HTTPP_TEST_ENV_B");
    env::scope c("HTTPP_TEST_ENV_C");
    ASSERT_STREQ("", env::first({"HTTPP_TEST_ENV_A", "HTTPP_TEST_ENV_B", "HTTPP_TEST_ENV_C"}).c_str()); // none set
    ASSERT_STREQ("", env::first({}).c_str());

    env::set("HTTPP_TEST_ENV_C", "from c");
    ASSERT_STREQ("from c", env::first({"HTTPP_TEST_ENV_A", "HTTPP_TEST_ENV_B", "HTTPP_TEST_ENV_C"}).c_str());
    env::set("HTTPP_TEST_ENV_B", ""); // empty counts as not set, like curl's proxy variables
    ASSERT_STREQ("from c", env::first({"HTTPP_TEST_ENV_A", "HTTPP_TEST_ENV_B", "HTTPP_TEST_ENV_C"}).c_str());
    env::set("HTTPP_TEST_ENV_B", "from b");
    ASSERT_STREQ("from b", env::first({"HTTPP_TEST_ENV_A", "HTTPP_TEST_ENV_B", "HTTPP_TEST_ENV_C"}).c_str());
    env::set("HTTPP_TEST_ENV_A", "from a");
    ASSERT_STREQ("from a", env::first({"HTTPP_TEST_ENV_A", "HTTPP_TEST_ENV_B", "HTTPP_TEST_ENV_C"}).c_str());
    ASSERT_STREQ("from b", env::first({"HTTPP_TEST_ENV_B", "HTTPP_TEST_ENV_A"}).c_str()); // the order of the names decides
}

UTEST(httpp_env, scope_sets_for_its_lifetime_and_puts_the_old_value_back) {
    env::set("HTTPP_TEST_ENV_SCOPE", "before");
    {
        env::scope s("HTTPP_TEST_ENV_SCOPE", "inside");
        ASSERT_STREQ("inside", env::get("HTTPP_TEST_ENV_SCOPE")->c_str());
    }
    ASSERT_STREQ("before", env::get("HTTPP_TEST_ENV_SCOPE")->c_str());
    env::unset("HTTPP_TEST_ENV_SCOPE");
}

UTEST(httpp_env, scope_over_a_variable_that_was_not_set_removes_it_again) {
    ASSERT_FALSE(env::get("HTTPP_TEST_ENV_SCOPE2").has_value());
    {
        env::scope s("HTTPP_TEST_ENV_SCOPE2", "inside");
        ASSERT_TRUE(env::get("HTTPP_TEST_ENV_SCOPE2").has_value());
    }
    ASSERT_FALSE(env::get("HTTPP_TEST_ENV_SCOPE2").has_value());
}

UTEST(httpp_env, a_scope_without_a_value_removes_the_variable_and_gives_it_back) {
    env::set("HTTPP_TEST_ENV_SCOPE3", "kept");
    {
        env::scope s("HTTPP_TEST_ENV_SCOPE3");
        ASSERT_FALSE(env::get("HTTPP_TEST_ENV_SCOPE3").has_value());
    }
    ASSERT_STREQ("kept", env::get("HTTPP_TEST_ENV_SCOPE3")->c_str());
    env::unset("HTTPP_TEST_ENV_SCOPE3");
}

UTEST(httpp_env, scopes_nest_and_unwind_in_reverse) {
    env::set("HTTPP_TEST_ENV_NEST", "0");
    {
        env::scope one("HTTPP_TEST_ENV_NEST", "1");
        {
            env::scope two("HTTPP_TEST_ENV_NEST", "2");
            ASSERT_STREQ("2", env::get("HTTPP_TEST_ENV_NEST")->c_str());
        }
        ASSERT_STREQ("1", env::get("HTTPP_TEST_ENV_NEST")->c_str());
    }
    ASSERT_STREQ("0", env::get("HTTPP_TEST_ENV_NEST")->c_str());
    env::unset("HTTPP_TEST_ENV_NEST");
}

UTEST(httpp_env, restore_puts_it_back_now_and_only_once) {
    env::set("HTTPP_TEST_ENV_RESTORE", "before");
    {
        env::scope s("HTTPP_TEST_ENV_RESTORE", "inside");
        ASSERT_TRUE(s.restore());
        ASSERT_STREQ("before", env::get("HTTPP_TEST_ENV_RESTORE")->c_str());
        env::set("HTTPP_TEST_ENV_RESTORE", "changed after"); // the destructor must not undo what came after restore()
        ASSERT_FALSE(s.restore());
    }
    ASSERT_STREQ("changed after", env::get("HTTPP_TEST_ENV_RESTORE")->c_str());
    env::unset("HTTPP_TEST_ENV_RESTORE");
}

UTEST(httpp_env, a_moved_scope_hands_over_the_restore) {
    env::set("HTTPP_TEST_ENV_MOVE", "before");
    {
        std::vector<env::scope> kept; // a container needs a movable type
        kept.emplace_back("HTTPP_TEST_ENV_MOVE", "inside");
        for (int i = 0; i < 20; ++i) kept.emplace_back("HTTPP_TEST_ENV_MOVE_OTHER_" + std::to_string(i), "x"); // forces reallocations
        ASSERT_STREQ("inside", env::get("HTTPP_TEST_ENV_MOVE")->c_str());
    }
    ASSERT_STREQ("before", env::get("HTTPP_TEST_ENV_MOVE")->c_str());
    ASSERT_FALSE(env::get("HTTPP_TEST_ENV_MOVE_OTHER_0").has_value());
    env::unset("HTTPP_TEST_ENV_MOVE");
}

UTEST(httpp_env, threads_that_set_and_read_their_own_variables_do_not_disturb_each_other) {
    static constexpr int threads = 8;
    static constexpr int rounds = 300;  // static: MSVC refuses a local constexpr in the lambda below without a capture (C3493)
    std::vector<int> wrong(threads, 0);
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t)
        pool.emplace_back([t, &wrong] {
            const std::string name = "HTTPP_TEST_ENV_THREAD_" + std::to_string(t);
            for (int i = 0; i < rounds; ++i) {
                const std::string value = std::to_string(t) + ":" + std::to_string(i);
                if (!env::set(name, value)) ++wrong[t];
                const auto got = env::get(name);
                if (!got || *got != value) ++wrong[t];
                if (i % 50 == 49 && !env::unset(name)) ++wrong[t];
            }
            env::unset(name);
        });
    for (auto& th : pool) th.join();
    for (int t = 0; t < threads; ++t) ASSERT_EQ(0, wrong[t]);
}

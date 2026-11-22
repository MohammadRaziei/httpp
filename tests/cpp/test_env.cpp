#include "utest/utest.h"
#include "httpp/env.hpp"

#include <string>
#include <thread>
#include <vector>

// httpp::env: environment variables as UTF-8 text, with a scope that puts the old value back (ADR-0107).
// Every test uses names of its own, so none depends on what the machine has set.

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
    constexpr int threads = 8;
    constexpr int rounds = 300;
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

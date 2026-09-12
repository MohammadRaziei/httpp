#include "utest/utest.h"
#include "httpp/progress.hpp"

#include <vector>

UTEST(httpp_progressbar, bar_tracks_current_and_total) {
    httpp::progress::bar b(10, "test");
    ASSERT_EQ(std::size_t(10), b.total());
    ASSERT_EQ(std::size_t(0), b.current());

    b.update();       // +1
    ASSERT_EQ(std::size_t(1), b.current());

    b.update(4);      // +4
    ASSERT_EQ(std::size_t(5), b.current());

    b.set_progress(9);
    ASSERT_EQ(std::size_t(9), b.current());

    b.finish();
    ASSERT_TRUE(b.is_finished());
    ASSERT_EQ(std::size_t(10), b.current());
}

UTEST(httpp_progressbar, range_iterates_all_values_in_order) {
    std::vector<std::size_t> seen;
    for (auto i : httpp::progress::range(5, "trange-like")) {
        seen.push_back(i);
    }
    ASSERT_EQ(std::size_t(5), seen.size());
    for (std::size_t i = 0; i < seen.size(); ++i) {
        ASSERT_EQ(i, seen[i]);
    }
}

UTEST(httpp_progressbar, range_of_zero_is_empty) {
    int count = 0;
    for (auto i : httpp::progress::range(0)) {
        (void)i;
        ++count;
    }
    ASSERT_EQ(0, count);
}

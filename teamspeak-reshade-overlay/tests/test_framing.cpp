// SPDX-License-Identifier: MIT
#include "tsro/framing.hpp"
#include "tsro_test.hpp"

using namespace tsro;

TEST(framing, splits_complete_lines) {
    LineFramer framer;
    std::vector<std::string> out;
    CHECK(framer.feed("one\ntwo\nthree\n", out));
    CHECK_EQ(out.size(), std::size_t{3});
    CHECK_EQ(out[0], std::string("one"));
    CHECK_EQ(out[2], std::string("three"));
}

TEST(framing, reassembles_across_reads) {
    LineFramer framer;
    std::vector<std::string> out;
    CHECK(framer.feed("par", out));
    CHECK_EQ(out.size(), std::size_t{0});
    CHECK(framer.feed("tial me", out));
    CHECK_EQ(out.size(), std::size_t{0});
    CHECK(framer.feed("ssage\n", out));
    CHECK_EQ(out.size(), std::size_t{1});
    CHECK_EQ(out[0], std::string("partial message"));
}

TEST(framing, keeps_a_trailing_partial_line_pending) {
    LineFramer framer;
    std::vector<std::string> out;
    CHECK(framer.feed("done\nnot done", out));
    CHECK_EQ(out.size(), std::size_t{1});
    CHECK_EQ(framer.pending_bytes(), std::size_t{8});
}

TEST(framing, tolerates_crlf) {
    LineFramer framer;
    std::vector<std::string> out;
    CHECK(framer.feed("a\r\nb\r\n", out));
    CHECK_EQ(out.size(), std::size_t{2});
    CHECK_EQ(out[0], std::string("a"));
    CHECK_EQ(out[1], std::string("b"));
}

TEST(framing, drops_empty_lines) {
    LineFramer framer;
    std::vector<std::string> out;
    CHECK(framer.feed("\n\n\nreal\n\n", out));
    CHECK_EQ(out.size(), std::size_t{1});
    CHECK_EQ(out[0], std::string("real"));
}

TEST(framing, overflows_during_accumulation_not_after) {
    // The point of the cap: a peer that never sends a newline must be cut off while streaming,
    // not after we have buffered the whole flood.
    LineFramer framer(64);
    std::vector<std::string> out;
    CHECK(framer.feed(std::string(40, 'x'), out));
    CHECK(!framer.overflowed());
    CHECK(!framer.feed(std::string(40, 'x'), out));
    CHECK(framer.overflowed());
    CHECK_EQ(framer.pending_bytes(), std::size_t{0});
}

TEST(framing, refuses_further_input_until_reset) {
    LineFramer framer(16);
    std::vector<std::string> out;
    CHECK(!framer.feed(std::string(32, 'x'), out));
    CHECK(!framer.feed("short\n", out));
    CHECK_EQ(out.size(), std::size_t{0});
    framer.reset();
    CHECK(framer.feed("short\n", out));
    CHECK_EQ(out.size(), std::size_t{1});
}

TEST(framing, preserves_lines_completed_before_an_overflow) {
    LineFramer framer(32);
    std::vector<std::string> out;
    CHECK(!framer.feed("good\n" + std::string(64, 'x'), out));
    CHECK_EQ(out.size(), std::size_t{1});
    CHECK_EQ(out[0], std::string("good"));
}

TEST(framing, a_line_exactly_at_the_limit_is_accepted) {
    LineFramer framer(8);
    std::vector<std::string> out;
    CHECK(framer.feed("12345678\n", out));
    CHECK_EQ(out.size(), std::size_t{1});
    CHECK_EQ(out[0].size(), std::size_t{8});
}

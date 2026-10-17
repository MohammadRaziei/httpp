// httpp/sha256.hpp: the digest helpers and the checksum-file parser (ported from libspeech's model_files).
#include "utest/utest.h"
#include "httpp/sha256.hpp"

#include <cctype>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>

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
    const std::string path = "test_sha256_file.bin";
    std::string body(200000, 'a');  // more than one read chunk
    { std::ofstream(path, std::ios::binary) << body; }
    EXPECT_TRUE(httpp::sha256_file(path) == httpp::sha256_hex(body.data(), body.size()));
    std::remove(path.c_str());
    EXPECT_TRUE(throws([&] { httpp::sha256_file("no_such_file_for_sha256.bin"); }));
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

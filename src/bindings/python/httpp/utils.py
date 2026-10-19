"""Helpers that mirror httpp's C++ free functions (httpp/sha256.hpp). They live here, not in the main
`httpp` namespace:

    httpp.utils.sha256_hex(b"abc")            # 64 lowercase hex characters
    httpp.utils.sha256_file("big.iso")        # read in chunks, RuntimeError if it cannot be read
    httpp.utils.parse_sha256_checksum(text, "big.iso")   # the digest from a `sha256sum` line

They are bound so Python can check the C++ code against `hashlib`; for ordinary hashing `hashlib` is fine too.
"""

from .httpp_cy import sha256_hex, sha256_file, parse_sha256_checksum

__all__ = ["sha256_hex", "sha256_file", "parse_sha256_checksum"]

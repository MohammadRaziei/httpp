"""Helpers that mirror httpp's C++ free functions (httpp/sha256.hpp, and mime_type_for of httpp/server.hpp). They live here, not in the main
`httpp` namespace:

    httpp.utils.sha256_hex(b"abc")            # 64 lowercase hex characters
    httpp.utils.sha256_file("big.iso")        # read in chunks, RuntimeError if it cannot be read
    httpp.utils.parse_sha256_checksum(text, "big.iso")   # the digest from a `sha256sum` line
    httpp.utils.mime_type_for("logo.PNG")     # "image/png": the table behind serve_directory and file()

They are bound so Python can check the C++ code against `hashlib`; for ordinary hashing `hashlib` is fine too.
"""

from .httpp_cy import sha256_hex, sha256_file, parse_sha256_checksum, mime_type_for

__all__ = ["sha256_hex", "sha256_file", "parse_sha256_checksum", "mime_type_for"]

# stb_deflate (vendored, compressor extracted)

- Origin: libstb `src/third_party/stb/stb_image_write.h` (https://github.com/MohammadRaziei/libstb), which is stb_image_write.h
  v1.16 (https://github.com/nothings/stb) with libstb's rewrite of `stbi_zlib_compress` (the upstream one only writes fixed
  Huffman blocks from a 3-byte hash).
- Extracted: lines 828 to 1284 (the built-in compressor and `stbi_zlib_compress`), unchanged, between the verbatim markers
  in `stb_deflate.h`. The shim before it is httpp's.
- License: see LICENSE (MIT or public domain)
- Check: `diff` the marked section against that range of libstb's stb_image_write.h; it must print nothing.

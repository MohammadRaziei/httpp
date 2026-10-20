# stb_inflate (vendored, decoder extracted)

- Origin: stb_image.h v2.30, https://github.com/nothings/stb (MIT or public domain), via
  https://github.com/MohammadRaziei/libstb (`src/third_party/stb/stb_image.h`, unmodified there)
- Extracted: lines 4081 to 4594 ("public domain zlib decode v0.2"), unchanged, between the verbatim markers
  in `stb_inflate.h`. The shim before it is httpp's.
- License: see LICENSE (the stb notice, MIT or public domain)
- Check: `diff` the marked section against that range of stb_image.h; it must print nothing.

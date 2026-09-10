#pragma once

// Explicit export macro: with CXX_VISIBILITY_PRESET=hidden, nothing is
// exported from libhttpp.so by default (this is what keeps the vendored
// httplib symbols compiled into the .so out of the dynamic symbol table).
// Public httpp classes/functions are marked HTTPP_API to opt back in.
#if defined(_WIN32)
#  if defined(HTTPP_BUILDING_DLL)
#    define HTTPP_API __declspec(dllexport)
#  else
#    define HTTPP_API __declspec(dllimport)
#  endif
#else
#  define HTTPP_API __attribute__((visibility("default")))
#endif

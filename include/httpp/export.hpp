#pragma once

// Explicit export macro: with CXX_VISIBILITY_PRESET=hidden, nothing is
// exported from libhttpp.so by default (this is what keeps the vendored
// httplib symbols compiled into the .so out of the dynamic symbol table).
// Public httpp classes/functions are marked HTTPP_API to opt back in.
//
// A class that callers derive from (httpp::logging::handler) is the exception: it is marked HTTPP_API as a whole,
// because a derived class in another module needs its vtable and type information, which stay hidden otherwise;
// that header silences MSVC's C4251 for it.
//
// Convention: HTTPP_API goes on individual out-of-line member functions
// (constructors, destructors, methods implemented in src/core/*.cpp), never
// on the class/struct itself. Marking a whole class dllexport on MSVC
// requires every member - including private ones - to have "dll-interface"
// (MSVC warning C4251), which fires even for private std::string/std::vector
// members and even for the std::unique_ptr<impl> of the pimpl pattern
// itself. Exporting only the specific symbols that are actually implemented
// in the DLL avoids this entirely: purely inline types (simple data structs
// like httpp::response, httpp::request, httpp::download_result) need no
// HTTPP_API at all, since nothing about them is compiled into the DLL.
#if defined(_WIN32)
#  if defined(HTTPP_BUILDING_DLL)
#    define HTTPP_API __declspec(dllexport)
#  else
#    define HTTPP_API __declspec(dllimport)
#  endif
#else
#  define HTTPP_API __attribute__((visibility("default")))
#endif

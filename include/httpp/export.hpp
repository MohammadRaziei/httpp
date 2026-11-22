#pragma once

// Explicit export macro: with CXX_VISIBILITY_PRESET=hidden, nothing is
// exported from libhttpp.so by default (this is what keeps the vendored
// httplib symbols compiled into the .so out of the dynamic symbol table).
// Public httpp classes/functions are marked HTTPP_API to opt back in.
//
// A class that callers derive from (httpp::logging::handler) is the exception: it is marked HTTPP_API as a whole,
// because a derived class in another module needs its vtable and type information, which stay hidden otherwise;
// that header silences MSVC's C4251 for it.
// A class with virtual functions is the same case even when nobody derives from it in another module (httpp::mountable and the
// parts that derive from it): the copy and the destructor a caller compiles inline use the vtable, so the class is marked as a whole.
// The members of such a class carry HTTPP_MEMBER, not HTTPP_API: MSVC refuses dllexport/dllimport on a member of a class that
// already has it (C2487). HTTPP_MEMBER expands to nothing; it stays in the header because tests/python/test_cpp_python_parity.py
// finds the public names by HTTPP_API and by HTTPP_MEMBER.
//
// Convention: HTTPP_API goes on individual out-of-line member functions
// (constructors, destructors, methods implemented in src/core/*.cpp), never
// on the class/struct itself. Marking a whole class dllexport on MSVC
// requires every member - including private ones - to have "dll-interface"
// (MSVC warning C4251), which fires even for private std::string/std::vector
// members and even for the std::unique_ptr<impl> of the pimpl pattern
// itself. Exporting only the specific symbols that are actually implemented
// in the DLL avoids this entirely: purely inline types (simple data structs
// like httpp::response, httpp::server_request, httpp::download_result) need no
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

// Marks a public member of a class that is exported as a whole (see above). Expands to nothing.
#define HTTPP_MEMBER

// Marks a class whose type information must be one and the same in every module, without exporting its members: an exception that
// the caller throws and libhttpp catches (httpp::http_exception). With hidden visibility each module gets its own type_info, and
// libc++abi (macOS) compares those by address, so the catch does not match. The members that are defined in the library still
// carry HTTPP_API one by one, as above.
// Nothing is needed on Windows: MSVC matches exception types by name, and the class is not dllexport, so there is no C4251/C4275 to silence.
#if defined(_WIN32)
#  define HTTPP_TYPE
#else
#  define HTTPP_TYPE __attribute__((visibility("default")))
#endif

// The second export macro: httpp_utils (libhttpp_utils: logging, path, sformat, progress) is a shared library of its own, below
// httpp_core. On Windows a symbol must be dllexport in the DLL that defines it and dllimport everywhere else, which one macro for two
// DLLs cannot give: HTTPP_UTILS_API is dllexport only while httpp_utils's own sources are compiled (HTTPP_BUILDING_UTILS_DLL),
// HTTPP_API is dllexport only while httpp_core's are (HTTPP_BUILDING_DLL). Headers of httpp_utils (logging.hpp, path.hpp, progress.hpp)
// use HTTPP_UTILS_API; everything else keeps HTTPP_API.
#if defined(_WIN32)
#  if defined(HTTPP_BUILDING_UTILS_DLL)
#    define HTTPP_UTILS_API __declspec(dllexport)
#  else
#    define HTTPP_UTILS_API __declspec(dllimport)
#  endif
#else
#  define HTTPP_UTILS_API __attribute__((visibility("default")))
#endif

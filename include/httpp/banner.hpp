// The httpp mark, for a program that wants to show it when it starts (the `httpp server` and `httpp browse` commands do). The library
// never prints it by itself.
#pragma once

#include "httpp/export.hpp"

#include <cstdio>
#include <string>

namespace httpp {

// The mark and the library's version: a few lines of ASCII art, then "  v0.9.0" and a blank line.
HTTPP_API std::string banner();

// Writes banner() to `out` (stderr unless told otherwise), but only when `out` is a terminal, so a pipe, a log file and a CI run never
// see it. Returns whether it wrote.
HTTPP_API bool print_banner(std::FILE* out = stderr);

}  // namespace httpp

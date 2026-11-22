#pragma once

// SHA-256 moved to httpp/utils.hpp, next to the environment helpers (ADR-0107), and no longer needs a TLS library. This header stays so
// that code that includes it keeps building.
#include "httpp/utils.hpp"

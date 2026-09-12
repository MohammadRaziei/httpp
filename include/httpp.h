/*==============================================================================
 Copyright (c) 2026 Mohammad Raziei <mohammadraziei1375@gmail.com>

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in all
 copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 SOFTWARE.
 *============================================================================*/

/**
 @file httpp.h
 @date 2026-09-10
 @author Mohammad Raziei

 Single umbrella header for the httpp public API. `cmake/DynamicVersion.cmake`
 (and `version.py`) parse the HTTPP_VERSION_* macros below to derive
 PROJECT_VERSION — keep them as plain `#define NAME NUMBER` lines.
 */

#ifndef HTTPP_H
#define HTTPP_H

/*==============================================================================
 * MARK: - Version
 *============================================================================*/

#define HTTPP_VERSION_MAJOR 0
#define HTTPP_VERSION_MINOR 1
#define HTTPP_VERSION_PATCH 0

/*==============================================================================
 * MARK: - Public API
 *============================================================================*/

#include "httpp/export.hpp"
#include "httpp/url.hpp"
#include "httpp/client.hpp"
#include "httpp/server.hpp"
#include "httpp/progress.hpp"
#include "httpp/download.hpp"

#endif /* HTTPP_H */

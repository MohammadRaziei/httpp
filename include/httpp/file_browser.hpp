#pragma once

#include "httpp/export.hpp"
#include "httpp/server.hpp"

#include <cstddef>
#include <string>

namespace httpp {

// How the file browser looks and how much it lists. The defaults are a sensible, safe setup.
struct file_browser_options {
    std::string title = "Files";     // shown in the page header and the tab (escaped, so any text is safe)
    std::string theme = "auto";      // "auto" (follows the system), "light" or "dark"; the viewer can still switch it
    std::string accent;              // "#rrggbb" (any case) or empty for the default colour
    bool search = true;              // show the filter box (it filters the folder that is open, in the browser)
    bool show_hidden = false;        // false: names that start with '.' are not listed and answer 404 when asked for by name
    std::size_t max_entries = 5000;  // a folder with more entries is cut at this many and flagged "truncated" (must be above 0)
    std::string view = "list";       // first view: "list" or "grid"
    std::string sort = "name";       // first sort: "name", "size" or "date"
};

// A read-only file browser for the folder `root`, served under `base` ("/files", or "/" for the whole server):
//   GET base                   the page (a single self-contained HTML document, compiled into the library)
//   GET base/api/list?path=/a  the entries of a folder as JSON: {"path","entries":[{"name","type":"dir"|"file","size","mtime"}],"truncated","limit"}
//   GET base/raw/<path>        the file itself (Range and HEAD work); anything that could run code in the page's origin (html, svg, xml, js,
//                              unknown types) is always a download, and "?download=1" forces a download for any file
// Nothing can be written, renamed or deleted: no route accepts a body. The served root is a ceiling: ".." segments, backslashes, NUL, absolute paths and
// symlinks that lead out of it are refused, and a missing, hidden or outside path gets the same 404. Names that are not valid UTF-8 are left out of
// the listing. The `before` hook of the server runs first, so it is the place for authentication (this browser has none of its own).
// Throws std::invalid_argument for a bad `base` (must start with '/'), an `root` that is not a folder, the root of the file system, or a bad option.
HTTPP_API void mount_file_browser(server& s, const std::string& base, const path& root, const file_browser_options& options = {});

} // namespace httpp

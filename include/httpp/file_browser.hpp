#pragma once

#include "httpp/export.hpp"
#include "httpp/path.hpp"
#include "httpp/server.hpp"

#include <cstddef>
#include <string>

namespace httpp {

// A read-only file browser for the folder `root`. Configure it with the chained setters, then mount it on a server:
//
//   httpp::file_browser(root)
//       .set_title("Shared")
//       .set_theme(httpp::file_browser::theme_mode::dark)
//       .set_accent("#2a7de1")
//       .mount(server, "/files");
//
// What `mount(server, base)` serves ("/files", or "/" for the whole server):
//   GET base                   the page (a single self-contained HTML document, compiled into the library)
//   GET base/api/list?path=/a  the entries of a folder as JSON: {"path","entries":[{"name","type":"dir"|"file","size","mtime"}],"truncated","limit"}
//   GET base/raw/<path>        the file itself (Range and HEAD work); anything that could run code in the page's origin (html, svg, xml, js,
//                              unknown types) is a download, and "?download=1" forces a download for any file. set_safe_mode(false) shows
//                              html and svg instead (see there); the other active types stay downloads
// Nothing can be written, renamed or deleted: no route accepts a body. The served root is a ceiling: ".." segments, backslashes, NUL, absolute paths and
// symlinks that lead out of it are refused, and a missing, hidden or outside path gets the same 404. Names that are not valid UTF-8 are left out of
// the listing. The `before` hook of the server runs first, so it is the place for authentication (this browser has none of its own).
// The defaults are a sensible, safe setup. A setter throws std::invalid_argument for a value it cannot use and then leaves the browser unchanged.
class file_browser {
public:
    enum class theme_mode { system, light, dark };  // system follows the viewer's system setting; the viewer can still switch it
    enum class view_mode { list, grid };            // the first view
    enum class sort_key { name, size, date };       // the first sort

    // `root` is checked when the browser is mounted, not here.
    HTTPP_API explicit file_browser(path root);

    HTTPP_API file_browser& set_title(std::string title);          // shown in the page header and the tab (escaped, so any text is safe); default "Files"
    HTTPP_API file_browser& set_theme(theme_mode theme);           // default system
    HTTPP_API file_browser& set_accent(std::string colour);        // "#rrggbb" (any case) or empty for the default colour; default empty
    HTTPP_API file_browser& set_search(bool on);                   // the filter box (it filters the folder that is open, in the browser); default true
    HTTPP_API file_browser& set_show_hidden(bool on);              // false: names that start with '.' are not listed and answer 404 when asked for by name; default false
    HTTPP_API file_browser& set_max_entries(std::size_t count);    // a folder with more entries is cut at this many and flagged "truncated"; must be above 0; default 5000
    HTTPP_API file_browser& set_view(view_mode view);              // default list
    HTTPP_API file_browser& set_sort(sort_key sort);               // default name
    // true: html, svg, xml, js and unknown types are never shown, only downloaded (the dialog has no preview for them).
    // false: "Open raw" renders html and svg in the tab it opens, and the dialog shows an svg as an image (html stays text there). They are
    // sent behind a "Content-Security-Policy: sandbox" that stops every script and every request to another origin, so they cannot touch
    // the page or the viewer's session. xml, js and unknown types stay downloads. Default true.
    HTTPP_API file_browser& set_safe_mode(bool on);

    // Adds the routes to `s` under `base` (must start with '/'). Throws std::invalid_argument for a bad `base`, a root that is not a folder, or the root
    // of the file system. The browser can be mounted more than once (on several servers, or under several bases).
    HTTPP_API void mount(server& s, const std::string& base = "/") const;

private:
    path root_;
    std::string title_ = "Files";
    std::string accent_;
    theme_mode theme_ = theme_mode::system;
    view_mode view_ = view_mode::list;
    sort_key sort_ = sort_key::name;
    bool search_ = true;
    bool show_hidden_ = false;
    bool safe_mode_ = true;
    std::size_t max_entries_ = 5000;
};

} // namespace httpp

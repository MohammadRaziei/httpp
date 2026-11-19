#pragma once

#include "httpp/export.hpp"
#include "httpp/path.hpp"
#include "httpp/server.hpp"

#include <cstddef>
#include <string>

namespace httpp {

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4251) // the std members of the exported classes are private and never touched by a caller
#endif

// A read-only file browser for the folder `root`. Configure it with the chained setters, then mount it on a server:
//
//   httpp::file_browser(root)
//       .title("Shared")
//       .theme(httpp::file_browser::theme_mode::dark)
//       .accent("#2a7de1")
//       .mount(server, "/files");
//
// What `mount(server, base)` serves ("/files", or "/" for the whole server):
//   GET base                   the page (a single self-contained HTML document, compiled into the library)
//   GET base/api/list?path=/a  the entries of a folder as JSON: {"path","entries":[{"name","type":"dir"|"file","size","mtime"}],"truncated","limit"}
//   GET base/raw/<path>        the file itself (Range and HEAD work); anything that could run code in the page's origin (html, svg, xml, js,
//                              unknown types) is a download, and "?download=1" forces a download for any file. Unless safe_mode(true),
//                              html and svg are the exception: shown, but only behind a sandbox (see there); the other active types stay downloads
//   GET|POST base/api/zip      (only with zip(true)) a ZIP of a folder or of chosen names in it, streamed as it is read; GET takes "?path=/a&name=x&name=y",
//                              POST the same fields as a form. No "name": the whole folder "path". Nothing is compressed (it is a bundle, not a smaller file)
// Nothing can be written, renamed or deleted: the only route that reads a body is the ZIP request, and all it reads is names. The served root is a ceiling: ".." segments, backslashes, NUL, absolute paths and
// symlinks that lead out of it are refused, and a missing, hidden or outside path gets the same 404. Names that are not valid UTF-8 are left out of
// the listing. The `before` hook of the server runs first, so it is the place for authentication (this browser has none of its own).
// The defaults are a sensible, safe setup. A setter throws std::invalid_argument for a value it cannot use and then leaves the browser unchanged.
class HTTPP_API file_browser : public mountable {
public:
    enum class theme_mode { system, light, dark };  // system follows the viewer's system setting; the viewer can still switch it
    enum class view_mode { list, grid };            // the first view
    enum class sort_key { name, size, date };       // the first sort

    // `root` is checked when the browser is mounted, not here.
    HTTPP_MEMBER explicit file_browser(path root);

    HTTPP_MEMBER file_browser& title(std::string title);          // shown in the page header and the tab (escaped, so any text is safe); default "Files"
    HTTPP_MEMBER file_browser& theme(theme_mode theme);           // default system
    HTTPP_MEMBER file_browser& accent(std::string colour);        // "#rrggbb" (any case) or empty for the default colour; default empty
    HTTPP_MEMBER file_browser& search(bool on);                   // the filter box (it filters the folder that is open, in the browser); default true
    HTTPP_MEMBER file_browser& show_hidden(bool on);              // false: names that start with '.' are not listed and answer 404 when asked for by name; default false
    // false: no file or folder time anywhere. The page has no "Modified" column and cannot sort by date, `api/list` has no "mtime", the files under
    // `base/raw` carry no "Last-Modified" or "ETag" (see directory::show_dates) and a ZIP stamps every entry 1980-01-01. Default true.
    HTTPP_MEMBER file_browser& show_dates(bool on);
    HTTPP_MEMBER file_browser& max_entries(std::size_t count);    // a folder with more entries is cut at this many and flagged "truncated"; must be above 0; default 5000
    HTTPP_MEMBER file_browser& layout(view_mode view);              // default list
    HTTPP_MEMBER file_browser& sort_by(sort_key sort);               // default name
    // false (default): "Open raw" renders html and svg in the tab it opens, and the dialog shows an svg as an image (html stays text there).
    // They are sent behind a "Content-Security-Policy: sandbox" that stops every script and every request to another origin, so they cannot
    // touch the page or the viewer's session. xml, js and unknown types stay downloads.
    // true: nothing that could run code is ever shown, html and svg are only downloaded too (the dialog has no preview for them).
    HTTPP_MEMBER file_browser& safe_mode(bool on);
    // true: the page can select files and folders and download them as one ZIP, and `base/api/zip` answers. A folder is included with everything in it
    // (hidden names only with show_hidden; links that lead out of the root, and links to folders, are left out). Nothing is compressed. Off by default:
    // a request can make the server read a lot of disk, so turn it on where the viewers are trusted or behind the `before` hook.
    HTTPP_MEMBER file_browser& zip(bool on);
    // true (default): a small "Powered by httpp" at the bottom of the page, a link to the project on GitHub. false: no credit line.
    HTTPP_MEMBER file_browser& powered_by(bool on);
    // The most entries (files and folders together) one ZIP may hold; a request for more answers 413 before any byte is sent. Must be above 0; default 50000.
    HTTPP_MEMBER file_browser& max_zip_entries(std::size_t count);

    // Adds the routes to `s` under `base` (must start with '/'). Throws std::invalid_argument for a bad `base`, a root that is not a folder, or the root
    // of the file system. The browser can be mounted more than once (on several servers, or under several bases).
    // `s` is a handle (ADR-0081): the routes go on the server it points to, so the caller's own `server` sees them. Returns that handle.
    HTTPP_MEMBER server mount(server s, const std::string& base = "/") const override;

private:
    path root_;
    std::string title_ = "Files";
    std::string accent_;
    theme_mode theme_ = theme_mode::system;
    view_mode view_ = view_mode::list;
    sort_key sort_ = sort_key::name;
    bool search_ = true;
    bool show_hidden_ = false;
    bool show_dates_ = true;
    bool safe_mode_ = false;
    bool zip_ = false;
    bool powered_by_ = true;
    std::size_t max_zip_entries_ = 50000;
    std::size_t max_entries_ = 5000;
};
#ifdef _MSC_VER
#pragma warning(pop)
#endif

} // namespace httpp

#pragma once

// Request bodies that are read while they are sent (ADR-0113): one file as the whole body (PUT), or a multipart/form-data body made
// of text fields, in-memory parts and files. The engine pulls them through body_source (async_http.hpp); nothing here touches a socket.

#include "async_http.hpp"
#include "httpp/path.hpp"

#include <memory>
#include <string>
#include <vector>

namespace httpp::detail {

// One part of a multipart body, as the builder recorded it.
struct form_part {
    enum class kind { text, data, file };
    kind what = kind::text;
    std::string name;
    std::string value;        // text: the value; data: the bytes; file: the path on disk
    std::string filename;     // data and file: the name the server sees (file: the last part of the path when empty)
    std::string content_type; // data and file: empty = guessed from the file name
};

// The whole body is this file. Fails (null, `error` set) when the path is not a readable file. The length is read now; a file that
// is shorter when it is sent fails that request with error_kind::file.
std::shared_ptr<body_source> make_file_source(const path& file, std::string& error);

// A multipart/form-data body in the order of `parts`. `content_type` is set to the full header value (the boundary included).
// Fails (null, `error` set) when a file part cannot be read or a name is empty.
std::shared_ptr<body_source> make_multipart_source(const std::vector<form_part>& parts, std::string& content_type, std::string& error);

} // namespace httpp::detail

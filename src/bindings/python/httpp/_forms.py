"""Form bodies: FormData and UploadFile, the Python side of C++ `httpp::form_data` and `httpp::upload_file`.

C++ parses the body (see request::form() in include/httpp/server.hpp); this module only wraps what it returns.
"""

import os


class UploadFile:
    """One uploaded file: `field` (the form field name), `filename` (as sent, without a directory part; "" for an empty
    file input), `content_type`, `data` (bytes) and `size`. The whole file is in memory (see Server's max_body)."""

    __slots__ = ("field", "filename", "content_type", "data")

    def __init__(self, field, filename, content_type, data):
        self.field, self.filename, self.content_type, self.data = field, filename, content_type, data

    @property
    def size(self):
        return len(self.data)

    def text(self, encoding="utf-8", errors="replace"):
        return self.data.decode(encoding, errors)

    def save(self, path):
        """Write the file to `path` (the path is yours: never build it from `filename` without checking it)."""
        with open(os.fspath(path), "wb") as out:
            out.write(self.data)

    def __repr__(self):
        return f"UploadFile(field={self.field!r}, filename={self.filename!r}, content_type={self.content_type!r}, size={self.size})"


class FormData:
    """`fields`: [(name, value), ...] in wire order, repeats kept; `files`: [UploadFile, ...]."""

    __slots__ = ("fields", "files")

    def __init__(self, fields, files):
        self.fields = fields
        self.files = files

    @classmethod
    def _from_tuple(cls, parsed):
        fields, files = parsed
        return cls(fields, [UploadFile(*f) for f in files])

    def field(self, name, default=None):
        """First value of a field, `default` if absent."""
        return next((v for k, v in self.fields if k == name), default)

    def getlist(self, name):
        """Every value of a field."""
        return [v for k, v in self.fields if k == name]

    def file(self, name):
        """First file sent under this field name, None if there is none."""
        return next((f for f in self.files if f.field == name), None)

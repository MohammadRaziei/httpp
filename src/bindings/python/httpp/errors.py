"""Exceptions raised by Response.raise_for_status()."""


class Error(Exception):
    """Base class of every httpp exception."""


class HTTPError(Error):
    """The server answered, with a 4xx or 5xx status. `.response` is the Response."""

    def __init__(self, message, response=None):
        super().__init__(message)
        self.response = response


class RequestError(Error):
    """No HTTP response was received at all. `.response` has status 0 and the reason."""

    def __init__(self, message, response=None):
        super().__init__(message)
        self.response = response


class Timeout(RequestError):
    """The connection or the server took longer than the timeout."""


class TLSError(RequestError):
    """The TLS handshake or the certificate verification failed."""


class TooManyRedirects(RequestError):
    pass


class InvalidURL(RequestError, ValueError):
    pass


_BY_KIND = {
    "timeout": Timeout,
    "tls": TLSError,
    "too_many_redirects": TooManyRedirects,
    "invalid_url": InvalidURL,
}


def request_error(kind, message, response):
    """The exception that matches Response.error `kind`."""
    return _BY_KIND.get(kind, RequestError)(f"{kind}: {message}" if message else kind, response)

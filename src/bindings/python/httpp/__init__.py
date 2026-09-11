"""httpp: a lightweight, zero-system-dependency HTTP client + server."""

__version__ = "0.1.0"

# Import the compiled Cython module
from .httpp_cy import Client, Server

__all__ = ["Client", "Server"]

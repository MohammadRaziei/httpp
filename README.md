# httpp

A Cython project with scikit-build

## Overview

This project provides Python bindings for the [libcurl](https://curl.se/libcurl/) library, enabling HTTP client capabilities in Python with native performance through Cython integration.

**Note:** This project is not just for Python developers! C++ developers can also use the pre-built libcurl library included in this package instead of building libcurl from source.

## Features

- Python bindings for libcurl HTTP client
- Built with Cython for optimal performance
- CMake integration via scikit-build
- Support for HTTP/HTTPS protocols
- Test-driven development approach

## Installation

```bash
pip install httpp
```

For the latest development version:
```bash
pip install git+https://github.com/MohammadRaziei/httpp.git
```

## Quick Start

### Basic HTTP Request

```python
from httpp import curl

# Create a curl instance
c = curl.Curl()

# Perform a GET request
response = c.get("https://httpbin.org/get")
print(response.status_code)
print(response.text)

# Perform a POST request
response = c.post("https://httpbin.org/post", data={"key": "value"})
print(response.json())
```

## Development

Run tests:
```bash
pytest -n auto
```

## License

MIT License
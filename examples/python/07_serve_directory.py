"""Serve static files (like `python -m http.server`) next to your own routes.

    python 07_serve_directory.py
    curl http://127.0.0.1:8000/static/index.html
    curl http://127.0.0.1:8000/api/time
"""
import tempfile
import time
from pathlib import Path

from httpp import Server

with tempfile.TemporaryDirectory() as site:  # demo content; point this at your own folder
    Path(site, "index.html").write_text("<h1>static page</h1>")

    app = Server()
    app.serve_directory("/static", site)

    @app.get("/api/time")
    def now():
        return {"time": time.time()}

    app.listen("127.0.0.1", 8000)

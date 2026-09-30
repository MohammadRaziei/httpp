"""Download a file, with or without a progress bar, and check the result.

Runs offline: it serves a temporary 8 MB file from a local server.
Use any http:// URL in place of `url` to download something real.

    python 11_download.py
"""
import tempfile
import threading
from pathlib import Path

from httpp import Download, Server, download

with tempfile.TemporaryDirectory() as tmp:
    # --- a local file server (not part of the download API) -------------
    Path(tmp, "big.bin").write_bytes(b"x" * 8_000_000)
    app = Server()
    app.serve_directory("/files", tmp)
    port = app.bind_to_any_port("127.0.0.1")
    threading.Thread(target=app.listen_after_bind, daemon=True).start()

    # --- downloading ------------------------------------------------------
    url = f"http://127.0.0.1:{port}/files/big.bin"
    out = Path(tmp, "copy.bin")

    result = download(url, str(out))  # progress bar on by default
    print("ok:", result.ok, "status:", result.status, "bytes:", out.stat().st_size)

    result = Download(url).output(str(out)).disable_progress().run()  # builder form, quiet
    print("quiet ok:", result.ok)

    result = download(f"http://127.0.0.1:{port}/files/nope.bin", str(out), show_progress=False)
    print("missing ->", result.ok, result.status)  # False 404; result.error holds details

    app.stop()

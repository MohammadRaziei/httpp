"""The echo server the Autobahn fuzzing client talks to: python tools/autobahn/echo_server.py [port]."""

import sys

from httpp import Server

app = Server()


# Autobahn sends messages up to 16 MiB; permessage-deflate (cases 12.* and 13.*) is not supported, see ADR-0026.
@app.websocket("/", origins=["*"], max_message=32 << 20, max_buffered=32 << 20, max_queued=32 << 20)
async def echo(websocket):
    await websocket.accept()
    async for message in websocket:
        if isinstance(message, str):
            await websocket.send_text(message)
        else:
            await websocket.send_bytes(message)


if __name__ == "__main__":
    app.run("127.0.0.1", int(sys.argv[1]) if len(sys.argv) > 1 else 9001)

"""The `websockets` library as a server (its asyncio implementation): an echo, the same as suite_asgi.py. python ws_websockets.py PORT

Compression and keep-alive pings are off, like in the other servers of the suite; uvloop is used when it is installed, like uvicorn does."""
import asyncio
import sys

from websockets.asyncio.server import serve


async def echo(websocket):
    async for message in websocket:
        await websocket.send(message)


async def main(port):
    async with serve(echo, "127.0.0.1", port, compression=None, ping_interval=None, max_size=1 << 20):
        await asyncio.get_running_loop().create_future()


if __name__ == "__main__":
    try:
        import uvloop
        uvloop.run(main(int(sys.argv[1])))
    except ImportError:
        asyncio.run(main(int(sys.argv[1])))

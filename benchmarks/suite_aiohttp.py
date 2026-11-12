"""The aiohttp app of the benchmark suite. The same answers as suite_asgi.py, suite_app.py and cpp/cpp_bench.cpp.

    python suite_aiohttp.py PORT

One process, no access log, uvloop when it is installed (the label of the variant says which)."""
import sys

from aiohttp import web


def _work():
    return sum(i * i for i in range(2000))


async def hello(request):
    return web.Response(text="hello")


async def cpu(request):
    return web.Response(text=str(_work()))


async def item(request):
    return web.Response(body=b'{"id":%d,"ok":true}' % int(request.match_info["item_id"]), content_type="application/json")


async def ws_echo(request):  # an echo, like the other servers of the suite
    ws = web.WebSocketResponse()
    await ws.prepare(request)
    async for msg in ws:
        if msg.type == web.WSMsgType.TEXT:
            await ws.send_str(msg.data)
        elif msg.type == web.WSMsgType.BINARY:
            await ws.send_bytes(msg.data)
    return ws


def make_app():
    app = web.Application()
    app.add_routes([web.get("/", hello), web.get("/cpu", cpu), web.get("/item/{item_id}", item), web.get("/ws", ws_echo)])
    return app


if __name__ == "__main__":
    try:
        import uvloop
        uvloop.install()
    except ImportError:
        pass
    web.run_app(make_app(), host="127.0.0.1", port=int(sys.argv[1]), access_log=None, print=None)

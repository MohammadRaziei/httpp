import asyncio

import httpp


async def main():
    result = await httpp.Download("http://127.0.0.1:8000/hello/world", "hello.txt").disable_progress().run_async()
    print(result.status)


asyncio.run(main())

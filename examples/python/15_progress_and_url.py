"""progress bars (drawn on stderr) and the URL helpers.

Runs offline.

    python 15_progress_and_url.py
"""
import time

import httpp
from httpp import URL, progress

# A bar over a loop, like tqdm.trange:
for i in progress.range(20, "working"):
    time.sleep(0.01)

# Or drive one by hand:
bar = progress.bar(100, "manual")
for _ in range(4):
    bar.update(25)  # advance by 25; bar.set_progress(n) sets an absolute position
    time.sleep(0.01)
bar.finish()
print("bar finished:", bar.finished, bar.current, "/", bar.total)

# URL: the parser the client uses, plus percent-encoding helpers.
u = URL.parse("https://example.com:8443/search/files?q=cats&page=2")
print(u.valid, u.scheme, u.host, u.port, u.path, u.query)
print(URL.parse("not a url").valid)  # never raises: check .valid

print(URL.encode_component("a b/é"))  # a%20b%2F%C3%A9
print(URL.decode_component("a%20b+c"))  # 'a b+c'  ("+" stays "+")
print(URL.build_query({"q": "x y", "n": 2}))  # q=x%20y&n=2

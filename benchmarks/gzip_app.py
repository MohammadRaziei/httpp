import json, os, random
import httpp
from httpp.responses import Response
random.seed(7)
SIZE = int(os.environ["GZ_SIZE"])
rows, body = [], b""
i = 0
while len(body) < SIZE:   # realistic API-like JSON: ids, names, enums, floats, timestamps
    rows.append({"id": 100000 + i, "name": random.choice(["alpha", "bravo", "charlie", "delta", "echo"]) + str(random.randint(1, 999)),
                 "status": random.choice(["active", "pending", "closed"]), "score": round(random.random() * 100, 3),
                 "created": "2026-11-%02dT%02d:%02d:00Z" % (random.randint(1, 12), random.randint(0, 23), random.randint(0, 59)),
                 "tags": random.sample(["a", "b", "c", "d", "e", "f"], 3)})
    body = json.dumps(rows, separators=(",", ":")).encode(); i += 1
level = int(os.environ.get("GZ_LEVEL", "0"))
app = httpp.Server(openapi_url=None, compression=(1 if level else None), compression_level=max(level, 1))
@app.get("/j")
def j():
    return Response(body, media_type="application/json")

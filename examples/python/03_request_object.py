"""Ask for `request` by name to see the method, query, headers and body.

    python 03_request_object.py
    curl "http://127.0.0.1:8000/inspect?page=2" -H "X-Token: abc" \
         -H "Content-Type: application/json" -d '{"hello": "world"}'
"""
from httpp import Server

app = Server()


@app.post("/inspect")
def inspect(request):
    try:
        body = request.json()  # also available: request.body (bytes), request.text
    except ValueError:
        return {"error": "body must be JSON"}, 400
    return {
        "method": request.method,
        "path": request.path,
        "query": request.query,  # {"page": "2"}
        "token": request.headers.get("x-token"),  # header names are lower-case
        "json": body,
    }


app.listen("127.0.0.1", 8000)

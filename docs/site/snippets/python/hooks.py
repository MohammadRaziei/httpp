from httpp import Server

app = Server()


@app.before_request
def require_key(request):
    if request.headers.get("x-key") != "secret":
        return {"error": "forbidden"}, 403  # answers here; the route never runs


@app.after_request
def stamp(request, response):
    response.set_header("X-Served-By", "httpp")  # status and headers only


@app.get("/")
def index():
    return "hello"


app.listen("127.0.0.1", 8000)

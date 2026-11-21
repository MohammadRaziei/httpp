import httpp

res = (
    httpp.Request("http://127.0.0.1:8000/inspect?page=2")
    .method("POST")
    .header("X-Token", "abc")
    .content_type("application/json")
    .data('{"a": 1}')
    .run()
)
print(res.status)
print(res.body)

from httpp import Client

client = Client("127.0.0.1", 8000)
res = client.get("/hello/world")
print(res.status, res.body)  # 200 hello world

# One call with a full URL, no host and port to split:
print(Client.fetch("http://127.0.0.1:8000/hello/world").status)

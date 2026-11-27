import httpp

with open("notes.txt", "wb") as f:  # a small file to send
    f.write(b"remember the milk\n")

res = (
    httpp.Request("http://127.0.0.1:8000/upload")
    .field("title", "shopping")
    .file("doc", "notes.txt")
    .run()
)
print(res.status)
print(res.text)

import httpp


def show(sent, total):
    print(f"{sent}/{total}")


with open("big.bin", "wb") as f:  # 200 KB of zeros to send
    f.write(bytes(200_000))

res = httpp.Request("http://127.0.0.1:8000/upload").field("title", "big").file("doc", "big.bin").progress(show).run()
print(res.status)

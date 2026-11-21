import httpp

result = httpp.Download("http://127.0.0.1:8000/hello/world", "hello.txt").run()
print(result.status)  # 200; hello.txt now holds "hello world"

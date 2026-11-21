import httpp

result = (
    httpp.Download("http://127.0.0.1:8000/hello/world", "hello.txt")
    .disable_progress()
    .sha256("b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9")
    .run()
)
if not result.ok:
    raise SystemExit(f"download failed: {result.error}")
print(result.sha256)  # the digest the file was checked against

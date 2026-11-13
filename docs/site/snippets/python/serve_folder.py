import httpp

app = httpp.Server()
app.serve_directory("/static", "public")  # /static/notes.txt is public/notes.txt

if __name__ == "__main__":
    app.listen("127.0.0.1", 8000)

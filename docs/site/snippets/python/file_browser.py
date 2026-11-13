import httpp

app = httpp.Server()
app.mount_file_browser("/files", "shared", title="Team files")

if __name__ == "__main__":
    app.listen("127.0.0.1", 8000)

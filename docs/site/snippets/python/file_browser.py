import httpp

app = httpp.Server()
httpp.FileBrowser("shared").set_title("Team files").mount(app, "/files")

if __name__ == "__main__":
    app.listen("127.0.0.1", 8000)

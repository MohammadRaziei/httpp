import httpp

app = (httpp.Directory("public").show_hidden(False)
       .mount(httpp.Server(), "/")
       .tls("cert.pem", "key.pem"))

if __name__ == "__main__":
    app.listen("127.0.0.1", 8000)

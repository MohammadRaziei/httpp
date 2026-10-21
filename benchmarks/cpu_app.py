import httpp
app = httpp.Server()

def burn(n):
    s = 0
    for i in range(n):
        s += i * i % 7
    return s

@app.get("/cpu")
def cpu():
    return {"v": burn(30000)}

@app.get("/async/cpu")
async def cpu_async():
    return {"v": burn(30000)}

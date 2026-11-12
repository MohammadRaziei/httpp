"""Cost and gain of server gzip (ADR-0061): requests per second and bytes on the wire for a JSON answer of 1, 10 and 100 KB
with compression off and at levels 1, 6 and 9. Server pinned to CPU 0, oha to CPU 1, one I/O thread, loopback (so only the cost shows).

    python benchmarks/gzip_bench.py --python path/to/python --oha path/to/oha
"""
import argparse, subprocess, os, time, json, statistics, socket, urllib.request, shutil
ap = argparse.ArgumentParser()
ap.add_argument("--python", default=shutil.which("python3")); ap.add_argument("--oha", default=shutil.which("oha"))
args = ap.parse_args(); PY, OHA = args.python, args.oha
D = os.path.dirname(os.path.abspath(__file__))
def port():
    s=socket.socket(); s.bind(("127.0.0.1",0)); p=s.getsockname()[1]; s.close(); return p
def run(size,level):
    p=port(); env=dict(os.environ,GZ_SIZE=str(size),GZ_LEVEL=str(level),PYTHONPATH=D)
    srv=subprocess.Popen(["taskset","-c","0",PY,"-m","httpp","py","run","gzip_app:app","--port",str(p),"--log-level","error","--io-threads","1"],cwd=D,env=env,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    try:
        for _ in range(100):
            try: socket.create_connection(("127.0.0.1",p),0.2).close(); break
            except OSError: time.sleep(0.1)
        url=f"http://127.0.0.1:{p}/j"
        rq=urllib.request.Request(url,headers={"Accept-Encoding":"gzip"}); raw=urllib.request.urlopen(rq).read()
        wire=len(raw)
        cmd=["taskset","-c","1",OHA,"-H","Accept-Encoding: gzip","-c","32","--no-tui","--output-format","json"]
        subprocess.run(cmd+["-z","1s",url],capture_output=True)
        j=json.loads(subprocess.run(cmd+["-z","3s",url],capture_output=True,check=True).stdout)
        return j["summary"]["requestsPerSec"], wire
    finally:
        srv.terminate(); srv.wait()
sizes=[1024,10*1024,100*1024]; levels=[0,1,6,9]
res={(s,l):[] for s in sizes for l in levels}
for r in range(3):
    for s in sizes:
        for l in levels: res[(s,l)].append(run(s,l))
print("size    level   req/s (median)   wire bytes   ratio   cost vs off")
for s in sizes:
    base=statistics.median(x[0] for x in res[(s,0)]); wire0=res[(s,0)][0][1]
    for l in levels:
        v=statistics.median(x[0] for x in res[(s,l)]); w=res[(s,l)][0][1]
        print(f"{s//1024:>4} KB  {'off' if l==0 else l:>5}   {v:10.0f}      {w:9d}   {w/wire0:5.2f}   {base/v:5.2f}x")

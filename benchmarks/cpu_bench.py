import json, os, socket, statistics, subprocess, sys, time
oha, S = sys.argv[1], os.path.dirname(os.path.abspath(__file__))
def port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0)); return s.getsockname()[1]
p = port()
proc = subprocess.Popen([sys.executable, "-m", "httpp", "run", "cpu_app:app", "--port", str(p), "--log-level", "error"],
                        cwd=S, env=dict(os.environ, PYTHONPATH=S), stderr=subprocess.PIPE)
for _ in range(100):
    try: socket.create_connection(("127.0.0.1", p), .1).close(); break
    except OSError: time.sleep(.1)
def run(path, conc, secs=6):
    o = subprocess.run([oha, "-z", f"{secs}s", "-c", str(conc), "--no-tui", "--output-format", "json", f"http://127.0.0.1:{p}{path}"], capture_output=True, check=True)
    d = json.loads(o.stdout); dist = d["statusCodeDistribution"]
    assert dist.get("200", 0) == sum(dist.values()), dist
    return d["summary"]["requestsPerSec"]
gil = getattr(sys, "_is_gil_enabled", lambda: True)()
print(f"python {sys.version.split()[0]} gil={gil} cpus={os.cpu_count()}")
for path in ("/cpu", "/async/cpu"):
    for conc in (1, 8):
        run(path, conc, 1)
        r = [run(path, conc) for _ in range(3)]
        print(f"{path:<11} conc={conc:<2} median {statistics.median(r):8.1f} req/s [{min(r):.1f}-{max(r):.1f}]", flush=True)
proc.terminate()

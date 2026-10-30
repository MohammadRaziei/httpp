# Autobahn conformance run

1. `pip install httpp` (or the wheel built from this tree), then `python tools/autobahn/echo_server.py`
2. In `tools/autobahn`: `docker run --rm --net=host -v "$PWD:/config" -v "$PWD/reports:/reports" crossbario/autobahn-testsuite wstest -m fuzzingclient -s /config/fuzzingclient.json`
3. Open `reports/index.html`, or run `python check_report.py reports/index.json out.md` for a Markdown summary (the CI workflow publishes it as `autobahn-report.md` on the `__beta__` release). Cases 12.* and 13.* (permessage-deflate) are excluded: not supported.

#!/usr/bin/env python3
"""Helper for rest_var_churn.test: send many concurrent GET /customers requests
while another connection keeps changing vsql_rest.schema, and report every
answer that is not one of the two schemas' correct answers."""
import json, os, sys, time, urllib.error, urllib.request
from concurrent.futures import ThreadPoolExecutor

PORT = os.environ["REST_PORT"]
URL = f"http://127.0.0.1:{PORT}/customers?select=name&order=id.asc"
REQUESTS = 1000
WORKERS = 8

EXPECTED = (
    [{"name": "Alice"}, {"name": "Bob"}, {"name": "Carol"}],  # test_rest
    [{"name": "Zed"}],                                        # test_rest2
)


def fetch(url):
    try:
        with urllib.request.urlopen(url, timeout=10) as r:
            return r.status, r.read().decode()
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode()
    except Exception as e:  # noqa: BLE001 -- any failure is a wrong answer
        return None, repr(e)


if sys.argv[1:] == ["settle"]:
    # orders exists only in test_rest, so a 200 means the schema cache was
    # rebuilt for test_rest.
    url = f"http://127.0.0.1:{PORT}/orders?limit=1"
    deadline = time.time() + 30
    while fetch(url)[0] != 200 and time.time() < deadline:
        time.sleep(0.1)
    print(f"cache describes test_rest: {fetch(url)[0] == 200}")
    sys.exit(0)

with ThreadPoolExecutor(WORKERS) as pool:
    results = list(pool.map(fetch, [URL] * REQUESTS))

wrong = []
served = set()
for status, body in results:
    try:
        rows = json.loads(body) if status == 200 else None
    except ValueError:
        rows = None
    if rows in EXPECTED:
        served.add(EXPECTED.index(rows))
    else:
        wrong.append((status, body[:200]))

print(f"requests: {len(results)}")
# Both schemas answered, so the schema really changed while requests ran.
print(f"schemas served: {len(served)}")
print(f"wrong answers: {len(wrong)}")
for status, body in wrong[:5]:
    print(f"  {status} {body}")
sys.exit(0)

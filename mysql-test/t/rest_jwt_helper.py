#!/usr/bin/env python3
"""Helper for rest_auth.test: generate HS256 JWT tokens and make curl requests."""
import base64, hmac, hashlib, json, os, time, subprocess, sys

SECRET = "test-secret-key"
PORT = os.environ.get("REST_PORT", "18080")
BASE = f"http://localhost:{PORT}"

def b64url(d):
    if isinstance(d, str): d = d.encode()
    return base64.urlsafe_b64encode(d).rstrip(b'=').decode()

def make_token(payload):
    h = b64url('{"alg":"HS256","typ":"JWT"}')
    p = b64url(json.dumps(payload))
    sig = hmac.new(SECRET.encode(), f"{h}.{p}".encode(), hashlib.sha256).digest()
    return f"{h}.{p}.{b64url(sig)}"

def curl_code(path, token=None):
    cmd = ["curl", "-s", "-o", "/dev/null", "-w", "%{http_code}"]
    if token:
        cmd += ["-H", f"Authorization: Bearer {token}"]
    cmd.append(BASE + path)
    return subprocess.run(cmd, capture_output=True, text=True).stdout.strip()

def curl_code_msg(path, token=None):
    """Return "<status> <message>" so a test asserts the cause, not just the code."""
    cmd = ["curl", "-s", "-w", "\n%{http_code}"]
    if token:
        cmd += ["-H", f"Authorization: Bearer {token}"]
    cmd.append(BASE + path)
    out = subprocess.run(cmd, capture_output=True, text=True).stdout.rsplit("\n", 1)
    body, code = (out[0], out[-1]) if len(out) == 2 else ("", out[-1])
    try:
        return f"{code} {json.loads(body)['message']}"
    except Exception:
        return f"{code} <unparseable: {body[:60]}>"

def curl_json(path, token=None):
    cmd = ["curl", "-s"]
    if token:
        cmd += ["-H", f"Authorization: Bearer {token}"]
    cmd.append(BASE + path)
    r = subprocess.run(cmd, capture_output=True, text=True)
    try:
        return json.loads(r.stdout)
    except Exception:
        return r.stdout

action = sys.argv[1] if len(sys.argv) > 1 else ""

if action == "valid_200":
    tok = make_token({"sub": "alice@example.com", "exp": int(time.time()) + 3600})
    print(curl_code("/customers", token=tok))

elif action == "expired_401":
    tok = make_token({"sub": "alice@example.com", "exp": int(time.time()) - 3600})
    print(curl_code_msg("/customers", token=tok))

elif action == "noexp_401":
    # Token with no exp claim must be rejected (fail closed), not accepted forever
    # — and must say so, rather than claiming the token expired.
    tok = make_token({"sub": "alice@example.com"})
    print(curl_code_msg("/customers", token=tok))

elif action == "lowercase_bearer_200":
    tok = make_token({"sub": "alice@example.com", "exp": int(time.time()) + 3600})
    cmd = ["curl", "-s", "-o", "/dev/null", "-w", "%{http_code}",
           "-H", f"authorization: bearer {tok}", BASE + "/customers"]
    print(subprocess.run(cmd, capture_output=True, text=True).stdout.strip())

elif action == "hostile_claim_name":
    # A claim name is interpolated as a SQL identifier, where value escaping
    # does nothing. This one would extend the SET into a second assignment.
    tok = make_token({"sub": "alice@example.com",
                      "exp": int(time.time()) + 3600,
                      "a = 1, GLOBAL sql_mode": "ANSI_QUOTES"})
    print(curl_code("/customers", token=tok))

elif action == "no_claim_leak":
    # One SQL session serves every request drained in a single wakeup, so an
    # unauthenticated request can land on a session a previous caller's claims
    # were set on. It must never observe them. Fired concurrently to make that
    # sharing likely; the assertion holds regardless of how they batch.
    import threading
    tok = make_token({"sub": "alice@example.com", "exp": int(time.time()) + 3600})

    def rpc(token=None):
        cmd = ["curl", "-s", "-X", "POST",
               "-H", "Content-Type: application/json", "-d", "{}"]
        if token:
            cmd += ["-H", f"Authorization: Bearer {token}"]
        cmd.append(BASE + "/rpc/whoami")
        return subprocess.run(cmd, capture_output=True, text=True).stdout.strip()

    seen = []
    def authed():
        rpc(token=tok)
    def anon():
        seen.append(rpc())
    for _ in range(10):
        ta, tb = threading.Thread(target=authed), threading.Thread(target=anon)
        ta.start(); tb.start(); ta.join(); tb.join()
    leaked = [s for s in seen if "alice" in str(s)]
    print("leaked %d/%d" % (len(leaked), len(seen)))

elif action == "view_filter":
    tok = make_token({"sub": "alice@example.com", "exp": int(time.time()) + 3600})
    d = curl_json("/owned", token=tok)
    if isinstance(d, list) and len(d) == 1:
        print(len(d), d[0]["name"])
    else:
        print("FAIL:", str(d)[:120])

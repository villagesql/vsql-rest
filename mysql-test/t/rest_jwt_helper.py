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
    print(curl_code("/customers", token=tok))

elif action == "view_filter":
    tok = make_token({"sub": "alice@example.com", "exp": int(time.time()) + 3600})
    d = curl_json("/owned", token=tok)
    if isinstance(d, list) and len(d) == 1:
        print(len(d), d[0]["name"])
    else:
        print("FAIL:", str(d)[:120])

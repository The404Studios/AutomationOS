#!/usr/bin/env python3
"""oauth_mock.py -- a host-side MOCK OAuth 2.0 Device Flow (RFC 8628) server.

It lets AutomationOS prove the WHOLE "Sign in with Google" device-flow path
end-to-end at ZERO cost -- no Google project, no API key, no phone. It speaks
the same protocol + field names Google uses, so the OS-side `gsignin` code is
IDENTICAL for mock vs. live: only the endpoint host changes.

    AutomationOS gsignin  --TCP 10.0.2.2:8434-->  this mock
       (1) POST /device/code        -> {device_code,user_code,verification_url,interval,...}
       (2) POST /token  (poll loop) -> authorization_pending ... then {access_token,...}
       (3) GET  /oauth2/v3/userinfo -> {sub,email,name}   (Bearer <access_token>)

Going LIVE (real Google account) = point gsignin at Google instead of here:
    device/code,token -> https://oauth2.googleapis.com
    userinfo          -> https://www.googleapis.com/oauth2/v3/userinfo
with a real "TV and Limited Input device" OAuth client_id+secret. No code change.

Approval model (for a hands-free zero-cost proof):
    - default: the device is AUTO-APPROVED after AUTO_APPROVE_POLLS token polls,
      so the OS completes the full flow with no human step.
    - --manual: approval requires visiting  http://127.0.0.1:8434/approve?code=USERCODE
      on the host (closer to the real "approve on your phone" step).

Run:  python3 scripts/oauth_mock.py            # auto-approve (CI / hands-free)
      python3 scripts/oauth_mock.py --manual   # require /approve
Listens on 127.0.0.1:8434 (QEMU slirp forwards guest 10.0.2.2:8434 -> here).
"""
import sys, json, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

HOST, PORT = "127.0.0.1", 8434
AUTO_APPROVE_POLLS = 2          # auto mode: approve after this many /token polls
MANUAL = "--manual" in sys.argv

# device_code -> {user_code, polls, approved, created}
SESSIONS = {}
USER_CODE = "WDJB-MJHT"          # fixed so the proof is deterministic
DEV_EMAIL = "dev@automationos.local"
DEV_NAME = "AutomationOS Dev"


def _now():
    return int(time.time())


class H(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        sys.stderr.write("[oauth-mock] " + (fmt % args) + "\n")

    def _json(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _read_form(self):
        n = int(self.headers.get("Content-Length", "0") or "0")
        raw = self.rfile.read(n).decode("utf-8", "replace") if n else ""
        # accept application/x-www-form-urlencoded
        return {k: v[0] for k, v in parse_qs(raw).items()}

    def do_POST(self):
        path = urlparse(self.path).path
        if path == "/device/code":
            form = self._read_form()
            dc = "mock_device_code_" + str(_now())
            SESSIONS[dc] = {"user_code": USER_CODE, "polls": 0,
                            "approved": False, "created": _now()}
            sys.stderr.write("[oauth-mock] device/code client_id=%s scope=%s -> user_code=%s\n"
                             % (form.get("client_id", "?"), form.get("scope", "?"), USER_CODE))
            self._json(200, {
                "device_code": dc,
                "user_code": USER_CODE,
                "verification_url": "http://127.0.0.1:%d/approve?code=%s" % (PORT, USER_CODE),
                "verification_uri": "http://127.0.0.1:%d/approve" % PORT,
                "expires_in": 1800,
                "interval": 1,   # poll fast so the proof is quick
            })
            return

        if path == "/token":
            form = self._read_form()
            dc = form.get("device_code", "")
            s = SESSIONS.get(dc)
            if not s:
                self._json(400, {"error": "invalid_grant"})
                return
            s["polls"] += 1
            if not MANUAL and s["polls"] >= AUTO_APPROVE_POLLS:
                s["approved"] = True
            if not s["approved"]:
                # RFC 8628: HTTP 400 + authorization_pending while waiting.
                self._json(400, {"error": "authorization_pending"})
                return
            self._json(200, {
                "access_token": "mock_access_" + dc,
                "token_type": "Bearer",
                "refresh_token": "mock_refresh_" + dc,
                "id_token": "mock_id_token",
                "expires_in": 3600,
                "scope": "openid email profile",
            })
            return

        self._json(404, {"error": "not_found"})

    def do_GET(self):
        u = urlparse(self.path)
        path = u.path
        if path == "/approve":
            # Manual approval: mark every session whose user_code matches approved.
            q = parse_qs(u.query)
            code = (q.get("code", [""])[0]).strip().upper()
            hit = 0
            for s in SESSIONS.values():
                if s["user_code"].upper() == code:
                    s["approved"] = True
                    hit += 1
            msg = ("Approved %d session(s) for code %s. Return to AutomationOS."
                   % (hit, code)) if hit else ("No pending session for code %s." % code)
            body = ("<html><body><h2>AutomationOS mock OAuth</h2><p>%s</p></body></html>" % msg).encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        if path == "/oauth2/v3/userinfo" or path == "/userinfo":
            auth = self.headers.get("Authorization", "")
            if not auth.startswith("Bearer mock_access_"):
                self._json(401, {"error": "invalid_token"})
                return
            self._json(200, {
                "sub": "1234567890",
                "email": DEV_EMAIL,
                "email_verified": True,
                "name": DEV_NAME,
                "given_name": "AutomationOS",
                "family_name": "Dev",
                "picture": "",
            })
            return

        self._json(404, {"error": "not_found"})


def main():
    mode = "MANUAL (visit /approve)" if MANUAL else ("AUTO-approve after %d polls" % AUTO_APPROVE_POLLS)
    sys.stderr.write("[oauth-mock] RFC 8628 device-flow mock on http://%s:%d  (%s)\n" % (HOST, PORT, mode))
    sys.stderr.write("[oauth-mock] endpoints: POST /device/code  POST /token  GET /oauth2/v3/userinfo\n")
    ThreadingHTTPServer((HOST, PORT), H).serve_forever()


if __name__ == "__main__":
    main()

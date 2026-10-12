#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Local HTTP + HTTPS test server for the curl transport tests, then run the test binary.

    http_server.py TEST_BINARY [ARG...]
    http_server.py --serve HOST:PORT [DIR]   plain HTTP/WebSocket only, until interrupted (device
                                             tests); DIR is served at /files/<name>

Generates a throwaway CA and a server certificate for "localhost" with the openssl CLI, starts an
HTTP server and an HTTPS server on 127.0.0.1 (ephemeral ports), and runs TEST_BINARY with:
    WNK_TEST_HTTP_BASE   http://127.0.0.1:<port>
    WNK_TEST_HTTPS_BASE  https://localhost:<port>
    WNK_TEST_HTTPS_IP    https://127.0.0.1:<port>   (same server, name not in the certificate)
    WNK_TEST_CA          path of the CA certificate (PEM)
    WNK_TEST_FILES_DIR   a directory served at /files/<name> (byte ranges supported)
In each ARG, @HTTP@ is replaced by the http base (for apps that take the server from launch params).
Exits with the test binary's exit code. Stdlib + openssl CLI only; nothing leaves the machine.

Byte-range routes (Range: bytes=FIRST- or FIRST-LAST, answered with 206/416), for streaming tests:
    /range?n=N        N bytes, byte i = i % 251; &noranges=1 ignores Range (200, whole body);
                      &drop=K closes every connection after K body bytes
    /files/NAME       a file from the served directory

Catalogue routes, a synthetic stand-in for the TVmaze API used by Example C (applications/examples/shows):
    /catalogue[/MODE]/shows?page=N   page 0: 250 shows, page 1: 50, later pages 404 (like TVmaze)
    /catalogue[/MODE]/shows/ID       one show with _embedded seasons and cast
    MODE fail: every request answers 503; flaky: the first 3 requests of the server's life answer
    503 (one refresh with its retries), then normal; slow: show details arrive after 4 s; broken:
    page 0 is not JSON

WebSocket routes (GET with Upgrade: websocket), for tests/integration/test_websocket_curl.cpp:
    /ws/echo     echoes messages; text commands: ping-me (server ping), close-me (server close 1000
                 "bye"), fragment-me (3-fragment text), big (200000-byte binary), bad-frame (masked
                 server frame), header (echoes X-Test); answers pings; offers "chat" if asked
    /ws/reject   403 instead of 101
    /ws/drop     handshake, then the TCP connection is closed (counts connections in /ws/count)
    /ws/silent   handshake, then reads and ignores everything (no pongs)
"""
from __future__ import annotations

import gzip
import http.server
import json
import os
import shutil
import ssl
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
import base64
import hashlib
import struct

WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
WS_DROPS = {"n": 0}
FILES_DIR = {"path": None}
RANGE_LOG: list[str] = []
CATALOGUE_FLAKY = {"left": 3}  # Range header of every /range and /files request, for /range-log


def parse_range(value: str, size: int) -> tuple[int, int] | None | str:
    """(first, last) for "bytes=FIRST-[LAST]", None without a usable header, "416" if unsatisfiable."""
    if not value.startswith("bytes=") or "," in value:
        return None
    first, _, last = value[6:].strip().partition("-")
    if not first.isdigit() or (last and not last.isdigit()):
        return None
    a = int(first)
    b = min(int(last), size - 1) if last else size - 1
    if a >= size or b < a:
        return "416"
    return a, b


CATALOGUE_WORDS = ["Northern", "Harbour", "Signal", "Quiet", "Summit", "Echo", "Atlas", "Ember", "Tide",
                   "Orbit", "Meadow", "Glass", "River", "Lantern"]


def catalogue_show(i: int) -> dict:
    """Synthetic show i (1..300): deterministic, so tests can name what they expect."""
    return {
        "id": i,
        "name": f"{CATALOGUE_WORDS[i % 14]} {CATALOGUE_WORDS[(i * 5 + 3) % 14]} {i}",
        "premiered": f"{1990 + i % 35}-0{1 + i % 9}-15",
        "ended": None if i % 3 else f"{2000 + i % 25}-06-01",
        "status": "Running" if i % 3 else "Ended",
        "language": "English",
        "genres": ["Drama", "Comedy", "Mystery"][: 1 + i % 3],
        "runtime": 30 + (i % 4) * 15,
        "rating": {"average": None if i % 7 == 0 else round(5 + (i * 37 % 50) / 10, 1)},
        "weight": i * 13 % 101,
        "updated": 1700000000 + (i * 97 % 5000) * 60,
        "network": {"name": "Test Network"} if i % 2 else None,
        "webChannel": None if i % 2 else {"name": "Test Stream"},
        "image": None,
        "summary": f"<p>Synthetic show <b>{i}</b> &amp; its &quot;friends&quot;.</p><p>Second paragraph.</p>",
    }


def ws_send(w, op: int, payload: bytes, fin: bool = True, mask: bool = False):
    head = bytes([(0x80 if fin else 0) | op])
    n = len(payload)
    m = 0x80 if mask else 0
    if n < 126:
        head += bytes([m | n])
    elif n <= 0xFFFF:
        head += bytes([m | 126]) + struct.pack(">H", n)
    else:
        head += bytes([m | 127]) + struct.pack(">Q", n)
    if mask:
        key = b"\x01\x02\x03\x04"
        payload = bytes(b ^ key[i % 4] for i, b in enumerate(payload))
        head += key
    w.write(head + payload)
    w.flush()


def ws_read(r):
    h = r.read(2)
    if len(h) < 2:
        return None
    op = h[0] & 0x0F
    n = h[1] & 0x7F
    if n == 126:
        n = struct.unpack(">H", r.read(2))[0]
    elif n == 127:
        n = struct.unpack(">Q", r.read(8))[0]
    key = r.read(4) if h[1] & 0x80 else b"\0\0\0\0"
    data = r.read(n)
    return op, bytes(b ^ key[i % 4] for i, b in enumerate(data))


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):  # keep test output quiet
        pass

    def _send(self, code: int, body: bytes, headers: dict | None = None):
        self.send_response(code)
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def _ranged(self, size: int, read, ranges: bool = True, drop: int | None = None):
        """Answers GET/HEAD for a resource of `size` bytes; read(first, n) returns bytes."""
        RANGE_LOG.append(self.headers.get("Range", ""))
        r = parse_range(self.headers.get("Range", ""), size) if ranges else None
        if r == "416":
            return self._send(416, b"", {"Content-Range": f"bytes */{size}"})
        first, last = r if r else (0, size - 1)
        self.send_response(206 if r else 200)
        self.send_header("Accept-Ranges", "bytes" if ranges else "none")
        if r:
            self.send_header("Content-Range", f"bytes {first}-{last}/{size}")
        self.send_header("Content-Length", str(last - first + 1))
        self.send_header("Content-Type", "application/octet-stream")
        self.end_headers()
        if self.command == "HEAD":
            return
        sent, at = 0, first
        try:
            while at <= last:
                n = min(65536, last - at + 1)
                if drop is not None:
                    n = min(n, drop - sent)
                    if n <= 0:
                        self.close_connection = True
                        self.connection.shutdown(2)
                        return
                self.wfile.write(read(at, n))
                at += n
                sent += n
        except (BrokenPipeError, ConnectionResetError, OSError):
            self.close_connection = True

    def _websocket(self, path: str):
        if path == "/ws/reject":
            return self._send(403, b"no")
        key = self.headers.get("Sec-WebSocket-Key", "")
        accept = base64.b64encode(hashlib.sha1((key + WS_GUID).encode()).digest()).decode()
        self.send_response(101, "Switching Protocols")
        self.send_header("Upgrade", "websocket")
        self.send_header("Connection", "Upgrade")
        self.send_header("Sec-WebSocket-Accept", accept)
        if "chat" in self.headers.get("Sec-WebSocket-Protocol", ""):
            self.send_header("Sec-WebSocket-Protocol", "chat")
        self.end_headers()
        self.wfile.flush()
        self.close_connection = True
        if path == "/ws/drop":
            WS_DROPS["n"] += 1
            return
        while True:
            f = ws_read(self.rfile)
            if f is None:
                return
            op, data = f
            if path == "/ws/silent":
                continue
            if op == 0x8:
                ws_send(self.wfile, 0x8, data[:2] if len(data) >= 2 else b"")
                return
            if op == 0x9:
                ws_send(self.wfile, 0xA, data)
                continue
            if op == 0x1 and data == b"ping-me":
                ws_send(self.wfile, 0x9, b"srv")
                continue
            if op == 0x1 and data == b"close-me":
                ws_send(self.wfile, 0x8, struct.pack(">H", 1000) + b"bye")
                f = ws_read(self.rfile)  # the client's answering close
                return
            if op == 0x1 and data == b"fragment-me":
                ws_send(self.wfile, 0x1, b"frag", fin=False)
                ws_send(self.wfile, 0x0, b"ment", fin=False)
                ws_send(self.wfile, 0x0, b"ed", fin=True)
                continue
            if op == 0x1 and data == b"big":
                ws_send(self.wfile, 0x2, bytes(range(256)) * 781 + b"x" * 64)
                continue
            if op == 0x1 and data == b"bad-frame":
                ws_send(self.wfile, 0x1, b"masked", mask=True)
                continue
            if op == 0x1 and data == b"header":
                ws_send(self.wfile, 0x1, self.headers.get("X-Test", "").encode())
                continue
            if op == 0x1 and data == b"count":
                ws_send(self.wfile, 0x1, str(WS_DROPS["n"]).encode())
                continue
            ws_send(self.wfile, op, data)

    def _catalogue(self, path: str, q: dict):
        mode, _, rest = path.partition("/")
        if mode not in ("fail", "flaky", "slow", "broken"):
            mode, rest = "", path
        if mode == "flaky" and CATALOGUE_FLAKY["left"] > 0:
            CATALOGUE_FLAKY["left"] -= 1
            return self._send(503, b"unavailable")
        if mode == "fail":
            return self._send(503, b"unavailable")
        js = {"Content-Type": "application/json"}
        if rest == "shows":
            page = int(q.get("page", ["0"])[0])
            if mode == "broken" and page == 0:
                return self._send(200, b"<html>maintenance</html>", {"Content-Type": "text/html"})
            if page > 1:
                return self._send(404, b'{"name":"Not Found","status":404}', js)
            ids = range(1, 251) if page == 0 else range(251, 301)
            return self._send(200, json.dumps([catalogue_show(i) for i in ids]).encode(), js)
        if rest.startswith("shows/") and rest[6:].isdigit():
            i = int(rest[6:])
            if not 1 <= i <= 300:
                return self._send(404, b'{"name":"Not Found","status":404}', js)
            if mode == "slow":
                time.sleep(4)
            show = catalogue_show(i)
            show["_embedded"] = {
                "seasons": [{"number": n, "episodeOrder": 8 + n, "premiereDate": f"{2000 + n}-09-01"}
                            for n in range(1, 2 + i % 5)],
                "cast": [{"person": {"name": f"Actor {i}-{k}"}, "character": {"name": f"Role {k}"}} for k in range(3)],
            }
            return self._send(200, json.dumps(show).encode(), js)
        return self._send(404, b"not found")

    def _route(self):
        url = urllib.parse.urlparse(self.path)
        if self.headers.get("Upgrade", "").lower() == "websocket" and url.path.startswith("/ws/"):
            return self._websocket(url.path)
        q = urllib.parse.parse_qs(url.query)
        length = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(length) if length else b""
        if url.path == "/range":
            n = int(q.get("n", ["100000"])[0])
            drop = int(q["drop"][0]) if "drop" in q else None
            return self._ranged(n, lambda a, k: bytes((i % 251) for i in range(a, a + k)),
                                ranges="noranges" not in q, drop=drop)
        if url.path == "/range-log":
            return self._send(200, "\n".join(RANGE_LOG).encode())
        if url.path.startswith("/files/") and FILES_DIR["path"]:
            name = urllib.parse.unquote(url.path[len("/files/"):])
            path = os.path.join(FILES_DIR["path"], name)
            if "/" in name or name.startswith(".") or not os.path.isfile(path):
                return self._send(404, b"not found")
            with open(path, "rb") as f:
                def read(a, k, f=f):
                    f.seek(a)
                    return f.read(k)
                return self._ranged(os.path.getsize(path), read)
        if url.path.startswith("/catalogue/"):
            return self._catalogue(url.path[len("/catalogue/"):], q)
        if url.path == "/ok":
            return self._send(200, b"hello", {"Content-Type": "text/plain", "X-Server": "wnk-test"})
        if url.path.startswith("/status/"):
            return self._send(int(url.path.rsplit("/", 1)[1]), b"status")
        if url.path == "/big":
            return self._send(200, b"x" * int(q.get("n", ["100000"])[0]))
        if url.path == "/slow":
            # Headers immediately, then the body trickles in over `ms` milliseconds.
            ms = int(q.get("ms", ["1000"])[0])
            self.send_response(200)
            self.send_header("Content-Length", "10")
            self.end_headers()
            for _ in range(10):
                time.sleep(ms / 10000.0)
                try:
                    self.wfile.write(b".")
                    self.wfile.flush()
                except (BrokenPipeError, ConnectionResetError):
                    return
            return
        if url.path == "/redirect":
            self.send_response(302)
            self.send_header("Location", "/ok")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if url.path == "/echo":
            info = {"method": self.command, "body": body.decode("utf-8", "replace"),
                    "x_test": self.headers.get("X-Test", ""), "expect": self.headers.get("Expect", "")}
            return self._send(200, json.dumps(info).encode(), {"Content-Type": "application/json"})
        if url.path == "/gzip":
            return self._send(200, gzip.compress(b"compressed body " * 100),
                              {"Content-Encoding": "gzip", "Content-Type": "text/plain"})
        return self._send(404, b"not found")

    do_GET = do_POST = do_PUT = do_DELETE = do_HEAD = do_PATCH = _route


class QuietServer(http.server.ThreadingHTTPServer):
    daemon_threads = True

    def handle_error(self, request, client_address):
        # Connections the client rejects on purpose (TLS failures, cancellations) end in resets.
        pass


def make_certs(d: str) -> tuple[str, str, str]:
    def run(*args):
        subprocess.run(["openssl", *args], check=True, cwd=d, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    run("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "2", "-subj", "/CN=wnk test CA",
        "-keyout", "ca.key", "-out", "ca.pem", "-addext", "basicConstraints=critical,CA:TRUE",
        "-addext", "keyUsage=critical,keyCertSign,cRLSign")
    run("req", "-newkey", "rsa:2048", "-nodes", "-subj", "/CN=localhost", "-keyout", "server.key", "-out", "server.csr")
    with open(os.path.join(d, "ext.cnf"), "w") as f:
        f.write("subjectAltName=DNS:localhost\nextendedKeyUsage=serverAuth\n")
    run("x509", "-req", "-in", "server.csr", "-CA", "ca.pem", "-CAkey", "ca.key", "-CAcreateserial",
        "-days", "2", "-out", "server.pem", "-extfile", "ext.cnf")
    return (os.path.join(d, "ca.pem"), os.path.join(d, "server.pem"), os.path.join(d, "server.key"))


def serve(server: http.server.ThreadingHTTPServer):
    threading.Thread(target=server.serve_forever, daemon=True).start()


def main() -> int:
    if len(sys.argv) in (3, 4) and sys.argv[1] == "--serve":
        # Manual/device testing: plain HTTP + WebSocket on HOST:PORT until interrupted.
        host, port = sys.argv[2].rsplit(":", 1)
        if len(sys.argv) == 4:
            FILES_DIR["path"] = os.path.abspath(sys.argv[3])
        srv = QuietServer((host, int(port)), Handler)
        print(f"serving http/ws on {host}:{port}", flush=True)
        srv.serve_forever()
        return 0
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    if shutil.which("openssl") is None:
        print("SKIP: openssl CLI not found", file=sys.stderr)
        return 0
    d = tempfile.mkdtemp(prefix="wnk-https-")
    try:
        ca, cert, key = make_certs(d)
        FILES_DIR["path"] = os.path.join(d, "files")
        os.mkdir(FILES_DIR["path"])
        http_srv = QuietServer(("127.0.0.1", 0), Handler)
        https_srv = QuietServer(("127.0.0.1", 0), Handler)
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(cert, key)
        https_srv.socket = ctx.wrap_socket(https_srv.socket, server_side=True)
        for s in (http_srv, https_srv):
            serve(s)
        env = dict(os.environ,
                   WNK_TEST_HTTP_BASE=f"http://127.0.0.1:{http_srv.server_address[1]}",
                   WNK_TEST_HTTPS_BASE=f"https://localhost:{https_srv.server_address[1]}",
                   WNK_TEST_HTTPS_IP=f"https://127.0.0.1:{https_srv.server_address[1]}",
                   WNK_TEST_CA=ca,
                   WNK_TEST_FILES_DIR=FILES_DIR["path"])
        args = [a.replace("@HTTP@", env["WNK_TEST_HTTP_BASE"]) for a in sys.argv[2:]]
        return subprocess.run([sys.argv[1], *args], env=env).returncode
    finally:
        shutil.rmtree(d, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())

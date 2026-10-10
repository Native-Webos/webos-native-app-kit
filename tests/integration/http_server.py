#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Local HTTP + HTTPS test server for the curl transport tests, then run the test binary.

    http_server.py TEST_BINARY

Generates a throwaway CA and a server certificate for "localhost" with the openssl CLI, starts an
HTTP server and an HTTPS server on 127.0.0.1 (ephemeral ports), and runs TEST_BINARY with:
    WNK_TEST_HTTP_BASE   http://127.0.0.1:<port>
    WNK_TEST_HTTPS_BASE  https://localhost:<port>
    WNK_TEST_HTTPS_IP    https://127.0.0.1:<port>   (same server, name not in the certificate)
    WNK_TEST_CA          path of the CA certificate (PEM)
Exits with the test binary's exit code. Stdlib + openssl CLI only; nothing leaves the machine.
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

    def _route(self):
        url = urllib.parse.urlparse(self.path)
        q = urllib.parse.parse_qs(url.query)
        length = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(length) if length else b""
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
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    if shutil.which("openssl") is None:
        print("SKIP: openssl CLI not found", file=sys.stderr)
        return 0
    d = tempfile.mkdtemp(prefix="wnk-https-")
    try:
        ca, cert, key = make_certs(d)
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
                   WNK_TEST_CA=ca)
        return subprocess.run([sys.argv[1]], env=env).returncode
    finally:
        shutil.rmtree(d, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())

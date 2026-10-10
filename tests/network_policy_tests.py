"""Exercise real libcurl against local HTTP/SOCKS proxies; no external API requests."""
import http.server
import os
from pathlib import Path
import socketserver
import shutil
import ssl
import struct
import subprocess
import sys
import tempfile
import threading
import unittest

PROBE = sys.argv.pop(1)


class RoutingTests(unittest.TestCase):
    def setUp(self):
        self.destinations = []
        destinations = self.destinations

        class Proxy(http.server.BaseHTTPRequestHandler):
            def do_CONNECT(self):
                destinations.append(self.path)
                self.send_response(403)
                self.end_headers()

            def log_message(self, *args):
                pass

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Proxy)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.proxy = f"http://127.0.0.1:{self.server.server_port}"
        self.environment = {k: v for k, v in os.environ.items()
                            if k.lower() not in ("http_proxy", "https_proxy", "all_proxy", "no_proxy", "kufar_proxy",
                                                "kufar_proxy_pool", "kufar_proxy_pool_file")}
        # Unrelated requests must fail locally instead of reaching the public Internet.
        self.environment.update(http_proxy="http://127.0.0.1:1", https_proxy="http://127.0.0.1:1",
                                KUFAR_PROXY=self.proxy, KUFAR_PROXY_POOL="")

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)

    def run_probe(self, url, **settings):
        result = subprocess.run([PROBE, url], env=dict(self.environment, **settings),
                                capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        return result

    def test_both_search_apis_and_phone_use_explicit_proxy_even_with_no_proxy(self):
        for host, path in (("api.kufar.by", "/search-api/v2/search/rendered-paginated"),
                           ("searchapi.kufar.by", "/v1/search/rendered-paginated"),
                           ("api.kufar.by", "/search-api/v2/item/1/phone")):
            with self.subTest(host=host, path=path):
                self.destinations.clear()
                self.run_probe(f"https://{host}{path}", NO_PROXY="*")
                self.assertEqual(self.destinations, [host + ":443"])

    def test_telegram_and_lookalike_hosts_never_use_kufar_proxy(self):
        for url in ("https://api.telegram.org/botoffline/getUpdates",
                    "https://api.kufar.by.evil.invalid/search",
                    "https://api.kufar.by@evil.invalid/search",
                    "https://evil.invalid/api.kufar.by/search"):
            with self.subTest(url=url):
                self.destinations.clear()
                self.run_probe(url)
                self.assertEqual(self.destinations, [])

    def test_socks4a_routes_both_search_apis_and_phone_even_with_no_proxy(self):
        requests = []

        class SocksProxy(socketserver.BaseRequestHandler):
            def handle(self):
                self.request.settimeout(2)

                def read_exact(size):
                    data = b""
                    while len(data) < size:
                        part = self.request.recv(size - len(data))
                        if not part:
                            raise ConnectionError("Incomplete SOCKS request")
                        data += part
                    return data

                def read_string():
                    data = bytearray()
                    for _ in range(256):
                        value = read_exact(1)
                        if value == b"\0":
                            return bytes(data)
                        data.extend(value)
                    raise ValueError("SOCKS field too long")

                version, command, port, address = struct.unpack("!BBH4s", read_exact(8))
                read_string()  # SOCKS4 user ID
                hostname = read_string()
                requests.append((version, command, port, address, hostname))
                # Reject locally; never connect to Kufar or start a TLS exchange.
                self.request.sendall(b"\0\x5b\0\0\0\0\0\0")

        with socketserver.ThreadingTCPServer(("127.0.0.1", 0), SocksProxy) as server:
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            try:
                proxy = f"socks4a://127.0.0.1:{server.server_address[1]}"
                for host, path in (("api.kufar.by", "/search-api/v2/search/rendered-paginated"),
                                   ("searchapi.kufar.by", "/v1/search/rendered-paginated"),
                                   ("api.kufar.by", "/search-api/v2/item/1/phone")):
                    with self.subTest(host=host, path=path):
                        requests.clear()
                        self.run_probe(f"https://{host}{path}", KUFAR_PROXY=proxy, NO_PROXY="*")
                        self.assertEqual(requests, [(4, 1, 443, b"\0\0\0\1", host.encode("ascii"))])
            finally:
                server.shutdown()
                thread.join(timeout=2)

    def test_proxy_credentials_are_not_printed_on_failure(self):
        result = self.run_probe("https://api.kufar.by/search", KUFAR_PROXY=self.proxy.replace(
            "http://", "http://offline-user:private-test-secret@"))
        self.assertEqual(self.destinations, ["api.kufar.by:443"])
        self.assertNotIn("private-test-secret", result.stdout + result.stderr)

    def test_dead_primary_reaches_backup_and_exhaustion_is_a_proxy_error(self):
        result = self.run_probe("https://api.kufar.by/search", KUFAR_PROXY="http://127.0.0.1:1",
                                KUFAR_PROXY_POOL=self.proxy, NO_PROXY="*")
        self.assertEqual(self.destinations, ["api.kufar.by:443"])
        self.assertIn("ProxyError:", result.stderr)
        self.assertIn("route 1", result.stderr)
        self.assertIn("route 2", result.stderr)

    def test_every_backup_is_attempted_before_proxy_error(self):
        result = self.run_probe("https://api.kufar.by/search", KUFAR_PROXY="http://127.0.0.1:1",
                                KUFAR_PROXY_POOL=f"http://127.0.0.1:2;{self.proxy};http://127.0.0.1:3",
                                NO_PROXY="*")
        self.assertEqual(self.destinations, ["api.kufar.by:443"])
        self.assertIn("ProxyError:", result.stderr)
        for route in range(1, 5):
            self.assertIn(f"route {route}", result.stderr)

    def test_pool_deduplicates_primary_and_backups(self):
        self.run_probe("https://api.kufar.by/search", KUFAR_PROXY_POOL=f"{self.proxy};{self.proxy}")
        self.assertEqual(self.destinations, ["api.kufar.by:443"])

    def test_telegram_ignores_pool_even_when_invalid(self):
        self.run_probe("https://api.telegram.org/botoffline/getUpdates",
                       KUFAR_PROXY_POOL="invalid-private-test-secret", KUFAR_PROXY_POOL_FILE="missing-file")
        self.assertEqual(self.destinations, [])

    def test_working_reserve_is_reused_and_does_not_raise_proxy_error(self):
        destinations = []
        fixtures = Path(__file__).parent / "fixtures"
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(fixtures / "proxy-test-cert.pem", fixtures / "proxy-test-key.pem")

        class WorkingProxy(socketserver.BaseRequestHandler):
            def handle(self):
                self.request.settimeout(3)
                data = b""
                while not data.endswith(b"\r\n\r\n"):
                    part = self.request.recv(1)
                    if not part:
                        return
                    data += part
                destinations.append(data.split(b"\r\n")[0])
                self.request.sendall(b"HTTP/1.1 200 Connection established\r\n\r\n")
                with context.wrap_socket(self.request, server_side=True) as connection:
                    request = b""
                    while not request.endswith(b"\r\n\r\n"):
                        part = connection.recv(1)
                        if not part:
                            return
                        request += part
                    body = b'{"ads":[]}'
                    connection.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 10\r\nConnection: close\r\n\r\n" + body)

        with tempfile.TemporaryDirectory(prefix="kufar-proxy-tls-") as directory, \
                socketserver.ThreadingTCPServer(("127.0.0.1", 0), WorkingProxy) as server:
            # Some Windows TLS backends cannot read CA files from non-ASCII paths.
            certificate = Path(directory) / "test-ca.pem"
            shutil.copyfile(fixtures / "proxy-test-cert.pem", certificate)
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            try:
                backup = f"http://127.0.0.1:{server.server_address[1]}"
                result = subprocess.run([PROBE, "https://api.kufar.by/search", "https://searchapi.kufar.by/search"],
                    env=dict(self.environment, KUFAR_PROXY="http://127.0.0.1:1", KUFAR_PROXY_POOL=backup,
                             NO_PROXY="*", NETWORK_TEST_CA_FILE=str(certificate)),
                    capture_output=True, text=True, timeout=15)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(result.stdout.count('{"ads":[]}'), 2)
                self.assertEqual(len(destinations), 2)
                self.assertEqual(result.stderr.count("route 1"), 1, "dead primary should not be retried immediately")
                self.assertNotIn("ProxyError:", result.stderr)
            finally:
                server.shutdown()
                thread.join(timeout=2)

    def test_unreachable_proxy_is_a_connection_failure_only_for_kufar(self):
        for scheme in ("http", "socks4a", "socks5h"):
            with self.subTest(scheme=scheme):
                proxy = f"{scheme}://127.0.0.1:1"
                result = self.run_probe("https://api.kufar.by/search", KUFAR_PROXY=proxy, NO_PROXY="*")
                self.assertIn("Proxy connection failed:", result.stderr)
                result = self.run_probe("https://api.telegram.org/botoffline/getUpdates", KUFAR_PROXY=proxy)
                self.assertNotIn("Proxy connection failed:", result.stderr)

    def test_unset_proxy_keeps_existing_routing_and_invalid_value_is_not_echoed(self):
        self.run_probe("https://api.kufar.by/search", KUFAR_PROXY="")
        self.assertEqual(self.destinations, [])
        result = self.run_probe("https://api.kufar.by/search", KUFAR_PROXY="invalid-private-test-secret")
        self.assertIn("Invalid KUFAR_PROXY", result.stderr)
        self.assertNotIn("private-test-secret", result.stderr)
        self.assertEqual(self.destinations, [])


if __name__ == "__main__":
    unittest.main()

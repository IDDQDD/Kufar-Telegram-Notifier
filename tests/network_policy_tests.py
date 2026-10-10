"""Exercise real libcurl against local HTTP/SOCKS proxies; no external API requests."""
import http.server
import os
import socketserver
import struct
import subprocess
import sys
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
                            if k.lower() not in ("http_proxy", "https_proxy", "all_proxy", "no_proxy", "kufar_proxy")}
        # Unrelated requests must fail locally instead of reaching the public Internet.
        self.environment.update(http_proxy="http://127.0.0.1:1", https_proxy="http://127.0.0.1:1",
                                KUFAR_PROXY=self.proxy)

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)

    def run_probe(self, url, **settings):
        result = subprocess.run([PROBE, url], env=dict(self.environment, **settings),
                                capture_output=True, text=True, timeout=8)
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

    def test_unset_proxy_keeps_existing_routing_and_invalid_value_is_not_echoed(self):
        self.run_probe("https://api.kufar.by/search", KUFAR_PROXY="")
        self.assertEqual(self.destinations, [])
        result = self.run_probe("https://api.kufar.by/search", KUFAR_PROXY="invalid-private-test-secret")
        self.assertIn("Invalid KUFAR_PROXY", result.stderr)
        self.assertNotIn("private-test-secret", result.stderr)
        self.assertEqual(self.destinations, [])


if __name__ == "__main__":
    unittest.main()

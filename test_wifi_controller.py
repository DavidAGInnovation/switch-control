import hashlib
import hmac
import importlib.util
from pathlib import Path
import socket
import threading
import unittest

spec = importlib.util.spec_from_file_location('wifi_controller', Path(__file__).with_name('wifi_controller.py'))
wifi = importlib.util.module_from_spec(spec)
spec.loader.exec_module(wifi)


class WifiControllerTest(unittest.TestCase):
    def test_home_press_keeps_controller_present_around_system_event(self):
        self.assertEqual(wifi.wire_command('click HOME'), 'clickSeq W500,+HOME,W300,-HOME,W500')
        self.assertEqual(wifi.wire_command('click A'), 'click A')

    def test_fragmented_challenge_and_authenticated_request(self):
        key, nonce = bytes(range(32)), bytes(range(32, 64))
        server = socket.socket()
        server.bind(('127.0.0.1', 0))
        server.listen(1)
        port = server.getsockname()[1]
        observed = []

        def peer():
            with server:
                conn, _ = server.accept()
                with conn:
                    conn.sendall(nonce[:3])
                    conn.sendall(nonce[3:])
                    n = int.from_bytes(wifi.receive_exact(conn, 2), 'big')
                    payload = wifi.receive_exact(conn, n)
                    mac = wifi.receive_exact(conn, 32)
                    observed.append((payload, mac))
                    conn.sendall(b'OK\n')

        thread = threading.Thread(target=peer)
        thread.start()
        self.assertEqual(wifi.command('127.0.0.1', key, 'click A', port), b'OK\n')
        thread.join(timeout=5)
        self.assertFalse(thread.is_alive())
        payload, mac = observed[0]
        self.assertEqual(payload, b'click A')
        self.assertTrue(hmac.compare_digest(mac, hmac.digest(key, nonce + payload, hashlib.sha256)))
        self.assertFalse(hmac.compare_digest(mac, hmac.digest(key, nonce + b'click B', hashlib.sha256)))
        self.assertFalse(hmac.compare_digest(mac, hmac.digest(key, bytes(32) + payload, hashlib.sha256)))

    def test_rejects_invalid_requests_before_connecting(self):
        for key, value in ((b'x', 'click A'), (bytes(32), ''), (bytes(32), 'A\nB'), (bytes(32), 'x' * 256)):
            with self.subTest(value=value), self.assertRaises(ValueError):
                wifi.command('127.0.0.1', key, value, 1)


if __name__ == '__main__':
    unittest.main()

#!/usr/bin/env python3
"""TLS 1.3 PSK client for the Switch control service.

Requires Python 3.13+ built with OpenSSL PSK support. No plaintext fallback.
"""
import argparse
import hashlib
import hmac
from pathlib import Path
import socket
import ssl

from .wifi_controller import MAX_REPLY, receive_exact, wire_command

IDENTITY = 'za-wifi-tls-v1'


def tls_context(tls_key):
    if len(tls_key) != 32:
        raise ValueError('TLS PSK must contain exactly 32 bytes')
    if not getattr(ssl, 'HAS_PSK', False) or not hasattr(ssl.SSLContext, 'set_psk_client_callback'):
        raise RuntimeError('Python 3.13+ with OpenSSL TLS-PSK support is required')
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    # PSK, rather than a certificate, authenticates the server. The negotiated
    # PSK is checked explicitly below before any application data is sent.
    context.check_hostname = False
    context.verify_mode = ssl.CERT_NONE
    context.minimum_version = ssl.TLSVersion.TLSv1_3
    context.maximum_version = ssl.TLSVersion.TLSv1_3
    context.options |= ssl.OP_NO_TICKET
    context.set_psk_client_callback(lambda _hint: (IDENTITY, tls_key))
    return context


def command(host, auth_key, tls_key, text, port=6012, *, body=b''):
    if len(body)>65536:raise ValueError('Body exceeds chunk limit')
    if body and not text.startswith('fsChunk '):raise ValueError('Binary bodies require a file-chunk command')
    payload = wire_command(text).encode('ascii')
    if len(auth_key) != 32 or not 0 < len(payload) <= 2048 or any(c in payload for c in (0, 10, 13)):
        raise ValueError('Invalid authentication key or command')
    if hmac.compare_digest(auth_key, tls_key):
        raise ValueError('TLS and command authentication keys must be distinct')
    context = tls_context(tls_key)
    with socket.create_connection((host, port), timeout=10) as raw:
        with context.wrap_socket(raw, server_hostname=None, suppress_ragged_eofs=False) as sock:
            sock.settimeout(15)
            # OpenSSL marks external TLS 1.3 PSK use as session_reused. Without
            # this check CERT_NONE could accept an unrelated certificate server.
            if not sock.session_reused or sock.version() != 'TLSv1.3':
                raise ssl.SSLError('Server did not authenticate with the configured TLS PSK')
            nonce = receive_exact(sock, 32)
            mac = hmac.digest(auth_key, nonce + payload, hashlib.sha256)
            sock.sendall(len(payload).to_bytes(2, 'big') + payload + mac + body)
            result = bytearray()
            while True:
                chunk = sock.recv(65536)
                if not chunk:
                    break
                result.extend(chunk)
                if len(result) > MAX_REPLY:
                    raise ValueError('Reply exceeds limit')
            return bytes(result)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', required=True)
    parser.add_argument('--key-file', required=True, type=Path, help='existing command authentication key')
    parser.add_argument('--tls-key-file', required=True, type=Path, help='separate TLS PSK')
    parser.add_argument('--port', type=int, default=6012)
    parser.add_argument('--screen', type=Path)
    parser.add_argument('command', nargs='?', default='getReaderStatus')
    args = parser.parse_args()
    data = command(args.host, args.key_file.read_bytes(), args.tls_key_file.read_bytes(),
                   'pixelPeek' if args.screen else args.command, args.port)
    if args.screen:
        header, sep, jpeg = data.partition(b'\n')
        if not sep or not header.startswith(b'JPEG ') or int(header[5:]) != len(jpeg) or not jpeg.startswith(b'\xff\xd8') or not jpeg.endswith(b'\xff\xd9'):
            raise ValueError('Invalid screenshot response')
        args.screen.write_bytes(jpeg)
    else:
        print(data.decode('ascii'), end='')


if __name__ == '__main__':
    main()

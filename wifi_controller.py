#!/usr/bin/env python3
"""Authenticated client for the experimental resident Wi-Fi transport.

One connection carries one command. Authentication prevents unauthorised
commands and replay; this protocol does not encrypt the network traffic.
"""
import argparse
import hashlib
import hmac
from pathlib import Path
import socket

PORT = 6012
MAX_REPLY = 1024 * 1024


def wire_command(text):
    # HOME is handled by the system shell. On a newly attached controller, a
    # short press can be acknowledged by HID before the shell processes it.
    # Keep the controller connected around the press and release.
    if text == 'click HOME':
        return 'clickSeq W500,+HOME,W300,-HOME,W500'
    return text


def receive_exact(sock, length):
    result = bytearray()
    while len(result) < length:
        part = sock.recv(length - len(result))
        if not part:
            raise ConnectionError('Disconnected before challenge completed')
        result.extend(part)
    return bytes(result)


def command(host, key, text, port=PORT):
    payload = wire_command(text).encode('ascii')
    if len(key) != 32 or not 0 < len(payload) <= 255 or any(c in payload for c in (0, 10, 13)):
        raise ValueError('Invalid key or command')
    with socket.create_connection((host, port), timeout=10) as sock:
        sock.settimeout(15)
        nonce = receive_exact(sock, 32)
        mac = hmac.digest(key, nonce + payload, hashlib.sha256)
        sock.sendall(len(payload).to_bytes(2, 'big') + payload + mac)
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
    parser.add_argument('--key-file', required=True, type=Path)
    parser.add_argument('--port', type=int, default=PORT)
    parser.add_argument('--screen', type=Path)
    parser.add_argument('command', nargs='?', default='getReaderStatus')
    args = parser.parse_args()
    data = command(args.host, args.key_file.read_bytes(), 'pixelPeek' if args.screen else args.command, args.port)
    if args.screen:
        header, sep, jpeg = data.partition(b'\n')
        if not sep or not header.startswith(b'JPEG ') or int(header[5:]) != len(jpeg) or not jpeg.startswith(b'\xff\xd8') or not jpeg.endswith(b'\xff\xd9'):
            raise ValueError('Invalid screenshot response')
        args.screen.write_bytes(jpeg)
    else:
        print(data.decode('ascii'), end='')


if __name__ == '__main__':
    main()

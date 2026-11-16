# Security

Use Switch Control on a trusted local network. Do not forward its ports to the
internet. It runs with substantial console permissions; any authenticated client
can control input, read permitted SD files and change supported settings.

The native API uses a per-device certificate and bearer token. Check the physical
setup screen's certificate fingerprint before trusting a certificate obtained
from the network. The token is a full-access credential. The PSK interface uses
TLS 1.3 plus a separate command-authentication key. There is no plaintext fallback
on these endpoints. The old `switch_control/wifi_controller.py` is retained as a framing library
and legacy development client; do not use its plaintext transport.

Keep `/config/za-usb-control/` and local client profiles private. They contain keys
and pairing material. Do not attach setup screenshots showing a token, network
profiles, certificates with private keys, memory dumps or full SD backups to
issues. Revoke compromised pairing by powering off, replacing both binary keys
with independent random 32-byte values, then re-pairing clients; a changed
certificate must be verified again. Replacing keys revokes the bearer token but
does not require deleting the device's certificate.

For a security issue, use this repository's private GitHub vulnerability reporting
feature rather than posting credentials or an exploit in a public issue. Reports
should describe the affected release, request and expected/observed behavior,
with all private values redacted.

# Contributing

Open an issue describing the behavior before proposing a large change. Include
the tool version, firmware/Atmosphère versions and a minimal reproduction, with
private information removed.

Run `scripts/check.py` with a host-built Mbed TLS tree and build the Switch package
before submitting changes. Tests should cover behavior and failure paths, not
merely repeat implementation details. Keep the native MCP schema synchronized
with `control.py` using `scripts/generate_schemas.py`.

Contributions use GPL-3.0-only with the libnx linking permission in
`LICENSE-EXCEPTION`, with third-party notices preserved. Do not add
Nintendo SDK material, keys, game data, console backups or incompatible copied
code. Keep generated build products and device-specific evidence out of Git.

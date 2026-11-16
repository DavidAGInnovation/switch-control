# Switch Control

Control an Atmosphère Nintendo Switch over your local network. HTTPS REST and
Streamable HTTP MCP run directly on the console: no PC bridge or cloud service
is needed. A Python command-line client is included.

[Download v0.5.0](https://github.com/DavidAGInnovation/switch-control/releases/tag/v0.5.0)
· [Installation and API](docs/RELEASE.md) · [License notices](docs/THIRD_PARTY.md)

## Features

- Screenshots, button chords, analog sticks and sequences of up to 32 steps.
- Leased input with ownership tokens, renewal, cancellation and automatic release.
- SD directory browsing, streaming transfers, SHA-256 verification, guarded
  replacement, cancellable verification jobs and log tailing.
- Battery, charging, brightness, volume, theme, nickname and installed-title status.
- Saved-profile DNS configuration, manual date/time and automatic clock sync.
- DNS-SD discovery and native REST/MCP over certificate TLS; the companion PSK
  interface uses TLS 1.3 and a separate command-authentication key.
- Sleep/wake cleanup of input and partial transfers, then listener/discovery recovery.

## Install

Requires an Atmosphère-capable Switch, an SD card and a reachable IPv4 LAN.
Custom firmware installation is outside this project's scope.

1. Download the release ZIP and copy the **contents of `sd/`** to the SD card.
2. Temporarily put `optional/switch-control-setup.nro` in `/switch/`.
3. Boot Atmosphère, open the helper in hbmenu and press **A** to provision keys
   and start the service. Existing valid keys are preserved.
4. The helper shows the hostname and certificate fingerprint. Press **X** to
   display the private bearer token. Trust the certificate only after checking
   its SHA-256 fingerprint, and configure your REST/MCP client with that token.
5. Exit with **PLUS**. You can remove the helper from `/switch/`; the service
   stays in the background. There is no browser remote at the root URL.

REST uses `https://switch-<device-id>.local:7443/status`; MCP uses the same host
and port with `/mcp`. All commands require `Authorization: Bearer <token>`.
The public `/certificate` endpoint supplies the certificate for fingerprint
verification. Clients must support local-network access and the device's
self-signed certificate. Never disable certificate verification as a workaround.

The service cannot wake a sleeping console. Keep credentials private and use it
on a trusted local network. See [security and credential handling](SECURITY.md).

## Optional Python client

No third-party Python packages are required. Authenticated certificate bootstrap
and the PSK interface require **Python 3.13+ with OpenSSL PSK support**.
Copy the two provisioned 32-byte binary keys from the SD card into a private
local directory, then run from this repository:

```sh
python3 -m switch_control configure --host auto \
  --key-file /private/path/wifi.key --tls-key-file /private/path/tls.key /private/path/profile
python3 -m switch_control --config /private/path/profile/config.json status
python3 -m switch_control --config /private/path/profile/config.json call tap_buttons '{"buttons":["A"]}'
python3 -m switch_control --config /private/path/profile/config.json screenshot /private/path/screen.jpg
```

Use an explicit IP instead of `auto` when multicast discovery is unavailable.
The profile stores a trusted certificate and token file paths. Do not commit it.
`GET /tools` and MCP `tools/list` describe all 33 typed tools.

## Build and test

The service and setup helper build with Docker using the pinned
`devkitpro/devkita64:20260219` toolchain and Mbed TLS 3.6.7 commit
`068ff080b369adfac81509f9b57b2afabaf82dc5`.
Run from a fresh checkout with Docker running:

```sh
sh firmware/usb-resident/prepare_mbedtls.sh /absolute/new/mbedtls-switch
python3 scripts/assemble_release.py /absolute/new/release --mbedtls-root /absolute/new/mbedtls-switch
```

Output paths must not exist. The ZIP contains SD files, the optional helper,
project source, client scripts, license notices, build metadata and a SHA-256
manifest. No provisioned keys or certificates are bundled. The matching Mbed TLS
source is also published as a separate release asset; see [THIRD_PARTY.md](docs/THIRD_PARTY.md).

To run the full host suite (C compiler, OpenSSL development headers, pkg-config,
Git, Make and a PSK-capable Python are required):

```sh
sh scripts/prepare_host_mbedtls.sh /absolute/new/mbedtls-host
ZA_MBEDTLS_HOST_ROOT=/absolute/new/mbedtls-host python3 scripts/check.py
```

CI runs the host tests with no skipped tests and builds the Switch package.
The generated C tool schemas are checked against `switch_control/control.py`.

## Tested scope and limits

The service has been tested on one physical Switch running HOS 23.0.1. Live
checks covered HTTPS/MCP, discovery, controller cleanup and leases, transfers
above 64 MiB, cancellation, sleep/wake recovery, system settings, automatic DNS
profile writes and clock writes with synchronization restored afterward. Manual
DNS address changes are covered by host tests. Reboot startup for v0.5.0, other
console/firmware combinations, a second physical client and the full 8 GiB file
ceiling have not yet been exercised.

Files use bounded chunks and cooperative verification. The 8 GiB ceiling is
subject to filesystem limits (FAT32 limits individual files to less than 4 GiB).
Replacement is **not atomic across power loss**: retain hidden transfer/recovery
files for inspection if interrupted. The API protects its credentials and
service files, denies Nintendo-directory mutations, and requires games closed
for Atmosphère writes. It does not mount NAND or save partitions.

## Project layout

| Directory | Contents |
| --- | --- |
| `firmware/` | Resident service, read-only reader, setup helper and maintainer updater. |
| `switch_control/` | Python client package; run the HTTPS CLI with `python3 -m switch_control`. |
| `tests/` | Host unit and integration tests. |
| `scripts/` | Build, packaging, schema generation and verification tools. |
| `docs/` | Detailed installation/API documentation and dependency license review. |
| `licenses/` | Third-party license texts and attribution notices. |

The read-only reader provides process metadata, memory mappings and bounded
byte reads through Atmosphère's selected process. It uses no game-specific
layouts, addresses or version checks. See the
[reader details](docs/RELEASE.md#memory-diagnostics) before using foreground
attachment. Credential paths and the PSK identity retain their historical names
for compatibility with paired consoles.

`firmware/file-update/` and `switch_control/install_update.py` retain the guarded
updater's source for maintainers. They require an explicitly reviewed prior
executable fingerprint and matching recovery app. The public package uses SD
installation; it contains no updater pinned to a private development build.

Project code is GPL-3.0-only with the additional libnx linking permission in
[LICENSE-EXCEPTION](LICENSE-EXCEPTION). Linked libraries retain their own licenses
and attributions. This project is independent of Nintendo and Atmosphère.

This product includes software developed by Jonathan Stone and Jason R. Thorpe
for the NetBSD Project.

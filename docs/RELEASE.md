# Switch Control 0.5.0 — installation and API

The service runs directly on the Switch. Start with the installation steps in
[README.md](../README.md); the public release uses SD-card installation.
The service ID is `430000000000000D` and HTTPS listens on port 7443.
DNS-SD advertises `_switch-control._tcp.local` and the `/mcp` endpoint.
The TLS-PSK compatibility interface listens on port 6012.

## Credentials and pairing

The optional setup helper provisions independent random 32-byte binary keys at
`/config/za-usb-control/wifi.key` and `tls.key`. These historical names are retained
for compatibility. The bearer token is the lowercase hexadecimal encoding of
`wifi.key`; it grants access to all authenticated operations. The device creates
its certificate, private key and random discovery identity locally.

To provision manually, securely generate two independent 32-byte binary files at
those paths before booting. Do not use text passwords or reuse the same key.
The helper's A button restarts the service after provisioning; X shows/hides the
bearer token. Do not share screenshots while the token is visible.

Verify the device certificate's SHA-256 fingerprint against the physical setup
screen before trusting it. For example, save the public PEM from `/certificate`
and inspect its DER fingerprint with `openssl x509 -in switch-cert.pem -noout
-fingerprint -sha256`. Fingerprint-checked TLS remains required for subsequent
requests. The Python client's configure command bootstraps trust over the
already authenticated PSK channel instead.

MCP clients use Streamable HTTP with the URL ending `/mcp`, the trusted device
certificate and an `Authorization: Bearer <token>` header. Local certificate and
LAN support depend on the client. Tools list schemas are available via `/tools`.
No OAuth server, browser page or internet relay is provided.

To update an existing installation, close the game, power off fully, back up
the existing service executable, replace the two files under `sd/`, then boot
Atmosphère. Preserve `/config/za-usb-control/` to retain pairing. To uninstall,
power off and remove `/atmosphere/contents/430000000000000D/`; remove the setup
NRO and credentials separately if no longer wanted. Remove stale keys from clients.
Do not run multiple versions of the controller service simultaneously.

## Endpoints

- `GET /status`, `GET /screenshot`, `GET /tools`.
- `POST /input/tap`, `/input/sequence`, `/input/stick`, `/input/clear`.
- `GET /settings`; `POST /settings` with `name` and `value`: brightness, volume,
  automatic brightness, theme or nickname. Theme visibility may wait for HOME
  to reload. Individual status failures are reported as null plus native codes.
- `GET /directory?path=/switch&cursor=0`, `POST /directory?path=...`.
- `GET /stat?path=...`, `GET /hash?path=...`.
- `GET /files?path=...&offset=0&size=65536`, `PUT /files?path=...` (one chunk),
  `DELETE /files?path=...&expected_sha256=...`.
- For larger uploads: `POST /uploads` with path, size, sha256 and optional
  expected_sha256; `PUT /uploads/<id>?offset=...` for binary chunks; then
  `POST /uploads/<id>/commit`, or `DELETE /uploads/<id>` to abort.
- `POST /tools/<tool-name>` exposes the same typed operations as MCP.
- `POST /mcp` supports initialize, ping, tools/list and tools/call. Notifications
  receive 202; optional SSE and session DELETE are declined with 405.

HTTP bodies and transfer chunks are bounded to 64 KiB; verified files are
limited to 8 GiB. MCP write_file accepts base64 up to 48 KiB of decoded bytes.
Uploads default to creating a new file; replacements and deletions check the
expected old SHA-256. System and input parameters are validated. Timed analog
input recenters and detaches afterward, with positive Y pointing up.

Credentials and the running service are protected from SD mutations. Changes
under `/atmosphere/` require the game closed. No NAND/save partitions are mounted.
File publication is not power-loss atomic; retain and inspect recovery files
if power is lost between renames. Directory pages can change while listed.

## Background jobs

The browser remote stays removed. No setup app is installed by the SD tree.

- `tap_sequence` and `run_input` accept up to 32 steps and 120 seconds total.
  Each step combines a button chord and both analog sticks, with 1..10000 ms
  duration and 0..10000 ms wait. `run_input` is also `POST /input/run`.
- Jobs return a random `action` ownership token immediately. Poll with
  `input_status` (`POST /input/status`), cancel with `cancel_input`
  (`POST /input/cancel`), and renew with `renew_input` (`POST /input/renew`).
  Renew within ten seconds for longer sequences. Stale tokens cannot restart
  completed or expired input. The optional client's `wait_input` renews while
  waiting, reports progress and cancels on interruption.
- `hold_input` (`POST /input/hold`) requires `lease_ms` between 100 and 10000.
  Renewal requires that action's token. A lost client expires automatically;
  completed, cancelled, expired, sleep and network transitions release HID.
  `clear_input` is an authenticated emergency stop shared by clients.
- Timed `tap_buttons` and `set_stick` keep their previous synchronous behavior
  up to one second. Longer inputs return an asynchronous action token.
- Files are transferred in 64 KiB chunks with an 8 GiB verified-file ceiling,
  subject to the SD filesystem's limits. Hashes and commit verification above
  1 MiB run cooperatively: responses include a random `job` token. Poll
  `GET /file-jobs/<job>` or cancel with DELETE. MCP equivalents are
  `file_job_status`, `cancel_file_job`, and `start_hash`. Inspect upload bytes
  and verification state with `GET /uploads/<upload>`; DELETE aborts uploads.
- Staged replacement, expected-hash checks, publication verification and prior
  file restoration remain in place. Job cancellation during verification can
  restore the prior file. A completed publication cannot be undone by cancelling
  its old job token. Power loss is still not atomic.
- `GET /files?path=...&tail=4096` and MCP `tail_file` read the end of a log.
  NativeClient upload/download helpers stream with verification, progress
  callbacks and cancellation callbacks; keyboard interruption aborts work.
- `GET /titles?cursor=0` / `list_installed_titles` page through installed
  applications (16 records per page), with title ID, name, storage and version.
  Pages can change while installation records change.
- `GET /network`, `/network/dns` / `network_status` report effective IP, mask,
  gateway, DNS and connection status; saved Wi-Fi credentials are never returned.
- `GET /lifecycle` / `lifecycle_status` report PSC registration, sleep/wake
  counters and cleanup state. Leased input is unavailable if power registration
  fails. Sleep cancels input and unfinished transfers before filesystem sleep;
  wake/network changes rebuild both listeners without replaying commands.

The optional Python client's `put` and `get` commands use the native API; add
`--progress` for byte counts on stderr, and press Ctrl-C to cancel.
All added features run on the Switch; no host bridge is introduced.

HTTPS connections can carry consecutive requests, avoiding a TLS handshake for
every chunk. Authentication is checked on every request. The server bounds
native connections to four and idle/request time to five seconds; the optional
client starts a fresh connection after three seconds of local inactivity.
Failures propagate without replaying writes. Call `NativeClient.close()` when
finished. The title inventory marks malformed/unavailable names with
`name_available: false` while preserving the title ID and storage metadata.

## DNS and clock controls

- `POST /network/dns` / MCP `set_dns` accepts `{"automatic":true}` to restore
  automatic DNS, or `{"automatic":false,"primary":"1.1.1.1","secondary":"9.9.9.9"}`
  for manual IPv4 DNS. Secondary defaults to `0.0.0.0` (unused). Only the DNS
  fields of the currently selected saved profile are changed; Wi-Fi passwords,
  IP settings and proxy settings are preserved and never returned.
- DNS writes are delayed 250 ms to acknowledge the request first. They return
  an operation ID and `scheduled` state. Read `GET /network/dns/update` or MCP
  `dns_update_status` after reconnecting to inspect `pending`, `state` and the
  native `result`. Profile/DNS changes during scheduling cancel a stale write.
  Sleep cancels pending writes. A successful profile rewrite triggers one
  listener/discovery rebuild; requests are never replayed automatically.
- `GET /network/dns` / `network_status` includes effective DNS plus the saved
  `configured_primary_dns` and `configured_secondary_dns` values.
- `GET /settings/datetime` / `get_datetime` reports local calendar fields,
  UTC `timestamp`, timezone name, UTC offset, effective `automatic_sync` and
  `saved_automatic_sync`. The timezone is read-only.
- `POST /settings/auto-time` / `set_auto_time` with `{"enabled":true|false}`
  updates both the runtime clock-correction setting and its saved preference.
  `GET /settings/auto-time` / `get_auto_time` reports their current values.
- `POST /settings/datetime` / `set_datetime` accepts either a UTC `timestamp`
  (Unix seconds, 2000..2099) or all six local fields: year, month, day, hour,
  minute and second. Local fields use the console's current timezone. Invalid
  dates and DST gaps are rejected; ambiguous DST times require a UTC timestamp.
  Manual time setting disables automatic synchronization so the value can
  persist. If a clock write fails, the prior time and sync settings are restored
  where the native services permit, and rollback results are reported.

These controls are available through the authenticated background service.
No browser controls or Homebrew setup entry are reinstalled.

## Memory diagnostics

These commands use the PSK interface and the debugger already owned by
Atmosphère's cheat manager. Generic operations report title/process/build IDs,
main and heap extents, memory mappings and bounded reads. They do not write game
memory, choose arbitrary processes or implicitly attach on a read.

`openForeground` explicitly asks Atmosphère to attach to the foreground game
when no process is selected. This can load that game's configured cheats and
activate them according to Atmosphère settings, so it is not a passive query.

Commands: `openForeground`, `getProcessID`, `getTitleID`, `getBuildID` (eight
bytes), `getFullBuildID`, `getMainNsoBase`, `getMainNsoSize`, `getHeapBase`,
`getHeapSize`, `queryMemory ADDRESS`, `peekMain OFFSET SIZE`, and
`peekAbsolute ADDRESS SIZE`. Addresses/offsets are hexadecimal and byte counts
are decimal, limited to 65536. The reader does not scan game-specific objects or
interpret game data structures. Its commands use the selected process's reported
metadata and mappings, without title or build restrictions. See
`firmware/reader/source/main.c` for the command dispatcher.

## Maintainer updater source

The public ZIP deliberately contains no prebuilt updater or legacy recovery app.
The source-only `firmware/file-update/prepare.py` takes a new executable, a separately
reviewed recovery NRO, an output directory, `--version` and `--old-sha256`.
Its old-service fingerprint, backup directory and recovery program must match
the intended installation. `switch_control/install_update.py` accepts only bundles containing
an explicit updater path. The updater health-check/commit channel is temporary;
ordinary installation needs only the SD instructions above.

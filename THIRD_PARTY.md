# Licensing and dependency review

The Switch Control project source is distributed under **GPL-3.0-only with a libnx linking exception**, retaining
the GPLv3 license from the original controller module. The full text is in
[LICENSE](LICENSE); the additional permission is in
[LICENSE-EXCEPTION](LICENSE-EXCEPTION). This standalone repository starts with a clean history; its
controller and read-only reader originated in the same project's private SD
maintenance workspace. No console backup history, Nintendo SDK source, game
assets, keys or provisioned certificates are included.

The service and helper link the following dependencies. Their licenses and
copyright notices are preserved in `licenses/` and in every release ZIP.

| Component | Pinned version/source | License treatment |
| --- | --- | --- |
| Mbed TLS | 3.6.7, `068ff080b369adfac81509f9b57b2afabaf82dc5` | Upstream offers Apache-2.0 OR GPL-2.0-or-later; Apache-2.0 is selected for this distribution. Full dual-license notice retained. |
| libnx | 4.12.0, `7644c9b26099aa2d2145bc72a21ee24190e92085` | ISC, with BSD-3-Clause, BSD-4-Clause and ISC notices in imported networking headers; full notices and the required acknowledgment retained. |
| newlib / libm | devkitA64 package 4.6.0.20260123-4 | Collection of permissive notices; full `COPYING.NEWLIB` retained. Source files can carry additional notices. |
| GCC runtime | devkitA64 GCC 15.2.0-7 | GPLv3 with GCC Runtime Library Exception 3.1; both GPLv3 and the exception are supplied. |

The toolchain is `devkitpro/devkita64:20260219`, OCI index digest
`sha256:1fc388c3a0d34bd2045a6dadcb1020e069d5f876a187fd705de14b4440c00282`.
Its compiler, build tools and Docker image are build dependencies, not bundled
as executables in the release. Host tests use the platform C compiler and
OpenSSL; those host libraries are not linked into the Switch release.

The imported `net/if_media.h` has a BSD-4-Clause advertising acknowledgment.
Rather than treat every libnx file as ISC, this project grants a specific
GPLv3 section 7 linking permission for libnx and preserves the BSD terms.
The permission requires the corresponding libnx source with binaries; the
release supplies that pinned source archive. Mbed TLS is used under Apache-2.0,
with its notices and configuration modification notice retained.

This product includes software developed by Jonathan Stone and Jason R. Thorpe
for the NetBSD Project.

## Corresponding source and modifications

Project source is included in the SD release ZIP and the tagged GitHub source
archives. The full pinned libnx tree is supplied as
`switch-control-0.5.0-libnx-source.tar.gz`; additional file notices are collected
in `licenses/libnx-NOTICES`. The release also provides `switch-control-0.5.0-mbedtls-source.tar.gz`
with the exact tracked Mbed TLS and framework files used for the build, including
the modified configuration header. `scripts/dependency_source.py` recreates this
archive from the prepared tree. No compiled library objects or Git metadata are
included in that source archive.

`usb-resident/prepare_mbedtls.sh` makes these Mbed TLS configuration changes:
disable `MBEDTLS_NET_C` and `MBEDTLS_TIMING_C`; enable
`MBEDTLS_NO_PLATFORM_ENTROPY`, `MBEDTLS_ENTROPY_HARDWARE_ALT` and
`MBEDTLS_PLATFORM_MS_TIME_ALT`. The service supplies platform entropy/time and
network callbacks. The upstream library implementation is otherwise unchanged.
The configuration modification notice is added by the preparation script.

Upstream source and notices:

- [Mbed TLS at the pinned commit](https://github.com/Mbed-TLS/mbedtls/tree/068ff080b369adfac81509f9b57b2afabaf82dc5), including framework submodule `dde0c4a0e448a0552f18817dcea633bb851fd288`.
- [libnx v4.12.0](https://github.com/switchbrew/libnx/tree/v4.12.0) and its [ISC notice](licenses/libnx-LICENSE).
- [devkitPro newlib](https://github.com/devkitPro/newlib) and [collected notices](licenses/newlib-COPYING.NEWLIB), retained from devkitPro source commit `c7dbfcd15c6992cede5376af15d64afce2c6fdb1`.
- [GCC 15.2.0](https://gcc.gnu.org/releases.html) and [runtime exception](licenses/GCC-Runtime-Library-Exception).
- [devkitPro toolchain build recipes](https://github.com/devkitPro/buildscripts).

The sys-autopilot project was consulted as a feature/API reference. It is not a
linked or vendored dependency. Third-party code added later must retain its own
attribution and be compatible with the project's license.

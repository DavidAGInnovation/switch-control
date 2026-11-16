#!/bin/sh
# Prepare the pinned TLS dependency outside the repository; do not run on the Switch.
set -eu

if [ "$#" -ne 1 ]; then
    printf '%s\n' 'Usage: prepare_mbedtls.sh /absolute/new/directory' >&2
    exit 2
fi
case "$1" in /*) ;; *) printf '%s\n' 'Destination must be absolute.' >&2; exit 2;; esac
if [ -e "$1" ]; then
    printf '%s\n' 'Destination already exists; refusing to overwrite it.' >&2
    exit 1
fi
git clone --depth 1 --branch v3.6.7 --recurse-submodules --shallow-submodules \
    https://github.com/Mbed-TLS/mbedtls.git "$1"
actual=$(git -C "$1" rev-parse HEAD)
if [ "$actual" != 068ff080b369adfac81509f9b57b2afabaf82dc5 ]; then
    printf '%s\n' 'Unexpected Mbed TLS commit; aborting.' >&2
    exit 1
fi
cd "$1"
python3 -c 'from pathlib import Path; p=Path("include/mbedtls/mbedtls_config.h"); p.write_text("/* Modified for Switch Control: platform entropy/time and custom socket I/O. */\n"+p.read_text())'
python3 scripts/config.py unset MBEDTLS_NET_C
python3 scripts/config.py unset MBEDTLS_TIMING_C
python3 scripts/config.py set MBEDTLS_NO_PLATFORM_ENTROPY
python3 scripts/config.py set MBEDTLS_ENTROPY_HARDWARE_ALT
python3 scripts/config.py set MBEDTLS_PLATFORM_MS_TIME_ALT
docker run --rm -v "$1:/work" -w /work devkitpro/devkita64:20260219@sha256:1fc388c3a0d34bd2045a6dadcb1020e069d5f876a187fd705de14b4440c00282 \
    make -C library -j4 \
    CC=/opt/devkitpro/devkitA64/bin/aarch64-none-elf-gcc \
    AR=/opt/devkitpro/devkitA64/bin/aarch64-none-elf-ar \
    'CFLAGS=-O2 -march=armv8-a -mtune=cortex-a57 -mtp=soft -fPIE'

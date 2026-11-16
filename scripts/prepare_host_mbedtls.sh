#!/bin/sh
# Build the unmodified pinned dependency for host TLS integration tests.
set -eu
[ "$#" -eq 1 ] || { echo 'Usage: prepare_host_mbedtls.sh /absolute/new/directory' >&2; exit 2; }
case "$1" in /*) ;; *) echo 'Destination must be absolute.' >&2; exit 2;; esac
[ ! -e "$1" ] || { echo 'Destination exists.' >&2; exit 1; }
git clone --depth 1 --branch v3.6.7 --recurse-submodules --shallow-submodules \
  https://github.com/Mbed-TLS/mbedtls.git "$1"
[ "$(git -C "$1" rev-parse HEAD)" = 068ff080b369adfac81509f9b57b2afabaf82dc5 ] || exit 1
make -C "$1/library" -j4

#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

if ! command -v phpize &>/dev/null; then
    echo "ERROR: phpize not found. Install php-dev / php-devel package." >&2
    exit 1
fi

# Clean any previous build artifacts
if [ -f Makefile ]; then
    make distclean 2>/dev/null || true
fi
[ -d modules ] && rm -rf modules
[ -f configure ] && rm -f configure

phpize
./configure --enable-nxs
make -j"$(nproc 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || echo 4)"

echo ""
echo "Built: modules/nxs.so"

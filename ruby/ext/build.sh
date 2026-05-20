#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/nxs"
ruby extconf.rb
make -j$(nproc 2>/dev/null || sysctl -n hw.logicalcpu)
echo "Built: nxs_ext.bundle (or .so)"

#!/usr/bin/env bash
# Build the _nxs C extension without setuptools.
set -euo pipefail

cd "$(dirname "$0")"

PYINCLUDE=$(python3 -c "import sysconfig; print(sysconfig.get_path('include'))")
EXT_SUFFIX=$(python3 -c "import sysconfig; print(sysconfig.get_config_var('EXT_SUFFIX'))")

OUT="_nxs${EXT_SUFFIX}"

echo "Building $OUT"
echo "  Python include: $PYINCLUDE"

# -undefined dynamic_lookup is macOS-only; on Linux use python3-config --ldflags
if [[ "$(uname)" == "Darwin" ]]; then
  LDFLAGS="-undefined dynamic_lookup"
else
  LDFLAGS=$(python3-config --ldflags --embed 2>/dev/null || python3-config --ldflags)
fi

cc -O3 -Wall -Wextra -fPIC \
   -I"$PYINCLUDE" \
   -shared \
   $LDFLAGS \
   _nxs.c \
   -o "$OUT"

echo "Built $OUT"
